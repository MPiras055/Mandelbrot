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
    using RebaseMatrix = RebaseMatrix__<9>;
    static constexpr unsigned int MAX_WORKERS = 128;
    static constexpr double REBASE_SHRINK_FACTOR = 0.25;
    static constexpr unsigned int REBASE_ITERATION = 100;
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

    
    

    /**
     * @brief: record struct that is used to store the final orbit
     * information
     * @note: for this implementation we're thinkering with the Taylor
     * series approximation of the Mandelbrot function
     */
    struct OrbitRecord {
        ComplexDouble center;
        ComplexDouble A;    //first  order derivative
        ComplexDouble B;    //second order derivative
        ComplexDouble C;    //third  order derivative
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
        util::LazyVector<OrbitRecord> orbit;   // Z_i + A,B,C at the reference point
        std::vector<double> thresholdEnvelope;
        std::complex<core::BigFloat> reference;     // absolute reference (center + probe delta)
        unsigned int iterations{0};            // orbit depth this cache was built to
        bool valid{false};

        /// @return the actual orbit length (== max_iterations if the reference never
        /// escaped). Polls @p job so a long BigFloat build aborts promptly when the
        /// frame is superseded; on abort it returns early with the cache left invalid
        /// (the caller checks and does not publish it).
        size_t build(const job::RenderJob& job, std::complex<core::BigFloat> ref, size_t max_iterations) {
            this->iterations = max_iterations;
            reference = ref;
            
            // orbit: reset size to 0 (keeps capacity); records are appended with
            // push_back below so orbit.size() reflects the true orbit length.
            orbit.init(max_iterations);
            // thresholdEnvelope: size it up front (resize, not reserve) so the
            // per-iteration writes below are in-bounds and the prefix-min scan sees
            // a real size().
            thresholdEnvelope.assign(max_iterations, 0.0);
    
            // Standard BigFloat Z tracking for the absolute reference orbit
            core::BigFloat z_r = 0.0;
            core::BigFloat z_i = 0.0;
    
            // BLA Coefficients (starting at 0)
            ComplexDouble A{0.0, 0.0};
            ComplexDouble B{0.0, 0.0};
            ComplexDouble C{0.0, 0.0};
            ComplexDouble D{0.0, 0.0}; // The 4th derivative used for the threshold
    
            size_t actual_length = 0;
    
            for (size_t i = 0; i < max_iterations; i++) {
                // Poll abort off the hot path so a superseded frame drops its build fast.
                if ((i & 0xFF) == 0 && job.aborted()) return actual_length;

                // 1. Store current state (cast center to double for the fast per-pixel loop)
                double current_z_r = static_cast<double>(z_r);
                double current_z_i = static_cast<double>(z_i);
                
                // Append the record (advances orbit.size()); reusing capacity from
                // the init() above keeps this allocation-free across rebuilds.
                orbit.push_back(OrbitRecord{
                    ComplexDouble{current_z_r, current_z_i}, A, B, C});
    
                // 2. Threshold from the 4th derivative (D). Stored SQUARED (the squared
                // valid-skip radius) so the render binary search compares squared
                // magnitudes and avoids a per-pixel sqrt: |delta|_valid is proportional
                // to |D|^(-1/4), so its square is |D|^(-1/2) = 1/sqrt(|D|).
                double abs_D = std::sqrt(D.real() * D.real() + D.imag() * D.imag());
                if (abs_D > 0.0) {
                    thresholdEnvelope[i] = 1.0 / std::sqrt(abs_D);
                } else {
                    // At iteration 0, error is 0, so valid radius is theoretically infinite
                    thresholdEnvelope[i] = std::numeric_limits<double>::max(); 
                }
    
                // 3. Calculate next BLA coefficients using double precision
                ComplexDouble Z_n{current_z_r, current_z_i};
                ComplexDouble two_Z_n = Z_n * 2.0;
    
                ComplexDouble next_D = (two_Z_n * D) + (A * C * 2.0) + (B * B);
                ComplexDouble next_C = (two_Z_n * C) + (A * B * 2.0);
                ComplexDouble next_B = (two_Z_n * B) + (A * A);
                ComplexDouble next_A = (two_Z_n * A) + ComplexDouble{1.0, 0.0};
    
                A = next_A;
                B = next_B;
                C = next_C;
                D = next_D;
    
                // 4. Step the actual reference orbit forward using BigFloat
                core::BigFloat next_z_r = z_r * z_r - z_i * z_i + reference.real();
                core::BigFloat next_z_i = core::BigFloat(2.0) * z_r * z_i + reference.imag();
                
                z_r = next_z_r;
                z_i = next_z_i;
                actual_length++;
    
                // If the reference orbit escapes, we stop building early
                if (static_cast<double>(z_r * z_r + z_i * z_i) > 4.0) {
                    break;
                }
            }
    
            // orbit already holds exactly `actual_length` records (via push_back);
            // trim the threshold envelope to match if the reference orbit escaped
            // before max_iterations.
            if (actual_length < max_iterations) {
                thresholdEnvelope.resize(actual_length);
            }
    
            // 5. Scan the threshold envelope to record the prefix minimum.
            // This ensures the skip radius is monotonically non-increasing, which is 
            // strictly required for the binary search in your rendering loop to work.
            if (!thresholdEnvelope.empty()) {
                double current_min = thresholdEnvelope[0];
                for (size_t i = 1; i < thresholdEnvelope.size(); i++) {
                    if (thresholdEnvelope[i] < current_min) {
                        current_min = thresholdEnvelope[i];
                    } else {
                        thresholdEnvelope[i] = current_min;
                    }
                }
            }
    
            this->valid = true;
            return actual_length;
        }
    };
    ReferenceCache cacheSlots_[2];
    std::atomic<ReferenceCache*> activeCache_{nullptr};   // nullptr => must rebuild
    std::atomic<uint64_t> rebuilds_{0};                   // reference rebuilds (telemetry)
    
    /**
     * @brief Validate one render tile against a cached reference (Phase 1).
     * @details Samples 1 pixel in 16 and runs the perturbation loop; returns true if
     * any sample glitches — the Pauldelbrot criterion |Z_ref+δ|² < |δ|²·ε (the full
     * point collapses relative to the perturbation => catastrophic cancellation =>
     * the reference is inadequate here) or a NaN. A glitch forces a rebuild.
     */
    bool validateTile(const job::RenderJob& job, const job::RenderJob::JobSpecs& specs, size_t chunkId);

    bool rebaseChunk(const job::RenderJob& job, size_t chunk_idx);

    const util::Gradient& gradient;

    void processChunkScalar(
        const job::RenderJob& job_ref,
        const job::RenderJob::JobSpecs& specs,
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
