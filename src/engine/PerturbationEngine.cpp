#include "engine/PerturbationEngine.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace engine {

namespace {

// Pauldelbrot glitch threshold (squared): |Z+δ|² < eps·|δ|² ⇒ catastrophic cancellation.
constexpr double GLITCH_EPS = 1e-6;

struct OrbitBuildResult {
    size_t length;      // number of stored orbit points (== escape iter if it escaped)
    bool   escaped;     // the reference point itself escaped before max_iter
    double final_r2;    // |z|² at escape (valid only when escaped)
};

// Build the absolute reference orbit at `abs_center` to `max_iter`, storing Z_i as
// doubles in `out` (reused buffer). Abort-polled every 256 steps. Reports whether the
// reference point escaped so the caller can colour it directly.
OrbitBuildResult buildOrbitBigFloat(const job::RenderJob& job,
                                    const std::complex<BigFloat>& abs_center,
                                    unsigned int max_iter,
                                    std::vector<ComplexDouble>& out) {
    out.reserve(max_iter);   // stable storage: no realloc during the emplace_back loop
    out.clear();
    BigFloat z_r = 0.0, z_i = 0.0;
    const BigFloat c_r = abs_center.real();
    const BigFloat c_i = abs_center.imag();

    for (unsigned int i = 0; i < max_iter; ++i) {
        if ((i & 0xFF) == 0 && job.aborted()) return { out.size(), false, 0.0 };
        out.emplace_back(static_cast<double>(z_r), static_cast<double>(z_i));

        const BigFloat nzr = z_r * z_r - z_i * z_i + c_r;
        const BigFloat nzi = BigFloat(2.0) * z_r * z_i + c_i;
        z_r = nzr; z_i = nzi;

        const double r2 = static_cast<double>(z_r * z_r + z_i * z_i);
        if (r2 > 4.0) return { out.size(), true, r2 };   // escaped at iteration out.size()
    }
    return { out.size(), false, 0.0 };   // reached max_iter without escaping (interior)
}

} // namespace

/**
 * @brief Processes a single chunk of the screen using Perturbation Theory and Taylor Series Approximation.
 */
