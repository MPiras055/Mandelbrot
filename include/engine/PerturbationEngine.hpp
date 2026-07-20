#pragma once
#include <atomic>
#include <cassert>
#include <cstdio>
#include "core/Numeric.hpp"
#include "core/Kernel.hpp"
#include "util/ColorUtil.hpp"
#include "job/RenderJob.hpp"
#include "util/LazyVector.hpp"
#include "util/RebaseMatrix.hpp"

namespace engine {

using BigFloat = core::BigFloat;
using ComplexDouble = core::ComplexDouble;

/**
 * @brief High-zoom rendering strategy: perturbation theory + Taylor-series orbits.
 *
 * @details The nested data structures and their (short, tightly-coupled) methods
 * stay inline here; the four large driver methods — getWorkerData,
 * processChunkScalar, getProbeWinnerDelta and processPerturbationJob — live in
 * PerturbationEngine.cpp. (This engine is scheduled for a fuller rewrite; the
 * split keeps the header readable without over-investing in out-of-lining every
 * nested method.)
 */
class PerturbationEngine {
    static constexpr unsigned int TILE_SIZE = core::CHUNK_BLOCK;
    //initialize matrix dimensions
    using RebaseMatrix = RebaseMatrix__<64>;      // wide grid for the parallel high-res search
    using RebaseMatrixLow = RebaseMatrix__<12>;    // small grid for the solo low-res probe (64 cells)
    static constexpr unsigned int MAX_WORKERS = 128;
    static constexpr double REBASE_SHRINK_FACTOR = 0.25;
    // The low-res probe only needs to RANK cells by depth, so cap its per-cell iterations
    // well below the render budget (the reference orbit itself is still built to full depth).
    static constexpr unsigned int LOW_RES_PROBE_CAP = 256;
    core::Pixel*& back_buf_ref;
    std::array<RebaseMatrix*,MAX_WORKERS> workerLocalPtr;
    std::atomic<uint32_t> nextWorkerLocal{0};

    RebaseMatrix* getLocalMatrix() {
        static thread_local RebaseMatrix* ptr = [this]() {
            const size_t my_idx = nextWorkerLocal.fetch_add(1ul, std::memory_order_relaxed);
            assert(my_idx < MAX_WORKERS && "Exceeded MAX_WORKERS allocation limits!");
            RebaseMatrix* allocated = new RebaseMatrix();
            //register for deallocation
            workerLocalPtr[my_idx] = allocated;
            return allocated;
        }();
        return ptr;
    }

    // Per-worker pool for the small low-res probe grid (shared cooperatively via the
    // job's rebaseMatrix CAS, like getLocalMatrix()).
    std::array<RebaseMatrixLow*,MAX_WORKERS> workerLocalPtrLow;
    std::atomic<uint32_t> nextWorkerLocalLow{0};

    RebaseMatrixLow* getLocalMatrixLow() {
        static thread_local RebaseMatrixLow* ptr = [this]() {
            const size_t my_idx = nextWorkerLocalLow.fetch_add(1ul, std::memory_order_relaxed);
            assert(my_idx < MAX_WORKERS && "Exceeded MAX_WORKERS allocation limits!");
            RebaseMatrixLow* allocated = new RebaseMatrixLow();
            workerLocalPtrLow[my_idx] = allocated;
            return allocated;
        }();
        return ptr;
    }

    
    

    /**
     * @brief: record struct that is used to store the final orbit
     * information
     * @note: for this implementation we're thinkering with the Taylor
     * series approximation of the Mandelbrot function
     */
    struct OrbitRecord {
        ComplexDouble center;   // Z_i of the reference orbit (double)
    };

    /**
     * @brief One bilinear-approximation step (Zhuoran BLA): maps the perturbation delta
     * across a run of 2^level iterations as  δ' = A·δ + B·dc,  valid while |δ|² < r2.
     * A and B are complex (stored as component pairs to avoid std::complex overhead in
     * the hot render walk).
     */
    struct BLA {
        double Ar, Ai;   // A (complex)
        double Br, Bi;   // B (complex)
        double r2;       // squared validity radius
    };

    /**
     * @brief Engine-owned persistent reference cache.
     * @details Survives across frames so panning within a reference's validity skips
     * the whole probe + reference-orbit rebuild. Double-buffered (two slots + an
     * atomic active pointer): a rebuild always writes the NON-active slot and then
     * publishes it, so a straggler from a previous frame still reading the old slot
     * is never corrupted (the previous slot's readers are gone by the time it is
     * rebuilt into again — same ping-pong argument as DoubleCanvas).
     */
    struct ReferenceCache {
        util::LazyVector<OrbitRecord> orbit;   // Z_i at the reference point
        // BLA merge tree: blaLevels[l][j] covers iterations [j·2^l, (j+1)·2^l).
        std::vector<std::vector<BLA>> blaLevels;
        double dc_max{0.0};                    // max |pixel − reference| over the view (radii scale)
        size_t orbitLen{0};                    // == orbit.size(), cached for the render walk
        std::complex<core::BigFloat> reference;     // absolute reference (center + probe delta)
        unsigned int iterations{0};            // orbit depth this cache was built to
        bool valid{false};

