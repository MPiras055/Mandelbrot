#include "engine/PerturbationEngine.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>

namespace engine {

/**
 * @brief Processes a single chunk of the screen using Perturbation Theory and Taylor Series Approximation.
 */
void PerturbationEngine::processChunkScalar(
    const job::RenderJob& job_ref,
    const job::RenderJob::JobSpecs& specs,
    const size_t chunk_idx
) {
    // Tile bounds first — independent of the cache.
    const int tiles_x = (specs.width + TILE_SIZE - 1) / TILE_SIZE;
    const size_t tile_px = (chunk_idx % tiles_x) * TILE_SIZE;
    const size_t tile_py = (chunk_idx / tiles_x) * TILE_SIZE;

    const int end_px = std::min(tile_px + TILE_SIZE, static_cast<size_t>(specs.width));
    const int end_py = std::min(tile_py + TILE_SIZE, static_cast<size_t>(specs.height));

    core::Pixel* const __restrict__ raw_canvas = back_buf_ref;

    ReferenceCache* cache = activeCache_.load(std::memory_order_acquire);
    if (!cache) {
        // No reference yet (a probe-disabled preview before the first settle built the
        // cache): paint the tile blank so the frame is clean rather than stale.
        for (int py = tile_py; py < end_py; py++) {
            core::Pixel* const row_ptr = raw_canvas + (py * specs.width);
            for (int px = tile_px; px < end_px; px++) row_ptr[px] = core::PIXEL_BLACK;
        }
        return;
    }

    const OrbitRecord* __restrict__ orbitCache = cache->orbit.data();
    const unsigned int max_iter = specs.iterations;
    const size_t valid_orbit_size = cache->orbit.size();

    const double cache_offset_x = static_cast<double>(specs.reference.real() - cache->reference.real());
    const double cache_offset_y = static_cast<double>(specs.reference.imag() - cache->reference.imag());

    const double base_screen_xMin = -(specs.width / 2.0) * specs.pixelStep.real();
    const double base_screen_yMin = -(specs.height / 2.0) * specs.pixelStep.imag();

    const double eps_scale2 = 1.0 / std::sqrt(specs.pixelStep.real() * 1e-6);

    // Cap per-tile BigFloat fallbacks so a short/inadequate reference degrades to a
    // fast, slightly-wrong tile instead of freezing this worker on one heavy tile.
    constexpr unsigned int MAX_FALLBACK_PER_TILE = 128;
    unsigned int fallback_budget = MAX_FALLBACK_PER_TILE;

    for (int py = tile_py; py < end_py; py++) {
        core::Pixel* const row_ptr = raw_canvas + (py * specs.width);
        
        double screen_dy = base_screen_yMin + (py * specs.pixelStep.imag());
        double dc_imag = cache_offset_y + screen_dy;

        for (int px = tile_px; px < end_px; px++) {
            double screen_dx = base_screen_xMin + (px * specs.pixelStep.real());
            double dc_real = cache_offset_x + screen_dx;

            double dx = 0.0, dy = 0.0;
            unsigned int iter = 0;

            const double dc_mag2 = dc_real * dc_real + dc_imag * dc_imag;
            const double T2 = dc_mag2 * eps_scale2;

            // Cap the skip at the iteration budget: a cache deeper than max_iter
            // (a 2048 settle cache serving a 1024 preview) must not skip past it.
            unsigned int lo = 0, hi = static_cast<unsigned int>(std::min(valid_orbit_size, static_cast<size_t>(max_iter)));
            while (lo < hi) {
                const unsigned int mid = (lo + hi) >> 1;
                if (cache->thresholdEnvelope[mid] >= T2) lo = mid + 1; else hi = mid;
            }
            const unsigned int N_skip = (lo > 0) ? lo - 1 : 0;

            // OPTIMIZATION: Unrolled polynomial seed (No ComplexDouble object overhead)
            if (N_skip > 0 && N_skip < valid_orbit_size) {
                // dc^2
                double dc2_r = dc_real * dc_real - dc_imag * dc_imag;
                double dc2_i = 2.0 * dc_real * dc_imag;
                
                // dc^3
                double dc3_r = dc2_r * dc_real - dc2_i * dc_imag;
                double dc3_i = dc2_r * dc_imag + dc2_i * dc_real;

                const OrbitRecord& rec = orbitCache[N_skip];
                
                dx = (rec.A.real() * dc_real - rec.A.imag() * dc_imag) +
                     (rec.B.real() * dc2_r   - rec.B.imag() * dc2_i) +
                     (rec.C.real() * dc3_r   - rec.C.imag() * dc3_i);
                     
                dy = (rec.A.real() * dc_imag + rec.A.imag() * dc_real) +
                     (rec.B.real() * dc2_i   + rec.B.imag() * dc2_r) +
                     (rec.C.real() * dc3_i   + rec.C.imag() * dc3_r);
                     
                iter = N_skip;
            }

            double final_r2 = 0.0;
            bool escaped = false;
            bool glitched = false;

            // PERTURBATION LOOP
            while (iter < valid_orbit_size && iter < max_iter) {
                // Bitwise AND is slightly faster than modulo for abort polling
                if (job_ref.aborted()) {
                    puts("JOB ABORTED");
                    return;
                }

                double zr = orbitCache[iter].center.real();
                double zi = orbitCache[iter].center.imag();

                // Precompute squares to save multiplications below
                double dx2 = dx * dx;
                double dy2 = dy * dy;
                
                double z_px_r = zr + dx;
                double z_px_i = zi + dy;

                double total_r2 = z_px_r * z_px_r + z_px_i * z_px_i;
                if (total_r2 > 4.0) {
                    final_r2 = total_r2;
                    escaped = true;
                    break;
                }

                double next_dx = 2.0 * (zr * dx - zi * dy) + dx2 - dy2 + dc_real;
                double next_dy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + dc_imag;

                // Branch reduction: std::abs compiles to a fast sign-bit mask
                if (std::abs(next_dx) > 1e150 || std::abs(next_dy) > 1e150) {
                    glitched = true; 
                    break;
                }

                dx = next_dx; 
                dy = next_dy;
                iter++;
            }

            // ==============================================================================
            // BIGFLOAT FALLBACK HANDLING
            // ==============================================================================
            // Ran out of reference orbit before escaping: recompute this pixel in full
            // BigFloat — but only within the per-tile budget. Glitched pixels are NOT
            // routed here (they get the cheap indicator fill below).
            if (!escaped && !glitched && iter == valid_orbit_size && iter < max_iter
                && fallback_budget > 0) {
                --fallback_budget;
                BigFloat abs_c_r = specs.reference.real() + BigFloat(screen_dx);
                BigFloat abs_c_i = specs.reference.imag() + BigFloat(screen_dy);

                BigFloat bf_z_r = 0.0;
                BigFloat bf_z_i = 0.0;
                unsigned int bf_iter = 0;

                while (bf_iter < max_iter) {
                    if (job_ref.aborted()) {
                        puts("JOB ABORTED");
                        return;
                    }

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
            }
            // Ran out of orbit but the fallback budget was spent: approximate as
            // interior so the tile can't stall on a wave of BigFloat recomputes.
            else if (!escaped && !glitched && iter == valid_orbit_size && iter < max_iter) {
                iter = max_iter;
            }

            // Final Color Write
            if (glitched) {
                row_ptr[px] = core::Pixel{255, 0, 255, 255}; 
            } else {
                row_ptr[px] = util::ColorUtil::Compute(iter, max_iter, static_cast<float>(final_r2), gradient);
            }
        }
    }
}


bool PerturbationEngine::validateTile(
    const job::RenderJob& job,
    const job::RenderJob::JobSpecs& specs,
    size_t chunk_idx)
{
    const ReferenceCache* cache = activeCache_.load(std::memory_order_acquire);
    if (!cache) return false;

    const OrbitRecord* __restrict__ oc = cache->orbit.data();
    const size_t osize = cache->orbit.size();
    const unsigned int max_iter = specs.iterations;

    // --- 1. Chunk Boundary Logic ---
    const int tiles_x = (specs.width + TILE_SIZE - 1) / TILE_SIZE;
    const int tile_px = (chunk_idx % tiles_x) * TILE_SIZE;
    const int tile_py = (chunk_idx / tiles_x) * TILE_SIZE;
    const int end_px = std::min(tile_px + TILE_SIZE, specs.width);
    const int end_py = std::min(tile_py + TILE_SIZE, specs.height);

    // --- 2. Coordinate Offset Calculation ---
    // Safely bridge the BigFloat gap just like in the render loop
    const double cache_offset_x = static_cast<double>(specs.reference.real() - cache->reference.real());
    const double cache_offset_y = static_cast<double>(specs.reference.imag() - cache->reference.imag());

    const double base_x = -(specs.width / 2.0) * specs.pixelStep.real();
    const double base_y = -(specs.height / 2.0) * specs.pixelStep.imag();

    // --- 3. Fast-Stepping Optimization ---
    // Instead of multiplying px/py inside the loops, we precalculate the starting 
    // positions and just add a constant "step" value on each iteration.
    const double skip_step_x = specs.pixelStep.real() * 4.0;
    const double skip_step_y = specs.pixelStep.imag() * 4.0;

    double current_dc_imag = cache_offset_y + base_y + (tile_py * specs.pixelStep.imag());
    const double start_dc_real = cache_offset_x + base_x + (tile_px * specs.pixelStep.real());

    static constexpr double GLITCH_EPS = 1e-6;

    // Sample 1 pixel in 16 (every 4th in x and y).
    for (int py = tile_py; py < end_py; py += 4) {
        if (job.aborted()) return false;
        
        double current_dc_real = start_dc_real;
        
        for (int px = tile_px; px < end_px; px += 4) {
            double dx = 0.0, dy = 0.0;
            
            for (unsigned int it = 0; it < osize && it < max_iter; ++it) {
                const double zr = oc[it].center.real();
                const double zi = oc[it].center.imag();
                
                const double zfr = zr + dx;
                const double zfi = zi + dy;
                const double full2 = zfr * zfr + zfi * zfi;
                
                if (full2 > 4.0) break;                     // Escaped: fine
                
                const double d2 = dx * dx + dy * dy;
                if (full2 < d2 * GLITCH_EPS) return true;   // Pauldelbrot glitch detected
                
                const double ndx = 2.0 * (zr * dx - zi * dy) + dx * dx - dy * dy + current_dc_real;
                const double ndy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + current_dc_imag;

                //kind of a std::isnan check just with ffast_math enabled
                if (ndx > 1e150 || ndx < -1e150 || 
                    ndy > 1e150 || ndy < -1e150) {
                        return true;
                }
                
                dx = ndx; 
                dy = ndy;
            }
            // Add step to X instead of multiplying
            current_dc_real += skip_step_x; 
        }
        // Add step to Y instead of multiplying
        current_dc_imag += skip_step_y; 
    }
    
    return false;
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
 * @brief Per-job driver: rebuild the reference UNCONDITIONALLY (cooperative
 * neighbourhood search + orbit build), then render. Every frame computes its own
 * reference — no cross-frame reuse, no validation, no center-first skip — so a frame
 * never renders against a stale or off-center reference.
 */
void PerturbationEngine::processPerturbationJob(job::RenderJob& job) {
    auto& jobState = job.getState<job::RenderJob::PTBJob>();
    const auto specs = job.getSpecs();

    // ---- Phase 1: REBUILD the reference (cooperative grid search + build) ----
    {
        // Inactive double-buffer slot to build into (ping-pong; never delete a live
        // cache — a straggler from the previous frame may still be reading the active one).
        auto inactiveSlot = [&]() -> ReferenceCache* {
            ReferenceCache* active = activeCache_.load(std::memory_order_acquire);
            return (active == &cacheSlots_[0]) ? &cacheSlots_[1] : &cacheSlots_[0];
        };

        RebaseMatrix* localMatrixPtr = getLocalMatrix();
        std::complex<double> start_step{
            specs.pixelStep.real() * (specs.width / 4.0) / RebaseMatrix::DIMENSION,
            specs.pixelStep.imag() * (specs.height / 4.0) / RebaseMatrix::DIMENSION
        };
        localMatrixPtr->init(specs.reference, start_step);

        // CAS elects the matrix owner; the losers share it and cooperate on the search.
        void* sharedPtr = nullptr;
        if (!jobState.rebaseMatrix.compare_exchange_strong(sharedPtr, localMatrixPtr))
            localMatrixPtr = static_cast<RebaseMatrix*>(sharedPtr);
        RebaseMatrix& sharedMatrix = *localMatrixPtr;

        const double limit_x = specs.pixelStep.real() * 0.25;
        const double limit_y = specs.pixelStep.imag() * 0.25;
        const unsigned int rebase_iter_cap = specs.iterations;   // probe to the real budget

        size_t cell;
        uint32_t iteration;
        bool leader;

        while (true) {
            leader = false;
            while (jobState.claimRebase(RebaseMatrix::REBASE_MTX_CHUNKS, iteration, cell)) {
                auto test_point = sharedMatrix.getPointAt(cell);
                unsigned int depth = evaluateBigFloatDepth(job, test_point, rebase_iter_cap);
                sharedMatrix.setValueAt(cell, depth);
                leader = jobState.reportRebase(RebaseMatrix::REBASE_MTX_CHUNKS);
            }

            if (job.aborted()) return;

            if (leader) {
                sharedMatrix.shrinkAndCenter(0.15);   // recenter on the deepest cell
                if (std::abs(sharedMatrix.stepSize.real()) <= limit_x ||
                    std::abs(sharedMatrix.stepSize.imag()) <= limit_y) {
                    ReferenceCache* target = inactiveSlot();
                    target->build(job, sharedMatrix.center, specs.iterations);
                    if (job.aborted()) return;   // build bailed early — don't publish a partial orbit
                    rebuilds_.fetch_add(1, std::memory_order_relaxed);
                    activeCache_.store(target, std::memory_order_release);
                }
                // Release followers ONLY after the cache is published (advanceRebase's
                // release pairs with the acquire in waitRebaseIteration).
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

    // ---- Phase 2: RENDER ----
    size_t rc;
    while (jobState.claimRender(specs.chunks, rc)) {
        processChunkScalar(job, specs, rc);
        jobState.reportRender();
    }
}

} // namespace engine
