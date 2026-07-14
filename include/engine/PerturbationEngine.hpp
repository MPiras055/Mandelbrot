#pragma once
#include <algorithm>
#include <complex>
#include <cassert>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include "core/Numeric.hpp"
#include "core/Kernel.hpp"
#include "macro_util.hpp"
#include "util/FrameBuffer.hpp"
#include "util/ColorUtil.hpp"
#include <optional>
#include "job/RenderJob.hpp"
#include "util/LazyVector.hpp"

namespace engine {

class MandelbrotEngine;

using BigFloat = core::BigFloat;
using ComplexDouble = core::ComplexDouble;

class PerturbationEngine {
    static constexpr size_t PROBE_MROW = 128;
    static constexpr size_t PROBE_MCOL = 128;
    static constexpr size_t PROBE_CELL_CHUNK = 128; 
    static constexpr size_t MAX_WORKERS = 128;

    /**
     * configuration struct for the matrix processing
     * * computed once per job only if the deltaProbing is enabled
     */
    struct ProbeGridConfig {
        double start_dx;
        double start_dy;
        double grid_step_x;
        double grid_step_y;

        ProbeGridConfig(job::RenderJob::JobSpecs specs) {
            grid_step_x = (specs.width * specs.pixelStepX) / static_cast<double>(PROBE_MCOL);
            grid_step_y = (specs.height * specs.pixelStepY) / static_cast<double>(PROBE_MROW);
            start_dx = -(specs.width / 2.0) * specs.pixelStepX;
            start_dy = -(specs.height / 2.0) * specs.pixelStepY;
        }
    };

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
        ComplexDouble C;    //third  order derivative (ErrorCheck)
    };

    /**
     * @brief: struct which stores the orbit cache of the initial computed
     * reference and the probe matrix (maybe unused but accounts for only
     * 36 kbytes)
     */
    struct InitialOrbit {
        util::LazyVector<ComplexDouble> orbitCache;
        std::array<unsigned int, PROBE_MROW * PROBE_MCOL> probeMatrix;
    };

    /**
     * @brief: struct which stores the per-worker local data to allow
     * for atomic pointer setting
     * * @note: the flags are needed since in between 2 jobs any thread
     * which successfuly published any part of its local structure and 
     * made it public has to wait that all threads have stopped looking
     * at it (or a thread could get stucked behind and if LazyVector 
     * reallocates it will be looking at garbage memory)
     */
    struct WorkerLocal {
        bool wonFirstOrbitSet{false};
        bool wonLastOrbitSet{false};
        InitialOrbit initOrbit;
        util::LazyVector<OrbitRecord> finalOrbitCache;

        /**
         * @brief: init the workerLocal fields given an amount of iterations
         */
        void init(unsigned int iter) {
            //we account for one more iteration to also store the points
            initOrbit.orbitCache.init(iter + 1);
            finalOrbitCache.init(iter + 1);
            wonFirstOrbitSet = false;
            wonLastOrbitSet  = false;
        }