        /// @return the actual orbit length (== max_iterations if the reference never
        /// escaped). Polls @p job so a long BigFloat build aborts promptly when the
        /// frame is superseded; on abort it returns early with the cache left invalid
        /// (the caller checks and does not publish it).
        size_t build(const job::RenderJob& job, std::complex<core::BigFloat> ref,
                     size_t max_iterations, double dc_max_in) {
            this->iterations = max_iterations;
            this->dc_max = dc_max_in;
            reference = ref;
            
            // orbit: reset size to 0 (keeps capacity); records are appended with
            // push_back below so orbit.size() reflects the true orbit length.
            orbit.init(max_iterations);

            // Standard BigFloat Z tracking for the absolute reference orbit.
            core::BigFloat z_r = 0.0;
            core::BigFloat z_i = 0.0;

            size_t actual_length = 0;

            for (size_t i = 0; i < max_iterations; i++) {
                // Poll abort off the hot path so a superseded frame drops its build fast.
                if ((i & 0xFF) == 0 && job.aborted()) return actual_length;

                // Store Z_i (cast to double for the fast per-pixel loop + BLA table).
                orbit.push_back(OrbitRecord{
                    ComplexDouble{ static_cast<double>(z_r), static_cast<double>(z_i) }});

                // Step the reference orbit forward in BigFloat.
                core::BigFloat next_z_r = z_r * z_r - z_i * z_i + reference.real();
                core::BigFloat next_z_i = core::BigFloat(2.0) * z_r * z_i + reference.imag();

                z_r = next_z_r;
                z_i = next_z_i;
                actual_length++;

                // If the reference orbit escapes, stop building early.
                if (static_cast<double>(z_r * z_r + z_i * z_i) > 4.0) {
                    break;
                }
            }

            // ---- Build the BLA merge tree from the stored reference orbit (Zhuoran) ----
            buildBLA(actual_length);

            this->valid = true;
            return actual_length;
        }

        /**
         * @brief Build the bilinear-approximation merge tree from the (already-built)
         * reference orbit `orbit[0..L)`. Level 0 has one BLA per iteration; each higher
         * level merges adjacent pairs, doubling the skip length, with validity radii
         * shrunk by the standard Zhuoran merge rule (using `dc_max`). All double math.
         */
        void buildBLA(size_t L) {
            // tol trades skip aggressiveness vs approximation error (visible glitches).
            constexpr double BLA_TOL = 1.0 / static_cast<double>(1u << 24);
            blaLevels.clear();
            orbitLen = L;
            if (L == 0) return;

            // Level 0: δ_{i+1} ≈ 2·Z_i·δ_i + dc  →  A = 2 Z_i, B = 1, r = tol·|2 Z_i|.
            std::vector<BLA> level;
            level.reserve(L);
            for (size_t i = 0; i < L; ++i) {
                const double Ar = 2.0 * orbit[i].center.real();
                const double Ai = 2.0 * orbit[i].center.imag();
                const double r  = BLA_TOL * std::sqrt(Ar * Ar + Ai * Ai);
                level.push_back(BLA{Ar, Ai, 1.0, 0.0, r * r});
            }
            blaLevels.push_back(std::move(level));

            // Merge upward while a full pair of children exists.
            while (blaLevels.back().size() >= 2) {
                const std::vector<BLA>& prev = blaLevels.back();
                const size_t cnt = prev.size() / 2;
                std::vector<BLA> merged;
                merged.reserve(cnt);
                for (size_t j = 0; j < cnt; ++j) {
                    const BLA& x = prev[2 * j];       // earlier run [n, n+m)
                    const BLA& y = prev[2 * j + 1];   // later run   [n+m, n+2m)
                    // A = Ay·Ax ; B = Ay·Bx + By   (complex)
                    const double Ar = y.Ar * x.Ar - y.Ai * x.Ai;
                    const double Ai = y.Ar * x.Ai + y.Ai * x.Ar;
                    const double Br = (y.Ar * x.Br - y.Ai * x.Bi) + y.Br;
                    const double Bi = (y.Ar * x.Bi + y.Ai * x.Br) + y.Bi;
                    // r = min(rx, max(0, (ry − |Bx|·dc_max) / |Ax|))
                    const double rx    = std::sqrt(x.r2);
                    const double ry    = std::sqrt(y.r2);
                    const double absBx = std::sqrt(x.Br * x.Br + x.Bi * x.Bi);
                    const double absAx = std::sqrt(x.Ar * x.Ar + x.Ai * x.Ai);
                    const double r2nd  = (absAx > 0.0) ? std::max(0.0, (ry - absBx * dc_max) / absAx) : 0.0;
                    const double r     = std::min(rx, r2nd);
                    merged.push_back(BLA{Ar, Ai, Br, Bi, r * r});
                }
                blaLevels.push_back(std::move(merged));
            }
        }
    };
    std::atomic<uint64_t> rebuilds_{0};                   // reference rebuilds (telemetry)