void PerturbationEngine::processChunkScalar(
    const job::RenderJob& job_ref,
    const job::RenderJob::JobSpecs& specs,
    const ReferenceCache* __restrict__ cache,
    const size_t chunk_idx
) {
    // Tile bounds first — independent of the cache.
    const int tiles_x = (specs.width + TILE_SIZE - 1) / TILE_SIZE;
    const size_t tile_px = (chunk_idx % tiles_x) * TILE_SIZE;
    const size_t tile_py = (chunk_idx / tiles_x) * TILE_SIZE;

    const int end_px = std::min(tile_px + TILE_SIZE, static_cast<size_t>(specs.width));
    const int end_py = std::min(tile_py + TILE_SIZE, static_cast<size_t>(specs.height));

    core::Pixel* const __restrict__ raw_canvas = back_buf_ref;

    const OrbitRecord* __restrict__ orbitCache = cache->orbit.data();
    const unsigned int max_iter = specs.iterations;
    const size_t valid_orbit_size = cache->orbit.size();

    const double cache_offset_x = static_cast<double>(specs.reference.real() - cache->reference.real());
    const double cache_offset_y = static_cast<double>(specs.reference.imag() - cache->reference.imag());

    const double base_screen_xMin = -(specs.width / 2.0) * specs.pixelStep.real();
    const double base_screen_yMin = -(specs.height / 2.0) * specs.pixelStep.imag();

    // BLA merge tree for this reference (built in ReferenceCache::buildBLA).
    const std::vector<std::vector<BLA>>& blaLevels = cache->blaLevels;
    const int num_bla_levels = static_cast<int>(blaLevels.size());

    // Stored reference points are Z_0..Z_{L-1}, all bounded (build() never stores the
    // escaping value). Rebasing reads Z_m at the top of the walk, so m must never exceed
    // `last_ref`. A degenerate reference (escaped at once) can't be perturbed against.
    const size_t L = valid_orbit_size;
    if (L < 2 || num_bla_levels == 0) return;
    const unsigned int last_ref = static_cast<unsigned int>(L - 1);

    for (int py = tile_py; py < end_py; py++) {
        core::Pixel* const row_ptr = raw_canvas + (py * specs.width);
        
        double screen_dy = base_screen_yMin + (py * specs.pixelStep.imag());
        double dc_imag = cache_offset_y + screen_dy;

        for (int px = tile_px; px < end_px; px++) {
            double screen_dx = base_screen_xMin + (px * specs.pixelStep.real());
            double dc_real = cache_offset_x + screen_dx;

            double dx = 0.0, dy = 0.0;   // δ, relative to Z_m
            unsigned int n = 0;          // TRUE iteration count — drives the colour
            unsigned int m = 0;          // index into the reference orbit (cycles)

            double final_r2 = 0.0;
            bool escaped = false;

            // ---- Perturbation walk with Zhuoran REBASING ----
            // A short reference is not a defect: when it is exhausted (or when δ dominates
            // the full value, i.e. the classic Pauldelbrot cancellation), re-express the
            // pixel against Z_0 = 0 by setting δ ← z. That is exact (z = Z_m + δ, Z_0 = 0)
            // and costs nothing, so ANY reference can track a pixel to arbitrary depth.
            // BLA runs skip 2^l iterations at a time while δ stays inside the validity
            // radius. `dc` is invariant under a rebase, which is what makes this exact.
            while (n < max_iter) {
                // Full value at the current reference index (m <= last_ref is invariant).
                double zr = orbitCache[m].center.real();
                double zi = orbitCache[m].center.imag();
                const double fr = zr + dx, fi = zi + dy;
                const double r2 = fr * fr + fi * fi;

                if (r2 > 4.0) { final_r2 = r2; escaped = true; break; }

                // Rebase when δ dominates the full value, or when one more step would run
                // off the end of the reference orbit. Z_0 = 0 by construction, so the
                // reference point must be re-zeroed here too — the single-step branch below
                // reads zr/zi and would otherwise apply the PRE-rebase Z_m against the
                // post-rebase δ, injecting a spurious 2·Z_old·δ term at every rebase.
                if (r2 < dx * dx + dy * dy || m >= last_ref) {
                    dx = fr; dy = fi;
                    m  = 0;
                    zr = 0.0; zi = 0.0;
                }

                const double d2 = dx * dx + dy * dy;

                // Largest usable BLA level at `m`: the run starts at a multiple of 2^l,
                // must stay within the orbit and the iteration budget, and its validity
                // radius must contain δ.
                const int maxL = (m == 0)
                    ? (num_bla_levels - 1)
                    : std::min(num_bla_levels - 1, static_cast<int>(__builtin_ctz(m)));

                bool applied = false;
                for (int l = maxL; l >= 0; --l) {
                    const unsigned int step = 1u << l;
                    if (m + step > last_ref) continue;
                    if (n + step > max_iter)  continue;
                    const BLA& b = blaLevels[l][m >> l];
                    if (d2 < b.r2) {
                        const double ndx = (b.Ar * dx - b.Ai * dy) + (b.Br * dc_real - b.Bi * dc_imag);
                        const double ndy = (b.Ar * dy + b.Ai * dx) + (b.Br * dc_imag + b.Bi * dc_real);
                        dx = ndx; dy = ndy;
                        m += step; n += step;
                        applied = true;
                        break;
                    }
                }

                if (!applied) {
                    // Single perturbation step (keeps the nonlinear δ² term).
                    const double dx2 = dx * dx, dy2 = dy * dy;
                    const double ndx = 2.0 * (zr * dx - zi * dy) + dx2 - dy2 + dc_real;
                    const double ndy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + dc_imag;
                    // δ blowing up past any representable orbit means the point has left
                    // the disc for good — treat it as escaped rather than stranding it.
                    if (std::abs(ndx) > 1e150 || std::abs(ndy) > 1e150) {
                        final_r2 = 8.0; escaped = true; break;
                    }
                    dx = ndx; dy = ndy;
                    ++m; ++n;
                }
            }

            // ---- Colour: escape count, or a genuine interior hit at the full budget ----
            row_ptr[px] = escaped
                ? util::ColorUtil::Compute(n, max_iter, static_cast<float>(final_r2), gradient)
                : util::ColorUtil::Compute(max_iter, max_iter, 0.0f, gradient);
        }
    }
}


