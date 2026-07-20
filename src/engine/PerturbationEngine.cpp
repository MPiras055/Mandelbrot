#include "engine/PerturbationEngine.hpp"
#include <cmath>
#include <algorithm>

namespace engine {

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

    // Cap per-tile BigFloat fallbacks so a short/inadequate reference degrades to a fast,
    // slightly-wrong tile instead of freezing this worker. Low-res uses a probe-picked
    // deep reference so fallbacks should be rare — give it ZERO budget (over-orbit pixels
    // are approximated as interior, keeping previews fast); high-res keeps a real budget.
    int fallback_budget = specs.fullReference ? 512 : 0;

    // Pauldelbrot glitch threshold (squared): |Z+δ|² < eps·|δ|² ⇒ catastrophic
    // cancellation (the reference is inadequate for this pixel) ⇒ resolve it exactly.
    constexpr double GLITCH_EPS = 1e-6;

    for (int py = tile_py; py < end_py; py++) {
        core::Pixel* const row_ptr = raw_canvas + (py * specs.width);
        
        double screen_dy = base_screen_yMin + (py * specs.pixelStep.imag());
        double dc_imag = cache_offset_y + screen_dy;

        for (int px = tile_px; px < end_px; px++) {
            double screen_dx = base_screen_xMin + (px * specs.pixelStep.real());
            double dc_real = cache_offset_x + screen_dx;

            double dx = 0.0, dy = 0.0;
            unsigned int iter = 0;

            double final_r2 = 0.0;
            bool escaped = false;
            bool glitched = false;

            // ---- BLA walk: greedily skip runs of iterations while δ stays inside the
            // local validity radius; drop to a single perturbation step (with the
            // nonlinear δ² term) only when δ grows past it — i.e. approaching escape. ----
            const unsigned int bound = static_cast<unsigned int>(
                std::min(valid_orbit_size, static_cast<size_t>(max_iter)));

            while (iter < bound) {
                const double d2 = dx * dx + dy * dy;

                // Largest usable BLA level at `iter`: the run starts at a multiple of
                // 2^l, must fit within `bound`, and its radius must contain δ.
                const int maxL = (iter == 0)
                    ? (num_bla_levels - 1)
                    : std::min(num_bla_levels - 1, static_cast<int>(__builtin_ctz(iter)));

                bool applied = false;
                for (int l = maxL; l >= 0; --l) {
                    const unsigned int step = 1u << l;
                    if (iter + step > bound) continue;
                    const BLA& b = blaLevels[l][iter >> l];
                    if (d2 < b.r2) {
                        const double ndx = (b.Ar * dx - b.Ai * dy) + (b.Br * dc_real - b.Bi * dc_imag);
                        const double ndy = (b.Ar * dy + b.Ai * dx) + (b.Br * dc_imag + b.Bi * dc_real);
                        dx = ndx; dy = ndy;
                        iter += step;
                        applied = true;
                        break;
                    }
                }

                if (!applied) {
                    // Single perturbation step (keeps the nonlinear δ² term).
                    const double zr = orbitCache[iter].center.real();
                    const double zi = orbitCache[iter].center.imag();
                    const double dx2 = dx * dx, dy2 = dy * dy;
                    const double ndx = 2.0 * (zr * dx - zi * dy) + dx2 - dy2 + dc_real;
                    const double ndy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + dc_imag;
                    if (std::abs(ndx) > 1e150 || std::abs(ndy) > 1e150) { glitched = true; break; }
                    dx = ndx; dy = ndy;
                    ++iter;
                }

                // Escape test at the landing iteration (δ is bounded across a BLA run,
                // so no escape can be skipped mid-run).
                if (iter < valid_orbit_size) {
                    const double zr = orbitCache[iter].center.real();
                    const double zi = orbitCache[iter].center.imag();
                    const double fr = zr + dx, fi = zi + dy;
                    const double total_r2 = fr * fr + fi * fi;
                    if (total_r2 > 4.0) { final_r2 = total_r2; escaped = true; break; }
                    // Pauldelbrot: the full point collapsed relative to δ → this reference
                    // can't track the pixel; flag it for exact resolution below.
                    if (total_r2 < GLITCH_EPS * (dx * dx + dy * dy)) { glitched = true; break; }
                }
            }

            // ==============================================================================
            // BIGFLOAT RESOLUTION (glitched or ran out of a short reference)
            // ==============================================================================
            // Recompute the pixel exactly in full BigFloat — bounded by the per-tile budget
            // so no single tile can freeze the worker.
            if (!escaped && (glitched || iter == valid_orbit_size) && iter < max_iter
                && fallback_budget > 0) {
                --fallback_budget;
                BigFloat abs_c_r = specs.reference.real() + BigFloat(screen_dx);
                BigFloat abs_c_i = specs.reference.imag() + BigFloat(screen_dy);

                BigFloat bf_z_r = 0.0;
                BigFloat bf_z_i = 0.0;
                unsigned int bf_iter = 0;

                while (bf_iter < max_iter) {

                    BigFloat bf_z_r_sq = bf_z_r * bf_z_r;
                    BigFloat bf_z_i_sq = bf_z_i * bf_z_i;

                    double r2 = static_cast<double>(bf_z_r_sq + bf_z_i_sq);
                    if (r2 > 4.0) {
                        final_r2 = r2;
                        escaped = true;
                        break;
                    }

                    BigFloat temp_i = BigFloat(2.0) * bf_z_r * bf_z_i + abs_c_i;
                    bf_z_r = bf_z_r_sq - bf_z_i_sq + abs_c_r;
                    bf_z_i = temp_i;

                    bf_iter++;
                }
                
                iter = bf_iter;
                if (!escaped) {
                    iter = max_iter;
                }
                glitched = false;   // resolved exactly
            }
            // Budget spent (or low-res, budget 0): approximate the unresolved pixel as
            // interior so the tile can't stall and we avoid speckle.
            else if (!escaped && (glitched || iter == valid_orbit_size) && iter < max_iter) {
                iter = max_iter;
                glitched = false;
            }

            if (job_ref.aborted()) {
                return;
            }

            // Final Color Write
            row_ptr[px] = util::ColorUtil::Compute(iter, max_iter, static_cast<float>(final_r2), gradient);
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
                    expected, reinterpret_cast<void*>(1), std::memory_order_acq_rel))
                jobState.publishCache(job.aborted() ? PTB::CACHE_ABORT : static_cast<void*>(reuse));
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
                puts("HIGH RES CACHE ITERATION");
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

    // ---- Phase 2: RENDER ----
    size_t rc;
    while (jobState.claimRender(specs.chunks, rc)) {
        std::cout << "Processing chunk " << rc << "\n";
        processChunkScalar(job, specs, cache, rc);
        jobState.reportRender();
    }
}

} // namespace engine
