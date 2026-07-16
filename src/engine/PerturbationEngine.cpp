#include "engine/PerturbationEngine.hpp"
#include <cmath>
#include <algorithm>

namespace engine {

PerturbationEngine::WorkerLocal& PerturbationEngine::getWorkerData() {
    static thread_local WorkerLocal* ptr = [this]() {
        const size_t my_idx = dealloc_idx__.fetch_add(1ul, std::memory_order_relaxed);
        assert(my_idx < MAX_WORKERS && "Exceeded MAX_WORKERS allocation limits!");
        WorkerLocal* allocated = new WorkerLocal();
        //register for deallocation
        dealloc__[my_idx] = allocated;
        return allocated;
    }();

    return *ptr;
}

/**
 * @brief Processes a single chunk of the screen using Perturbation Theory and Taylor Series Approximation.
 */
void PerturbationEngine::processChunkScalar(
    const job::RenderJob& job_ref,
    const job::RenderJob::JobSpecs& specs,
    const util::LazyVector<OrbitRecord>& finalOrbitCacheRef,
    const size_t chunk_idx,
    double best_dx,
    double best_dy
) {
    static constexpr size_t BLOCK_SIZE = 32;

    const OrbitRecord* __restrict__ finalOrbitCache = finalOrbitCacheRef.data();
    const unsigned int max_iter = specs.iterations;
    const size_t valid_orbit_size = finalOrbitCacheRef.size();

    // Calculate how many 2D tiles fit across the X-axis of the screen
    const int tiles_x = (specs.width + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // Convert the 1D chunk_idx into a starting 2D pixel coordinate (top-left of the block)
    const size_t tile_px = (chunk_idx % tiles_x) * BLOCK_SIZE;
    const size_t tile_py = (chunk_idx / tiles_x) * BLOCK_SIZE;

    // Clamp the boundaries so edge chunks don't render outside the screen array
    const int end_px = std::min(tile_px + BLOCK_SIZE, static_cast<size_t>(specs.width));
    const int end_py = std::min(tile_py + BLOCK_SIZE, static_cast<size_t>(specs.height));

    // Compute the distance from the mathematical center of the screen to the top-left corner
    double base_screen_xMin = -(specs.width / 2.0) * specs.pixelStepX;
    double base_screen_yMin = -(specs.height / 2.0) * specs.pixelStepY;

    // ==============================================================================
    // SERIES APPROXIMATION (GLITCH-FREE SKIPPING)
    // ==============================================================================
    // Instead of calculating the Mandelbrot loop from iteration 0 for every pixel, we can use
    // the derivatives (A and B) to "fast-forward" (skip) iterations en masse.
    // We can only do this while the error term (|B| * |dc|^2) is smaller than our tolerance.

    // First, find the maximum distance (max_dc_dist) from the reference point
    // to ANY pixel inside this specific 32x32 block.
    double max_dc_dist = 0.0;
    for (int vx : {0, static_cast<int>(end_px - tile_px)}) {
        for (int vy : {0, static_cast<int>(end_py - tile_py)}) {
            double dx = base_screen_xMin + ((tile_px + vx) * specs.pixelStepX) - best_dx;
            double dy = base_screen_yMin + ((tile_py + vy) * specs.pixelStepY) - best_dy;
            max_dc_dist = std::max(max_dc_dist, std::sqrt(dx*dx + dy*dy));
        }
    }

    // Calculate our acceptable error tolerance matching reference math exactly
    double tolerance = specs.pixelStepX * 0.1;
    unsigned int N_skip = 0;
    for (unsigned int i = 1; i < valid_orbit_size; ++i) {
        double b_mag = std::abs(finalOrbitCache[i].B);
        if (std::isnan(b_mag) || (b_mag * max_dc_dist * max_dc_dist > tolerance)) break;
        N_skip = i;
    }

    // ==============================================================================
    // MAIN PIXEL RENDERING LOOP
    // ==============================================================================
    core::Pixel* const __restrict__ raw_canvas = back_buf_ref;

    // Calculate absolute viewport center coordinates preserving precision
    double dbl_golden_cx = static_cast<double>(BigFloat(specs.x)) + best_dx;
    double dbl_golden_cy = static_cast<double>(BigFloat(specs.y)) + best_dy;

    for (int py = tile_py; py < end_py; py++) {
        core::Pixel* const row_ptr = raw_canvas + (py * specs.width);
        double dc_imag = base_screen_yMin + (py * specs.pixelStepY) - best_dy;

        for (int px = tile_px; px < end_px; px++) {
            double dc_real = base_screen_xMin + (px * specs.pixelStepX) - best_dx;

            double dx = 0.0, dy = 0.0;
            unsigned int iter = 0;

            //apply the polynomial until we're allowed to fast forward
            if (N_skip > 0 && N_skip < valid_orbit_size) {
                ComplexDouble dc{dc_real, dc_imag};
                ComplexDouble delta = (finalOrbitCache[N_skip].A * dc) + (finalOrbitCache[N_skip].B * (dc * dc));
                dx = delta.real();
                dy = delta.imag();
                iter = N_skip;
            }

            double z_px_r = 0.0, z_px_i = 0.0;
            double final_r2 = 0.0;
            bool escaped = false;
            bool glitched = false;

            //complete the iteration from the last possible one
            while (iter < valid_orbit_size && iter < max_iter) {
                if (job_ref.aborted()) return;

                double zr = finalOrbitCache[iter].center.real();
                double zi = finalOrbitCache[iter].center.imag();

                z_px_r = zr + dx;
                z_px_i = zi + dy;

                double total_r2 = z_px_r * z_px_r + z_px_i * z_px_i;
                if (total_r2 > 4.0) {
                    final_r2 = total_r2;
                    escaped = true;
                    break;
                }

                double next_dx = 2.0 * (zr * dx - zi * dy) + dx*dx - dy*dy + dc_real;
                double next_dy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + dc_imag;

                if (std::isnan(next_dx) || std::isnan(next_dy)) {
                    glitched = true; break;
                }

                dx = next_dx; dy = next_dy;
                iter++;
            }

            // --- FALLBACK LOOP ---
            if (!escaped && !glitched && iter == valid_orbit_size && iter < max_iter) {
                double abs_c_r = dbl_golden_cx + dc_real;
                double abs_c_i = dbl_golden_cy + dc_imag;

                while (iter < max_iter) {
                    if (job_ref.aborted()) return;

                    double temp_r = z_px_r * z_px_r - z_px_i * z_px_i + abs_c_r;
                    double temp_i = 2.0 * z_px_r * z_px_i + abs_c_i;
                    z_px_r = temp_r;
                    z_px_i = temp_i;

                    double r2 = z_px_r * z_px_r + z_px_i * z_px_i;
                    if (r2 > 4.0) {
                        final_r2 = r2;
                        break;
                    }
                    iter++;
                }
            }

            if (glitched) {
                row_ptr[px] = core::Pixel{255, 0, 255, 255};
            } else {
                row_ptr[px] = util::ColorUtil::Compute(iter, max_iter, static_cast<float>(final_r2), gradient);
            }
        }
    }
}

ComplexDouble PerturbationEngine::getProbeWinnerDelta(
    const std::array<unsigned int, PROBE_MROW * PROBE_MCOL> probeMatrix,
    ProbeGridConfig cfg,
    unsigned int max_iterations
) {
    (void)max_iterations;
    double best_dx{0.0}, best_dy{0.0};
    int best_iter = -1;

    for(size_t row = 0; row < PROBE_MROW; ++row) {
        double test_dy = cfg.start_dy + (row * cfg.grid_step_y);
        for(size_t col = 0; col < PROBE_MCOL; ++col) {
            double test_dx = cfg.start_dx + (col * cfg.grid_step_x);
            int escape_iter = probeMatrix[row * PROBE_MCOL + col];

            if( escape_iter > best_iter || (    //better iter
                escape_iter == best_iter &&
                ((test_dx * test_dx + test_dy * test_dy) < (best_dx * best_dx + best_dy * best_dy))
            )) {
                best_iter = escape_iter;
                best_dx = test_dx;
                best_dy = test_dy;
            }
        }
    }
    //if all points diverge then return 0,0 as delta (so the center)
    return best_iter == -1 ? ComplexDouble{0,0} : ComplexDouble{best_dx,best_dy};
}

/**
 * @brief: wait-free processing method (7-phase pipeline; see header for details).
 */
void PerturbationEngine::processPerturbationJob(job::RenderJob& job) {
    //get the perturbation job
    auto& jobState      = job.getState<job::RenderJob::PTBJob>();
    const auto specs    = job.getSpecs();
    WorkerLocal& local  = getWorkerData();
    //initialize the workerData
    local.init(specs.iterations);
    BigFloat finalOrbit_r = specs.x;
    BigFloat finalOrbit_i = specs.y;
    double best_dx = 0.0;
    double best_dy = 0.0;

    if(specs.enableDeltaProbing) {
        //init the grid config
        ProbeGridConfig probeCfg(specs);
        //compute the initial reference and publish it (return if job was aborted)
        // SYNCHRONIZATION POINT
        InitialOrbit* orbitPtr = local.computeInitialOrbit(job,specs.x,specs.y,specs.iterations);
        if(orbitPtr == nullptr) return; //job aborted
        InitialOrbit& refOrbit = *orbitPtr;
        size_t chunk_id;
        while(jobState.getProbeChunk(PROBE_MROW * PROBE_MCOL, chunk_id)) {
            (void)local.computeProbeChunk(job,probeCfg,refOrbit,specs.iterations,chunk_id);
            jobState.markCompletedProbe();
        }
        if(job.aborted()) return;

        //spin wait for all threads to finish the probing
        while(!jobState.probeCompleted(PROBE_MROW * PROBE_MCOL)) {
            cpu_relax();
        }
        if(job.aborted()) return;
        //compute the winner probe
        ComplexDouble bestDelta = getProbeWinnerDelta(refOrbit.probeMatrix,probeCfg,specs.iterations);
        best_dx = bestDelta.real();
        best_dy = bestDelta.imag();
        //add the delta to the final orbit position
        finalOrbit_r += best_dx;
        finalOrbit_i += best_dy;
    }

    //Compute the final orbit
    // SYNCHRONIZATION POINT
    util::LazyVector<OrbitRecord>* finalOrbitCache =
        local.computeFinalOrbit(job,jobState,finalOrbit_r,finalOrbit_i,specs.iterations);
    if(finalOrbitCache == nullptr) return;
    const util::LazyVector<OrbitRecord>& finalCache_ref = *finalOrbitCache;

    size_t chunk_id;
    while(jobState.getChunk(specs.chunks,chunk_id)) {
        processChunkScalar(job, specs, finalCache_ref, chunk_id, best_dx, best_dy);
        jobState.markCompletedChunk(chunk_id);
    }
}

} // namespace engine