unsigned int PerturbationEngine::evaluateBigFloatDepth(
    const job::RenderJob& job_ref,
    const std::complex<core::BigFloat>& c, 
    unsigned int max_iter) 
{
    core::BigFloat z_r = 0.0;
    core::BigFloat z_i = 0.0;

    const core::BigFloat c_r = c.real();
    const core::BigFloat c_i = c.imag();

    unsigned int iter = 0;
    
    // Evaluate standard Mandelbrot formula: Z_{n+1} = Z_n^2 + C
    while (iter < max_iter) {
        // Check for cancellation every 100 iterations to avoid stalling threads
        // doing heavy BigFloat math if the user pans the camera.
        if ((iter % 100 == 0) && job_ref.aborted()) {
            return 0; 
        }

        core::BigFloat z_r_sq = z_r * z_r;
        core::BigFloat z_i_sq = z_i * z_i;

        // Optimization: Cast the squared sum to a standard double for the threshold check.
        // BigFloat comparisons are relatively slow, and we only care if it crossed 4.0.
        double r2 = static_cast<double>(z_r_sq + z_i_sq);
        if (r2 > 4.0) {
            break;
        }

        // Calculate next Z
        core::BigFloat next_z_i = core::BigFloat(2.0) * z_r * z_i + c_i;
        z_r = z_r_sq - z_i_sq + c_r;
        z_i = next_z_i;

        iter++;
    }

    return iter;
}

/**
 * @brief Per-job driver: the leader builds the frame's reference into its OWN
 * worker-local cache and publishes the pointer in the job; followers render against it.
 *   - low-res frames (`!fullReference`): a single elected builder builds the SCREEN
 *     CENTER — no neighbourhood search (lightweight cache).
 *   - high-res frames: the cooperative neighbourhood search; the terminal leader builds.
 * @note The worker-local cache is reused by its owner on its next job while a straggler
 *       may still hold the published pointer — a known lifetime hazard, deferred to a
 *       later hazard-pointer / pooling pass.
 */