    // Last built reference, kept across frames so low-res previews REUSE it (temporal
    // stability + speed) while the camera stays within its validity radius; only settles
    // (or a too-far pan / iteration change) rebuild. `lastRef_`'s release publishes the
    // metadata written before it. (Cross-frame lifetime vs stragglers deferred.)
    std::atomic<ReferenceCache*> lastRef_{nullptr};
    std::complex<core::BigFloat> lastRefCamCenter_;   // camera centre when it was built
    double lastRefPixelStep_{0.0};                    // pixelStep.real() at build (zoom check)
    unsigned int lastRefIters_{0};

    // Per-worker reference-cache pool: the frame's leader builds the reference into its
    // OWN cache and publishes the pointer in the job; followers render against it. No
    // global cache. (Lifetime vs a straggler reusing this while another job reads it is
    // intentionally ignored for now — hazard pointers / pooling later.)
    std::array<ReferenceCache*, MAX_WORKERS> workerLocalCache_{};
    std::atomic<uint32_t> nextWorkerLocalCache_{0};

    ReferenceCache* getLocalCache() {
        static thread_local ReferenceCache* ptr = [this]() {
            const size_t my_idx = nextWorkerLocalCache_.fetch_add(1ul, std::memory_order_relaxed);
            assert(my_idx < MAX_WORKERS && "Exceeded MAX_WORKERS cache allocation limits!");
            ReferenceCache* allocated = new ReferenceCache();
            workerLocalCache_[my_idx] = allocated;
            return allocated;
        }();
        return ptr;
    }

    // ---- Frame-global glitch resolution (Phase 4) ----
    // Stranded pixels (glitched / outran a short reference) are recorded here during
    // render, then re-perturbed against ONE shared secondary reference per round so a
    // glitched blob resolves coherently (no per-tile seams). Double-buffered: round r
    // reads buf[cur], appends survivors to buf[next]; the round leader swaps them.
    struct StrandedPixel { uint32_t px, py, brk; };
    std::vector<StrandedPixel> resolveBuf_[2];
    std::atomic<size_t>        resolveCount_[2];
    int                        resolveCurSel_{0};   // which buf is "cur" (leader-managed)
    std::atomic<uint32_t>      resolveInit_{0};     // one-shot round-0 builder election

    // The round's shared secondary reference (built by the round leader, read by all).
    std::vector<ComplexDouble> secOrbit_;
    double secRefSdx_{0.0}, secRefSdy_{0.0};
    size_t secLen_{0};
    bool   secEscaped_{false};
    double secFinalR2_{0.0};
    size_t secPickIdx_{0};   // index (in cur buf) of the pixel used as this round's secondary

    // Reset per-frame resolve state + size the buffers to the whole frame. Called by the
    // cache-publishing worker BEFORE it publishes (so the reset happens-before any render
    // append, which happens-after every worker's waitCache).
    void prepareResolveFrame(size_t width, size_t height) {
        const size_t cap = width * height;
        if (resolveBuf_[0].size() < cap) resolveBuf_[0].resize(cap);
        if (resolveBuf_[1].size() < cap) resolveBuf_[1].resize(cap);
        resolveCount_[0].store(0, std::memory_order_relaxed);
        resolveCount_[1].store(0, std::memory_order_relaxed);
        resolveCurSel_ = 0;
        resolveInit_.store(0, std::memory_order_relaxed);
    }

    // Run the frame-global resolution phase (all workers cooperate). Defined in the .cpp.
    void processResolvePhase(job::RenderJob& job, const job::RenderJob::JobSpecs& specs);

    const util::Gradient& gradient;

    void processChunkScalar(
        const job::RenderJob& job_ref,
        const job::RenderJob::JobSpecs& specs,
        const ReferenceCache* __restrict__ cache,
        const size_t chunk_idx);

public:
    explicit PerturbationEngine(const util::Gradient& gradient, core::Pixel*& back_buf_ref):
        gradient(gradient),back_buf_ref(back_buf_ref) {}

    /// Three-phase per-job driver (validate → rebuild → render). Defined in the .cpp.
    void processPerturbationJob(job::RenderJob& job);

    /// Number of reference-orbit rebuilds so far (increments once per frame now that
    /// the reference is rebuilt unconditionally).
    uint64_t rebuildCount() const noexcept { return rebuilds_.load(std::memory_order_relaxed); }

    static inline size_t getChunks(unsigned int width, unsigned int height) noexcept {
        return core::ComputeTotalChunks(width, height);
    }

    unsigned int evaluateBigFloatDepth(
        const job::RenderJob& job_ref,
        const std::complex<core::BigFloat>& c, 
        unsigned int max_iter);
    
};

} // namespace engine
