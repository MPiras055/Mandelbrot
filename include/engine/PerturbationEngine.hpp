#pragma once
#include <array>
#include <atomic>
#include <complex>
#include <optional>
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include "core/Numeric.hpp"
#include "core/Kernel.hpp"
#include "macro_util.hpp"
#include "util/ColorUtil.hpp"
#include "job/RenderJob.hpp"
#include "util/LazyVector.hpp"

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
    static constexpr size_t PROBE_MROW = 128;
    static constexpr size_t PROBE_MCOL = 128;
    static constexpr size_t PROBE_CELL_CHUNK = 128;
    static constexpr size_t MAX_WORKERS = 128;
    core::Pixel*& back_buf_ref;

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

    const util::Gradient& gradient;
    //index for next slot in workerDataForDealloc
    std::atomic<size_t> dealloc_idx__{0};
    //contious storage for workerData for deallocation
    std::array<WorkerLocal*,MAX_WORKERS> dealloc__;

    // --- Large driver methods (defined in PerturbationEngine.cpp) ---
    WorkerLocal& getWorkerData();

    void processChunkScalar(
        const job::RenderJob& job_ref,
        const job::RenderJob::JobSpecs& specs,
        const util::LazyVector<OrbitRecord>& finalOrbitCacheRef,
        const size_t chunk_idx,
        double best_dx,
        double best_dy);

    /**
     * @brief: returns the distance from the center of the winner point
     * from the delta probing (biggest converging escape time).
     */
    static ComplexDouble getProbeWinnerDelta(
        const std::array<unsigned int, PROBE_MROW * PROBE_MCOL> probeMatrix,
        ProbeGridConfig cfg,
        unsigned int max_iterations);

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


    explicit PerturbationEngine(const util::Gradient& gradient, core::Pixel*& back_buf_ref):
        gradient(gradient),back_buf_ref(back_buf_ref) {}

    /// Wait-free per-job driver (7-phase pipeline). Defined in the .cpp.
    void processPerturbationJob(job::RenderJob& job);

    static inline size_t CalculateTotalChunks(unsigned int width, unsigned int height) noexcept {
        return core::CalculateTotalChunks(width, height);
    }
};

} // namespace engine