void PerturbationEngine::processPerturbationJob(job::RenderJob& job) {
    using PTB = job::PerturbationJob;
    auto& jobState = job.getState<job::RenderJob::PTBJob>();
    const auto specs = job.getSpecs();

    // Largest |pixel − reference| over the view = |reference→center| + half view diagonal.
    // The BLA validity radii are scaled to it so they stay valid for every pixel.
    auto computeDcMax = [&](const std::complex<core::BigFloat>& center) -> double {
        const double off_x = static_cast<double>(specs.reference.real() - center.real());
        const double off_y = static_cast<double>(specs.reference.imag() - center.imag());
        const double half_dx = specs.width  * 0.5 * specs.pixelStep.real();
        const double half_dy = specs.height * 0.5 * specs.pixelStep.imag();
        return std::sqrt(off_x * off_x + off_y * off_y)
             + std::sqrt(half_dx * half_dx + half_dy * half_dy);
    };

    // Leader publishes its just-built local cache pointer (or CACHE_ABORT if the job died),
    // and records it as the reusable reference for subsequent previews.
    auto publishBuilt = [&](ReferenceCache* local) {
        if (!job.aborted()) {
            lastRefCamCenter_ = specs.reference;
            lastRefPixelStep_ = specs.pixelStep.real();   // zoom at build (for the reuse zoom check)
            lastRefIters_     = specs.iterations;
            lastRef_.store(local, std::memory_order_release);   // publishes the metadata above
        }
        rebuilds_.fetch_add(1, std::memory_order_relaxed);
        // Reset the frame-global resolve buffers BEFORE publishing: the publishCache
        // release pairs with every worker's waitCache acquire, so this happens-before any
        // render-time append into resolveBuf_[0].
        prepareResolveFrame(specs.width, specs.height);
        jobState.publishCache(job.aborted() ? PTB::CACHE_ABORT : static_cast<void*>(local));
    };

    // ---- Phase 1: build the reference, publish the pointer ----
    if (!specs.fullReference) {
        // LOW-RES: REUSE the last reference while the camera is within its validity radius
        // (stable, flicker-free previews + no rebuild). All workers read the same stable
        // lastRef_ so they agree on the decision.
        ReferenceCache* reuse = lastRef_.load(std::memory_order_acquire);
        bool reusable = (reuse != nullptr) && reuse->valid && (lastRefIters_ >= specs.iterations);
        if (reusable) {
            // (2) Zoom must be within ~1 octave — else the reference is stale for this depth.
            const double zoomRatio = specs.pixelStep.real() / lastRefPixelStep_;
            reusable = (zoomRatio > 0.5 && zoomRatio < 2.0);
        }
        if (reusable) {
            // (3) Camera within half the CURRENT view diagonal of the reference centre.
            const double half_dx = specs.width  * 0.5 * specs.pixelStep.real();
            const double half_dy = specs.height * 0.5 * specs.pixelStep.imag();
            const double validRadius = std::sqrt(half_dx * half_dx + half_dy * half_dy);
            const double dx = static_cast<double>(specs.reference.real() - lastRefCamCenter_.real());
            const double dy = static_cast<double>(specs.reference.imag() - lastRefCamCenter_.imag());
            reusable = (dx * dx + dy * dy) < (validRadius * validRadius);
        }
        if (reusable) {
            // One worker republishes the existing reference; the rest fall through to waitCache.
            void* expected = nullptr;
            if (jobState.rebaseMatrix.compare_exchange_strong(
                    expected, reinterpret_cast<void*>(1), std::memory_order_acq_rel)) {
                prepareResolveFrame(specs.width, specs.height);   // reset before publish (see publishBuilt)
                jobState.publishCache(job.aborted() ? PTB::CACHE_ABORT : static_cast<void*>(reuse));
            }
            goto render_phase;
        }

        // Otherwise: a SINGLE cooperative probe pass over a SMALL grid (all idle workers
        // help), capped iterations (ranking only). The last reporter recenters on the
        // deepest cell, builds the reference to full depth, publishes it, and records it
        // as the new reusable reference.
        RebaseMatrixLow* localMatrixPtr = getLocalMatrixLow();
        std::complex<double> start_step{
            specs.pixelStep.real() * (specs.width / 4.0) / RebaseMatrixLow::DIMENSION,
            specs.pixelStep.imag() * (specs.height / 4.0) / RebaseMatrixLow::DIMENSION
        };
        localMatrixPtr->init(specs.reference, start_step);

        void* sharedPtr = nullptr;
        if (!jobState.rebaseMatrix.compare_exchange_strong(sharedPtr, localMatrixPtr))
            localMatrixPtr = static_cast<RebaseMatrixLow*>(sharedPtr);
        RebaseMatrixLow& sm = *localMatrixPtr;

        const unsigned int probe_cap = std::min(specs.iterations, LOW_RES_PROBE_CAP);

        size_t cell;
        uint32_t iteration;
        bool leader = false;
        while (jobState.claimRebase(RebaseMatrixLow::REBASE_MTX_CHUNKS, iteration, cell)) {
            sm.computeAndStoreAt(cell, probe_cap);
            leader = jobState.reportRebase(RebaseMatrixLow::REBASE_MTX_CHUNKS);
        }
        if (job.aborted()) return;
        if (leader) {
            sm.shrinkAndCenter(0.25);   // recenter on the deepest cell (single pass)
            ReferenceCache* local = getLocalCache();
            local->build(job, sm.center, specs.iterations, computeDcMax(sm.center));
            publishBuilt(local);
        }
        // non-leaders fall through to waitCache below
    } else {
        // HIGH-RES: cooperative neighbourhood search; the terminal leader builds + publishes.
        RebaseMatrix* localMatrixPtr = getLocalMatrix();
        std::complex<double> start_step{
            specs.pixelStep.real() * (specs.width / 4.0) / RebaseMatrix::DIMENSION,
            specs.pixelStep.imag() * (specs.height / 4.0) / RebaseMatrix::DIMENSION
        };
        localMatrixPtr->init(specs.reference, start_step);

        void* sharedPtr = nullptr;
        if (!jobState.rebaseMatrix.compare_exchange_strong(sharedPtr, localMatrixPtr))
            localMatrixPtr = static_cast<RebaseMatrix*>(sharedPtr);
        RebaseMatrix& sharedMatrix = *localMatrixPtr;

        const double limit_x = specs.pixelStep.real() * 0.25;
        const double limit_y = specs.pixelStep.imag() * 0.25;
        const unsigned int rebase_iter_cap = specs.iterations;

        size_t cell;
        uint32_t iteration;
        bool leader;

        while (true) {
            leader = false;
            while (jobState.claimRebase(RebaseMatrix::REBASE_MTX_CHUNKS, iteration, cell)) {
                sharedMatrix.computeAndStoreAt(cell,specs.iterations);
                leader = jobState.reportRebase(RebaseMatrix::REBASE_MTX_CHUNKS);
            }

            if (job.aborted()) return;

            if (leader) {
                sharedMatrix.shrinkAndCenter(0.25);   // recenter on the deepest cell
                if (std::abs(sharedMatrix.stepSize.real()) <= limit_x ||
                    std::abs(sharedMatrix.stepSize.imag()) <= limit_y) {
                    ReferenceCache* local = getLocalCache();
                    local->build(job, sharedMatrix.center, specs.iterations, computeDcMax(sharedMatrix.center));
                    publishBuilt(local);
                }
                // Release followers only after the cache is published.
                jobState.advanceRebase(iteration + 1);
            } else {
                if (!jobState.waitRebaseIteration(iteration, RebaseMatrix::REBASE_MTX_CHUNKS))
                    return;
            }

            if (std::abs(sharedMatrix.stepSize.real()) <= limit_x ||
                std::abs(sharedMatrix.stepSize.imag()) <= limit_y) {
                break;
            }
        }
    }

    // ---- All workers: obtain the published reference (or bail on abort) ----
render_phase:
    void* cachePtr = jobState.waitCache();
    if (cachePtr == PTB::CACHE_ABORT) return;
    const ReferenceCache* __restrict__ cache = static_cast<const ReferenceCache*>(cachePtr);

    // ---- Phase 2: RENDER (stranded pixels recorded into the frame-global buffer) ----
    size_t rc;
    while (jobState.claimRender(specs.chunks, rc)) {
        processChunkScalar(job, specs, cache, rc);
        jobState.reportRender();
    }

    // ---- Phase 3: RESOLVE — barrier until the whole frame is rendered (buffer complete),
    // then cooperatively re-perturb the stranded pixels against shared secondaries. ----
    if (!jobState.waitRenderComplete(specs.chunks)) return;   // false on abort
    processResolvePhase(job, specs);
}

