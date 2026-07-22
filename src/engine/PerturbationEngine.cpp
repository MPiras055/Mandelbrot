#include "engine/PerturbationEngine.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace engine {

namespace {

/**
 * @brief Initialise a worker-local probe grid and elect the one the whole frame shares.
 *
 * @details Every worker inits its own matrix, then races to CAS it into the job. The
 * winner's matrix becomes the shared one; losers adopt the winner's pointer and discard
 * their own. Both resolution paths did this identically, and the duplication was hiding
 * an inconsistency — the same publish used `acq_rel` at one site and the *default*
 * `seq_cst` at the other two. Stated once here.
 *
 * @return the shared matrix (this worker's, or the winner's).
 */
template <typename Matrix>
Matrix& electSharedMatrix(job::PerturbationJob& jobState,
                          const job::RenderJob::JobSpecs& specs,
                          Matrix* local) {
    const std::complex<double> start_step{
        specs.pixelStep.real() * (specs.width  / 4.0) / Matrix::DIMENSION,
        specs.pixelStep.imag() * (specs.height / 4.0) / Matrix::DIMENSION
    };
    local->init(specs.reference, start_step);

    void* expected = nullptr;
    if (!jobState.rebaseMatrix.compare_exchange_strong(expected, local, std::memory_order_acq_rel))
        local = static_cast<Matrix*>(expected);
    return *local;
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
    // Tile bounds first — independent of the cache (shared with the ETA engine).
    const core::TileBounds tile = core::ChunkTile(chunk_idx, specs.width, specs.height);
    const int tile_px = tile.x0, end_px = tile.x1;
    const int tile_py = tile.y0, end_py = tile.y1;

    core::Pixel* const __restrict__ raw_canvas = back_buf_ref;

    const OrbitRecord* __restrict__ orbitCache = cache->orbit.data();
    const unsigned int max_iter = specs.iterations;
    const size_t L = cache->orbit.size();

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
            // (Compute() returns PIXEL_BLACK for iter >= max_iter, so name it directly.)
            row_ptr[px] = escaped
                ? util::ColorUtil::Compute(n, max_iter, static_cast<float>(final_r2), gradient)
                : core::PIXEL_BLACK;
        }
    }
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
            globalCacheCameraCenter = specs.reference;
            globalCachePixelStep = specs.pixelStep.real();   // zoom at build (for the reuse zoom check)
            globalCacheIterations     = specs.iterations;
            globalCache.store(local, std::memory_order_release);   // publishes the metadata above
        }
        jobState.publishCache(job.aborted() ? PTB::CACHE_ABORT : static_cast<void*>(local));
    };


    bool cachePublisher = false;
    
    // ---- Phase 1: build the reference, publish the pointer ----
    if (!specs.fullReference) {
        // LOW-RES: REUSE the last reference while the camera is within its validity radius
        // (stable, flicker-free previews + no rebuild). All workers read the same stable
        // lastRef_ so they agree on the decision.
        ReferenceCache* reuse = globalCache.load(std::memory_order_acquire);
        bool reusable = (reuse != nullptr) && reuse->valid && (globalCacheIterations >= specs.iterations);
        if (reusable) {
            // (2) Zoom must be within ~1 octave — else the reference is stale for this depth.
            const double zoomRatio = specs.pixelStep.real() / globalCachePixelStep;
            reusable = (zoomRatio > 0.5 && zoomRatio < 2.0);
        }
        if (reusable) {
            // (3) Camera within half the CURRENT view diagonal of the reference centre.
            const double half_dx = specs.width  * 0.5 * specs.pixelStep.real();
            const double half_dy = specs.height * 0.5 * specs.pixelStep.imag();
            const double validRadius = std::sqrt(half_dx * half_dx + half_dy * half_dy);
            const double dx = static_cast<double>(specs.reference.real() - globalCacheCameraCenter.real());
            const double dy = static_cast<double>(specs.reference.imag() - globalCacheCameraCenter.imag());
            reusable = (dx * dx + dy * dy) < (validRadius * validRadius);
        }
        if (reusable) {
            // One worker republishes the existing reference; the rest fall through to waitCache.
            void* expected = nullptr;
            if (jobState.rebaseMatrix.compare_exchange_strong(
                    expected, reinterpret_cast<void*>(1), std::memory_order_acq_rel))
                jobState.publishCache(job.aborted() ? PTB::CACHE_ABORT : static_cast<void*>(reuse));
            goto render_phase;
        }

        // Otherwise: a SINGLE cooperative probe pass over a SMALL grid (all idle workers
        // help), capped iterations (ranking only). The last reporter recenters on the
        // deepest cell, builds the reference to full depth, publishes it, and records it
        // as the new reusable reference.
        RebaseMatrixLow& sm = electSharedMatrix(jobState, specs, getLocalMatrixLow());

        const unsigned int probe_cap = std::min(specs.iterations, LOW_RES_PROBE_CAP);

        size_t cell;
        uint32_t iteration;
        bool leaderPhase = false;
        while (jobState.claimRebase(RebaseMatrixLow::REBASE_MTX_CHUNKS, iteration, cell)) {
            sm.computeAndStoreAt(cell, probe_cap, [&]{ return job.aborted(); });
            leaderPhase = jobState.reportRebase(RebaseMatrixLow::REBASE_MTX_CHUNKS);
        }
        if (job.aborted()) return;
        if (leaderPhase) {
            cachePublisher = true;
            sm.shrinkAndCenter(REBASE_SHRINK_FACTOR);   // recenter on the deepest cell (single pass)
            ThreadLocalCell& tcell = getThreadLocalCell();
            if(tcell.isLocalCacheReachable(workerLocalCache_)) {
                tcell.deferDealloc(tcell.localCache);
                tcell.localCache = new ReferenceCache();
            }
            tcell.localCache->build(job, sm.center, specs.iterations, computeDcMax(sm.center), &jobState.refBuildPercent);
            publishBuilt(tcell.localCache);
            //sweep the deallocation list
            tcell.sweepBucket(workerLocalCache_);
        }
        // non-leaders fall through to waitCache below
    } else {
        // HIGH-RES: cooperative neighbourhood search; the terminal leader builds + publishes.
        RebaseMatrix& sharedMatrix = electSharedMatrix(jobState, specs, getLocalMatrix());

        const double limit_x = specs.pixelStep.real() * 0.25;
        const double limit_y = specs.pixelStep.imag() * 0.25;

        size_t cell;
        uint32_t iteration;
        bool leaderPhase;

        while (true) {
            leaderPhase = false;
            while (jobState.claimRebase(RebaseMatrix::REBASE_MTX_CHUNKS, iteration, cell)) {
                sharedMatrix.computeAndStoreAt(cell, specs.iterations, [&]{ return job.aborted(); });
                leaderPhase = jobState.reportRebase(RebaseMatrix::REBASE_MTX_CHUNKS);
            }

            if (job.aborted()) return;

            if (leaderPhase) {
                cachePublisher = true;
                sharedMatrix.shrinkAndCenter(REBASE_SHRINK_FACTOR);   // recenter on the deepest cell
                if (std::abs(sharedMatrix.stepSize.real()) <= limit_x ||
                    std::abs(sharedMatrix.stepSize.imag()) <= limit_y) {
                        ThreadLocalCell& tcell = getThreadLocalCell();
                        if(tcell.isLocalCacheReachable(workerLocalCache_)) {
                            ReferenceCache* newLocal = new ReferenceCache();
                            tcell.deferDealloc(tcell.localCache);
                            tcell.localCache = new ReferenceCache();
                        }
                        tcell.localCache->build(job, sharedMatrix.center, specs.iterations, computeDcMax(sharedMatrix.center), &jobState.refBuildPercent);
                        publishBuilt(tcell.localCache);
                        //sweep the deallocation list
                        tcell.sweepBucket(workerLocalCache_);
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
    /**
    * this may return either nullptr + 1 || a valid pointer || valid pointer + 1
    */
    void* cachePtr = jobState.waitCache();
    if(cachePtr == PTB::CACHE_ABORT) return;
    ThreadLocalCell& tcell = getThreadLocalCell();

    const ReferenceCache* __restrict__ cache = tcell.protectCache(globalCache);
    //we could also protect the cache here using a reference counter and use a global cacheDeferredDealloc vector
    
    // ---- Phase 2: RENDER ----
    // Rebasing resolves every pixel inline, so there is no post-render resolution pass.
    size_t rc;
    while (jobState.claimRender(specs.chunks, rc)) {
        processChunkScalar(job, specs, cache, rc);
        jobState.reportRender();
    }

    tcell.releaseCache();
    //sweep bucket to try to deallocate some unused stuff
    tcell.sweepBucket(workerLocalCache_);

}

} // namespace engine