        /**
         * @brief: attempt publishing the initialOrbit, setting a flag if successful
         * @returns: the oldest published orbit, ours if successful, another thread
         * if not
         */
        InitialOrbit* publishInitialOrbit(job::RenderJob::PTBJob& job_state) {
            void* init_orbit = nullptr;
            if(job_state.initialOrbitData.compare_exchange_strong(
                init_orbit,
                static_cast<void*>(&initOrbit),
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
                wonFirstOrbitSet = true;
                return &initOrbit;
            }
            return static_cast<InitialOrbit*>(init_orbit);
        }

        /**
         * @brief: attempts publishing the finalOrbit, setting a flag if successful
         * @returns: the oldest published orbit, ours if successful, another thread
         * if not
         */
        util::LazyVector<OrbitRecord>* publishFinalOrbit(job::RenderJob::PTBJob& job_state) {
            void* final_orbit = nullptr;
            if(job_state.finalOrbitData.compare_exchange_strong(
                final_orbit,
                static_cast<void*>(&finalOrbitCache),
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
                wonLastOrbitSet = true;
                return &finalOrbitCache;
            }
            return static_cast<util::LazyVector<OrbitRecord>*>(final_orbit); 
        }

        /**
         * @brief attempts to compute the initial orbit and publish it
         * @returns: the pointer to the published initial orbit, nullptr
         * if the job was aborted
         */
        InitialOrbit* computeInitialOrbit(
            job::RenderJob& job,
            BigFloat x0, BigFloat y0, unsigned int iterations
        ) {
            InitialOrbit* retval = nullptr;
            //get the jobState in order to check the publication
            auto& state    = job.getState<job::PerturbationJob>();
            auto& orbitVec = initOrbit.orbitCache;

            //push the point as 0-th iteration of the set
            orbitVec.emplace_back(0.0, 0.0);
            BigFloat z_real = 0, z_imag = 0, z_real_sq = 0, z_imag_sq = 0;

            //For each iteration we check
            // 1.iteration count
            // 2.escape condition
            // 3.nobody else setted retval
            for(
                unsigned int i = 0; 
                i < iterations && static_cast<double>(z_real_sq) + static_cast<double>(z_imag_sq) <= 4.0 && retval == nullptr; 
                i++
            ) {
                if((i & 63) == 0) {
                    retval = static_cast<InitialOrbit*>(state.initialOrbitData.load(std::memory_order_acquire));
                    if(job.aborted()) return nullptr;
                }
                
                z_imag = 2.0 * z_real * z_imag + y0;
                z_real = z_real_sq - z_imag_sq + x0;
                z_real_sq = z_real * z_real;
                z_imag_sq = z_imag * z_imag;
                
                orbitVec.emplace_back(static_cast<double>(z_real), static_cast<double>(z_imag));
            }

            //we publish only if we're done
            return retval == nullptr ? publishInitialOrbit(state) : retval;
        }


        /**
         * @brief: computes the final orbit and publishes it using high-precision math
         * @param: reference to the job to check for abortion and set the orbit data
         * @param: x0,y0: the coordinates of the reference orbit
         * @returns: the pointer to the published final orbit, nullptr if the job was aborted
         * @note: Aligned to reference math. finalOrbitCache[0] holds baseline ground state.
         */
        util::LazyVector<OrbitRecord>* computeFinalOrbit(
            const job::RenderJob& job, 
            job::RenderJob::PTBJob& jobState,
            BigFloat x0, 
            BigFloat y0,
            unsigned int iterations
        ) {
            using FinalOrbitCache = util::LazyVector<OrbitRecord>;
            FinalOrbitCache* retval = nullptr;
            
            // Local track variables starting at the true mathematical 0-th baseline
            ComplexDouble Z{0.0}, A{0.0}, B{0.0}, C{0.0};
            finalOrbitCache.emplace_back(Z, A, B, C);
    
            BigFloat z_real = 0, z_imag = 0, z_real_sq = 0, z_imag_sq = 0;
    
            for (unsigned int i = 0; 
                i < iterations && (Z.real() * Z.real() + Z.imag() * Z.imag()) <= 4.0 && retval == nullptr;
                i++
            ) {
                if ((i & 63) == 0) {
                    retval = static_cast<FinalOrbitCache*>(jobState.finalOrbitData.load(std::memory_order_acquire));
                    if (job.aborted() || retval != nullptr) break;
                }
                //compute the derivative with Z_i-1
                ComplexDouble next_A = (Z * A) * 2.0 + ComplexDouble{1.0, 0.0};
                ComplexDouble next_B = (Z * B) * 2.0 + (A * A);
                ComplexDouble next_C = (Z * C) * 2.0 + (A * B) * 2.0;
                
                //advance Z
                z_imag = 2.0 * z_real * z_imag + y0;
                z_real = z_real_sq - z_imag_sq + x0;
                z_real_sq = z_real * z_real;
                z_imag_sq = z_imag * z_imag;
                
                //commit the new Z
                Z = ComplexDouble(static_cast<double>(z_real), static_cast<double>(z_imag));
                A = next_A; 
                B = next_B; 
                C = next_C;
                finalOrbitCache.emplace_back(Z, A, B, C);
            }
            //we publish only if we're done
            return retval == nullptr ? publishFinalOrbit(jobState) : retval;
        }


        /**
         * @brief: evaluates the delta probing of a complex point dc using the cache 
         * array of the provided job
         * @prarm: const reference to the job (only checked for abortion)
         * @param: const reference to the orbit cache
         * @param: unsigned int iterations
         * @returns: the escape time of the point in reference to the center or nullopt if
         * job aborted
         */
        static std::optional<unsigned int> evalDeltaProbe(
            ComplexDouble dc, 
            const job::RenderJob& job, 
            const util::LazyVector<ComplexDouble>& refCache, 
            unsigned int iterations
        ) {
            double dx = 0.0, dy = 0.0, dx2 = 0.0, dy2 = 0.0;
            unsigned int size = refCache.size();
    
            // Safety bounds tracking actual computed limits
            for (unsigned int i = 0; i < size && i < iterations; ++i) {
                if (job.aborted()) {
                    return std::nullopt;
                }
                double zr = refCache[i].real();
                double zi = refCache[i].imag();
    
                // Check boundary conditions matching reference math exactly
                if ((zr + dx) * (zr + dx) + (zi + dy) * (zi + dy) > 4.0) return i;
    
                double next_dx = 2.0 * (zr * dx - zi * dy) + dx2 - dy2 + dc.real();
                double next_dy = 2.0 * (zr * dy + zi * dx) + 2.0 * dx * dy + dc.imag();
    
                dx = next_dx; 
                dy = next_dy;
                dx2 = dx * dx; 
                dy2 = dy * dy;
            }
            
            return size; 
        }


        /**
         * @brief: compute a chunk of the shared delta probe matrix
         */
        static bool computeProbeChunk(
            const job::RenderJob& job, //only to pass to the eval function for job abortion
            const ProbeGridConfig& config,
            InitialOrbit& refOrbit,
            unsigned int iterations,
            size_t chunk_idx
        ) {
            puts("PROBING");
            // Calculate the linear bounds for this specific chunk
            size_t start_cell = chunk_idx * PROBE_CELL_CHUNK;
            size_t total_cells = PROBE_MROW * PROBE_MCOL;
            size_t end_cell = std::min(start_cell + PROBE_CELL_CHUNK, total_cells);
    
            // Process the contiguous slice of cells allocated to this chunk
            for (size_t cell_idx = start_cell; cell_idx < end_cell; ++cell_idx) {
                // Deconstruct the 1D index into true 2D matrix coordinates
                int row = static_cast<int>(cell_idx / PROBE_MCOL);
                int col = static_cast<int>(cell_idx % PROBE_MCOL);
    
                // Compute delta offsets relative to the reference coordinate center
                double test_dx = config.start_dx + (col * config.grid_step_x);
                double test_dy = config.start_dy + (row * config.grid_step_y);
    
                // Run the adjusted orbit evaluation
                auto iter = evalDeltaProbe(ComplexDouble{test_dx, test_dy}, job, refOrbit.orbitCache, iterations);
                if(!iter.has_value()) return false;
                
                // commit to the matrix
                refOrbit.probeMatrix[cell_idx] = *iter;
            }

            return true;
        }

    };

    //reference to the MandelbrotEngine backBuffer
    util::FrameBuffer& backBuffer;
    const util::Gradient& gradient;
    //index for next slot in workerDataForDealloc
    std::atomic<size_t> dealloc_idx__{0};
    //contious storage for workerData for deallocation
    std::array<WorkerLocal*,MAX_WORKERS> dealloc__;

    WorkerLocal& getWorkerData() {
        static thread_local WorkerLocal* ptr = [this]() {
            assert(my_idx < MAX_WORKERS && "Exceeded MAX_WORKERS allocation limits!");
            WorkerLocal* allocated = new WorkerLocal();
            //register for deallocation
            dealloc__[dealloc_idx__.fetch_add(1ul, std::memory_order_relaxed)] = allocated;
            return allocated;
        }();

        return *ptr;
    }

    /**
     * @brief Processes a single chunk of the screen using Perturbation Theory and Taylor Series Approximation.
     * @param job_ref Reference to the current job (used exclusively to check for early abortion).
     * @param specs The job specifications (viewport, iterations, etc).
     * @param chunk_idx The linear index of the chunk this thread is assigned to compute.
     * @param finalOrbitCache The high-precision reference orbit.
     * @param best_dx Elected golden reference delta X.
     * @param best_dy Elected golden reference delta Y.
     */
    void processChunkScalar(
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
        core::Pixel* const __restrict__ raw_canvas = backBuffer.data();

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


    /**
     * @brief: returns the distance from the center of the winner point 
     * from the delta probing
     * * @note: the winner is selected amongst all probeMatrix based on the
     * the biggest escape time (which still converges) so we dont consider
     * best iter = max_iterations 
     */
    static ComplexDouble getProbeWinnerDelta(
        const std::array<unsigned int, PROBE_MROW * PROBE_MCOL> probeMatrix,
        ProbeGridConfig cfg,
        unsigned int max_iterations
    ) {

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

public:

    /**
     * @brief: waiting function for the caller
     * @note: for each perturbationJob a thread gets elected as the host of the 
     * reference orbits to compute the rendering. This means that other threads 
     * reference and access local data structures. Since in rare cases (though may
     * happen) LazyVectors inside the local structs could reallocate, if a thread
     * won any race, it has to wait that the other threads stop referencing its 
     * data before getting another job.
     * * @note: at the worst case, deltaProbing enabled and distinct threads which 
     * won the initialOrbit set and finalOrbit set, at most 2 threads have to wait 
     * that all the other threads are done
     */
    bool hasToWait() {
        const WorkerLocal& local = getWorkerData();
        return local.wonFirstOrbitSet || local.wonLastOrbitSet;
    }
    

    PerturbationEngine(util::FrameBuffer& fbuffer, const util::Gradient& gradient):
        backBuffer(fbuffer), gradient(gradient) {}


    /**
     * @brief: wait-free processing method
     * * @note: a perturbationJob is handled as follows:
     * 0. threads precompute all necessary data they need to synchronize and
     * perform the job
     * (if delta probe is not enabled skip to 5)
     * 1. Threads all compute the reference orbit cache in respect to the screen size
     * 2. (SYNC) the first thread which computes it, makes it public to the others
     * 3. Threads execute delta probing to compute the iterations of a 48x48 matrix of 
     * points centered
     * 4. When chunk delta probing is done, each thread searches the matrix and select
     * the best reference orbit
     * 5. Threads compute the OrbitCache (with Derivatives) in respect to the best reference
     * (reference is elected as the best reference during delta probing or as the center of
     * the screen if no delta probing)
     * 6. (SYNC) the first thread which computes it, makes it public to the others
     * 7. Threads chunk process the whole screen to compute the frame buffer
     * * @note: if delta probing is enabled, we compute the points that sit in the probing
     * matrix twice (first in the delta probing and secondly in the rendering). THis is 
     * acceptable since the second computation leverages the taylor approximation of the 
     * mandelbrot function to skip ALL the iterations of the points in the matrix: guaranteed
     * by how we select the best reference in the probing phase
     */
    void processPerturbationJob(job::RenderJob& job) {
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

    static inline size_t CalculateTotalChunks(unsigned int width, unsigned int height) noexcept {
        return core::CalculateTotalChunks(width, height);
    }
};

} // namespace engine