/**
 * @brief Frame-global glitch resolution (Phase 4). All workers cooperate: stranded pixels
 * recorded during render are re-perturbed against ONE shared secondary reference per round,
 * so a glitched region resolves coherently regardless of render-tile boundaries. Rounds are
 * bounded; the round leader picks the deepest survivor as the next secondary. Leftovers are
 * approximated as interior. Gates job completion via markResolveComplete().
 */
void PerturbationEngine::processResolvePhase(job::RenderJob& job,
                                             const job::RenderJob::JobSpecs& specs) {
    auto& jobState = job.getState<job::RenderJob::PTBJob>();

    core::Pixel* const __restrict__ raw_canvas = back_buf_ref;
    const unsigned int max_iter = specs.iterations;
    const double base_screen_xMin = -(specs.width  / 2.0) * specs.pixelStep.real();
    const double base_screen_yMin = -(specs.height / 2.0) * specs.pixelStep.imag();

    constexpr uint32_t MAX_ROUNDS     = 6;
    constexpr size_t   RESOLVE_RANGES = 256;   // fixed per-round partition (barrier count)

    auto screenDx = [&](uint32_t px){ return base_screen_xMin + px * specs.pixelStep.real(); };
    auto screenDy = [&](uint32_t py){ return base_screen_yMin + py * specs.pixelStep.imag(); };
    auto colourAt = [&](uint32_t px, uint32_t py, unsigned int it, double r2){
        raw_canvas[static_cast<size_t>(py) * specs.width + px] =
            util::ColorUtil::Compute(it, max_iter, static_cast<float>(r2), gradient);
    };

    // Leader helper: pick the deepest stranded pixel in buf[sel][0..count) and build its
    // secondary orbit into secOrbit_ + metadata. @return false on abort / degenerate build.
    auto buildSecondary = [&](int sel, size_t count) -> bool {
        const StrandedPixel* buf = resolveBuf_[sel].data();
        size_t pick = 0;
        for (size_t i = 1; i < count; ++i)
            if (buf[i].brk > buf[pick].brk) pick = i;
        secPickIdx_ = pick;
        secRefSdx_ = screenDx(buf[pick].px);
        secRefSdy_ = screenDy(buf[pick].py);
        const std::complex<BigFloat> abs_center{
            specs.reference.real() + BigFloat(secRefSdx_),
            specs.reference.imag() + BigFloat(secRefSdy_)
        };
        const OrbitBuildResult r = buildOrbitBigFloat(job, abs_center, max_iter, secOrbit_);
        secLen_ = r.length; secEscaped_ = r.escaped; secFinalR2_ = r.final_r2;
        if (job.aborted() || r.length == 0) return false;
        // Colour the reference pixel directly: perturbing it against its own orbit (dc=0)
        // keeps δ=0 and can never detect its escape, so resolve it from the build result.
        if (r.escaped) colourAt(buf[pick].px, buf[pick].py, static_cast<unsigned int>(r.length), r.final_r2);
        else           colourAt(buf[pick].px, buf[pick].py, max_iter, 0.0);
        return true;
    };

    // ---- round 0 bootstrap: one elected worker builds secondary[0] ----
    uint32_t claimed = 0;
    if (resolveInit_.compare_exchange_strong(claimed, 1, std::memory_order_acq_rel)) {
        const size_t count = resolveCount_[0].load(std::memory_order_acquire);
        if (count == 0 || !buildSecondary(0, count)) {
            jobState.markResolveComplete();   // nothing stranded (or aborted)
            return;
        }
        resolveCurSel_ = 0;
        jobState.beginResolveRound(0);        // publishes secondary[0] + opens round 0
    }

    // ---- round loop (all workers) ----
    uint32_t round = 0;
    while (true) {
        if (!jobState.waitResolveReady(round)) return;   // phase over / aborted

        bool   leader = false;
        size_t range;
        while (jobState.claimResolve(round, RESOLVE_RANGES, range)) {
            // Read the round's shared secondary state AFTER a successful claim: a successful
            // claim proves the round is still current, so no leader is concurrently
            // rebuilding secOrbit_ / resolveCurSel_ (the leader only advances once all
            // RESOLVE_RANGES ranges are reported, which can't happen while this range is
            // still in flight). This keeps the non-atomic reads race-free.
            const int    sel   = resolveCurSel_;
            const int    nxt   = sel ^ 1;
            const size_t count = resolveCount_[sel].load(std::memory_order_acquire);
            const StrandedPixel* __restrict__ curBuf = resolveBuf_[sel].data();
            const ComplexDouble* __restrict__ orb    = secOrbit_.data();
            const size_t       S       = secLen_;
            const unsigned int bound   = static_cast<unsigned int>(std::min(S, static_cast<size_t>(max_iter)));
            const double       ref_sdx = secRefSdx_, ref_sdy = secRefSdy_;
            const size_t       pickIdx = secPickIdx_;
            const size_t       range_sz = (count + RESOLVE_RANGES - 1) / RESOLVE_RANGES;

            const size_t begin = range * range_sz;
            const size_t end   = std::min(begin + range_sz, count);
            for (size_t i = begin; i < end; ++i) {
                if (i == pickIdx) continue;   // the reference pixel, coloured in buildSecondary
                const StrandedPixel& p = curBuf[i];
                const double dcr = screenDx(p.px) - ref_sdx;
                const double dci = screenDy(p.py) - ref_sdy;

                double dx = 0.0, dy = 0.0;
                unsigned int n = 0;
                double fr2 = 0.0;
                bool escaped = false, glitched = false;

                while (n < bound) {
                    const double zr = orb[n].real(), zi = orb[n].imag();
                    const double ndx = 2.0 * (zr * dx - zi * dy) + dx * dx - dy * dy + dcr;
                    const double ndy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + dci;
                    if (std::abs(ndx) > 1e150 || std::abs(ndy) > 1e150) { glitched = true; break; }
                    dx = ndx; dy = ndy;
                    ++n;
                    if (n < S) {
                        const double zr1 = orb[n].real(), zi1 = orb[n].imag();
                        const double fr = zr1 + dx, fi = zi1 + dy;
                        const double t = fr * fr + fi * fi;
                        if (t > 4.0) { fr2 = t; escaped = true; break; }
                        if (t < GLITCH_EPS * (dx * dx + dy * dy)) { glitched = true; break; }
                    }
                }

                if (escaped) {
                    colourAt(p.px, p.py, n, fr2);
                } else if (!glitched && n >= max_iter) {
                    colourAt(p.px, p.py, max_iter, 0.0);
                } else {
                    // Still unresolved → carry to the next round with a deeper breakout.
                    const size_t j = resolveCount_[nxt].fetch_add(1, std::memory_order_relaxed);
                    resolveBuf_[nxt][j] = StrandedPixel{ p.px, p.py, n };
                }
            }
            leader = jobState.reportResolve(RESOLVE_RANGES);
        }

        if (job.aborted()) return;

        if (leader) {
            // Safe to read/write the shared state directly: as the round's last reporter the
            // leader is the sole thread touching it now (all ranges reported ⇒ no worker is
            // still processing this round, and only the leader advances).
            const int    sel = resolveCurSel_;
            const int    nxt = sel ^ 1;
            const size_t nextCount = resolveCount_[nxt].load(std::memory_order_acquire);
            if (nextCount == 0 || round + 1 >= MAX_ROUNDS) {
                // Finalize: whatever survived (rare) → interior colour.
                const StrandedPixel* nb = resolveBuf_[nxt].data();
                for (size_t i = 0; i < nextCount; ++i)
                    colourAt(nb[i].px, nb[i].py, max_iter, 0.0);
                jobState.markResolveComplete();
                return;
            }
            // Swap: the survivors become the new "cur"; clear the old "cur" as the new "next".
            resolveCurSel_ = nxt;
            resolveCount_[sel].store(0, std::memory_order_relaxed);
            if (!buildSecondary(nxt, nextCount)) { jobState.markResolveComplete(); return; }
            jobState.beginResolveRound(round + 1);   // publishes secondary[r+1] + opens it
        }
        ++round;
    }
}

} // namespace engine
