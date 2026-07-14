#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <optional>
#include "../../macro_util.hpp" // Assumes CACHE_ALIGN and CACHE_PAD_SIZE are defined here

namespace engine::job {

    /**
     * @brief PerturbationJob struct
     * * The perturbationJob processes chunks as follows: there are at most 4 phases:
     * 1. quickValidation: based on the position offset of the current frame from the 
     * reference point we precompute a tolerance (allows us to skip the slowValidation
     * if this is a success we skip to the render phase)
     * 2. slowValidation: threads render 1/16th of the pixel of the screen trying to detect
     * glitches which would trigger the rebase phase 
     * 3. rebase: cache has to be thrashed, so an iterative neighbourhood search is performed
     * to find the best reference point
     * 4. renderPhase: we can jump here from 1, 2, or 3, we render the final frame using a new 
     * cache or an old one
     * * Since the job is cross-phase we get an implicit lock on the job (which would prevent its trashing)
     * by booking one renderFrame, if the job is aborted prior to the first frame rendering we ought to 
     * mark the frame as completed before exiting early
     * * 1. QuickValidation: a simple deterministic computation which can be performed by all threads in isolation
     * if the cache is not quick valid then all thread will fail the QuickValidation
     * * 2. SlowValidation: all threads render 1/16th of the image looking for glitches, they signal glitches to 
     * eachother by setting a special flag (62nd bit) via TestAndSet. The thread which actually sets the flag also
     * estinguish the validation chunks. The thread which commits the last chunk is elected as the leader for computing the
     * initial rebase reference point
     * * 3. Rebase: it's an iterative process, which happens for a few iteration (less than 100 i think). For each iteration threads
     * populate a matrix (9x9 current coordinate screen division) with the High precision mandelbrot iteration. After all threads are
     * done, the thread setting the last chunk for the current iteration is elected as leader, for aggregating the results and choosing
     * the next reference point. When it is done, it publishes it in the cache field, resetting the rebaseIdx to 0, while the other threads 
     * wait on the rebaseDone counter. The thread increments the counter then notifies all threads. Since threads cannot keep track of the current
     * iteration we embed it as 4-5 bits of the rebaseIdx counter, when we reset to 0 we also increment the iteration counter (which is shadowed when getting)
     * the index, when maxIteration is reached, threads will exit becasue the maxIteration will be published
     * * rebaseDone counter:
     * Considering a 9x9 matrix we have 81 chunks, but we also account for 2 more chunks for each iteration, for rebase pending (while the leader is aggregating
     * or computing) and rebaseReady (leader increments by one signaling all threads to continue). Threads explicitly stop waiting if the job is aborted
     */
    struct PerturbationJob {
    public:
        // --- Global State Flags ---
        static constexpr uint64_t ABORT_FLAG  = 1ULL << 63;                 // Abort flag for all doneCounters
        static constexpr uint64_t REBASE_FLAG = 1ULL << 62;                 // Need rebase for the validationPhase
        
        // --- Bitmasks ---
        static constexpr uint64_t VAL_CHUNK_MASK    = ~(ABORT_FLAG | REBASE_FLAG);
        static constexpr uint64_t STD_CHUNK_MASK    = ~ABORT_FLAG;          // For Rebase & Render
        
        // --- Rebase Iteration Extraction ---
        static constexpr uint64_t REBASE_ITER_MASK  = 0xFFFFF00000000000;   // 20 bits to express the iteration count
        static constexpr uint64_t REBASE_CHUNK_MASK = ~REBASE_ITER_MASK;    // Extract the chunk of an iteration

    private:
        /**
         * We mutually pad the idx counters since they're used in mutually exclusive phases.
         * The renderIdx acts as our upfront implicit memory lock ticket.
         */
        CACHE_ALIGN std::atomic<uint64_t> valIdx{0};
        std::atomic<uint64_t> rebaseIdx{0};
        std::atomic<uint64_t> renderIdx{0};
        
        CACHE_ALIGN std::atomic<uint64_t> valDone{0};
        std::atomic<uint64_t> rebaseDone{0};
        std::atomic<uint64_t> renderDone{0};
        
        // Explicitly pad the remaining 24 bytes (3 * 8 bytes) of the 64-byte cache line 
        // belonging to the completion counters to prevent false sharing.
        CACHE_PAD_SIZE(sizeof(std::atomic_uint64_t) * 3);

    public:
        void* rebaseCache = nullptr;
        const size_t validation_chunks;     
        const size_t rebase_phase_chunks;   // Also accounts for flag barriers (e.g. 81 + 2)
        const size_t render_chunks;
        const uint16_t rebase_iterations;

        /**
         * @brief Constructs a new Perturbation Job.
         * @param v_chunks Total chunks for the SlowValidation phase.
         * @param r_phase_chunks Total chunks per single Rebase iteration (matrix + barriers).
         * @param f_chunks Total chunks for the Final Render phase.
         * @param r_iters Total number of iterations for the Rebase phase.
         */
        PerturbationJob(size_t v_chunks, size_t r_phase_chunks, size_t f_chunks, uint16_t r_iters)
            : validation_chunks(v_chunks), rebase_phase_chunks(r_phase_chunks), 
              render_chunks(f_chunks), rebase_iterations(r_iters) {}

        // Deleted copy semantics (atomics cannot be copied)
        PerturbationJob(const PerturbationJob&) = delete;
        PerturbationJob& operator=(const PerturbationJob&) = delete;

        // ==========================================
        // 1. LIFECYCLE & STATUS METHODS
        // ==========================================

        /**
         * @brief Checks if the job was successfully completed without aborting.
         * @return True if the render phase is fully finished and no abort occurred.
         */
        inline bool completed() const {
            return renderDone.load(std::memory_order_acquire) == render_chunks;
        }

        /**
         * @brief Checks if the job has been flagged for abortion.
         * @return True if the global ABORT_FLAG is set.
         */
        inline bool aborted() const {
            return (renderDone.load(std::memory_order_acquire) & ABORT_FLAG) != 0;
        }

        /**
         * @brief Determines if the job memory is completely safe to recycle/destroy.
         * @details Because threads use the render chunk as an upfront ticket to lock the job, 
         * the job is logically and physically done ONLY when the renderDone counter has caught up 
         * to the total render_chunks, guaranteeing zero ghost threads remain.
         * @return True if all threads have completely drained out of the job.
         */
        inline bool done() const {
            uint64_t f_state = renderDone.load(std::memory_order_acquire);
            return (f_state & STD_CHUNK_MASK) == render_chunks;
        }

        /**
         * @brief Forcefully aborts all phases of the job.
         * @details Instantly drains all unclaimed chunks from the indices and artificially 
         * commits them to the done counters with the MSB set. Also broadcasts to all waiting barriers.
         */
        void abort() {
            auto drain_pipeline = [](std::atomic<uint64_t>& idx, std::atomic<uint64_t>& done, uint64_t total, bool is_rebase) {
                uint64_t claimed = idx.exchange(total, std::memory_order_acq_rel);
                if (is_rebase) claimed &= REBASE_CHUNK_MASK; // Strip iteration bits before math
                uint64_t unclaimed = (total > claimed) ? (total - claimed) : 0;
                done.fetch_add(unclaimed | ABORT_FLAG, std::memory_order_release);
                done.notify_all();
            };

            drain_pipeline(renderIdx, renderDone, render_chunks, false);
            drain_pipeline(valIdx, valDone, validation_chunks, false);
            drain_pipeline(rebaseIdx, rebaseDone, rebase_phase_chunks * rebase_iterations, true);
        }

        // ==========================================
        // 2. INTERNAL SYNCHRONIZATION (WAITERS)
        // ==========================================

        /**
         * @brief Generic lock-free wait utility that parks the thread using std::atomic::wait.
         * @param target_atomic The atomic counter to evaluate and wait on.
         * @param target_val The minimum chunk count required to break the wait.
         * @param mask The bitmask applied to strip state flags before evaluation.
         * @return True if the target was reached, False if the job was aborted while waiting.
         */
        inline bool wait_until_at_least(std::atomic<uint64_t>& target_atomic, uint64_t target_val, uint64_t mask) {
            uint64_t state = target_atomic.load(std::memory_order_acquire);
            
            while ((state & mask) < target_val) {
                if ((state & ABORT_FLAG) != 0) return false;
                
                target_atomic.wait(state, std::memory_order_acquire);
                state = target_atomic.load(std::memory_order_acquire);
            }
            return ((state & ABORT_FLAG) == 0);
        }

        // ==========================================
        // 3. CHUNK CLAIMING (GETTERS)
        // ==========================================

        /**
         * @brief Claims a single chunk from the SlowValidation phase.
         * @return The 0-based index of the validation chunk, or std::nullopt if exhausted.
         */
        inline std::optional<size_t> claim_validation_chunk() {
            uint64_t idx = valIdx.fetch_add(1, std::memory_order_relaxed);
            if (idx < validation_chunks) return idx;
            return std::nullopt;
        }

        /**
         * @brief Claims a single chunk from the iterative Rebase phase.
         * @details The returned value inherently encodes the iteration. Use REBASE_CHUNK_MASK to extract the local matrix index.
         * @return The encoded index of the rebase chunk, or std::nullopt if exhausted.
         */
        inline std::optional<size_t> claim_rebase_chunk() {
            uint64_t total_rebase = rebase_phase_chunks * rebase_iterations;
            uint64_t idx = rebaseIdx.fetch_add(1, std::memory_order_relaxed);
            if (idx < total_rebase) return idx;
            return std::nullopt;
        }

        /**
         * @brief Claims a single chunk from the final Render phase.
         * @details This function MUST be called upfront by all threads interacting with this job to acquire 
         * the implicit memory lock, even if the job aborts before rendering begins.
         * @return The 0-based index of the render chunk, or std::nullopt if all tickets are claimed.
         */
        inline std::optional<size_t> claim_render_chunk() {
            uint64_t idx = renderIdx.fetch_add(1, std::memory_order_relaxed);
            if (idx < render_chunks) return idx;
            return std::nullopt;
        }

        // ==========================================
        // 4. WORKER REPORTING & SYNCHRONIZATION (SETTERS)
        // ==========================================

        /**
         * @brief Reports a validation chunk as completed and handles Rebase glitch flagging.
         * @details If a glitch is found, sets the REBASE_FLAG. The FIRST thread to do so steals 
         * all remaining validation chunks and artificially commits them to trigger the barrier early.
         * @param glitch_found Boolean indicating if float divergence was detected.
         * @return True ONLY if the calling thread pushed the validation phase to completion (Leader Election).
         */
        inline bool report_validation_chunk(bool glitch_found) {
            uint64_t chunks_to_commit = 1; 
            
            if (glitch_found) {
                uint64_t prev = valDone.fetch_or(REBASE_FLAG, std::memory_order_acq_rel);
                
                // If I am the FIRST thread to report a glitch, steal the remaining work
                if ((prev & REBASE_FLAG) == 0) {
                    uint64_t claimed = valIdx.exchange(validation_chunks, std::memory_order_relaxed);
                    uint64_t stolen = (validation_chunks > claimed) ? (validation_chunks - claimed) : 0;
                    chunks_to_commit += stolen;
                }
            }
            
            uint64_t prev_done = valDone.fetch_add(chunks_to_commit, std::memory_order_acq_rel);
            
            return ((prev_done & VAL_CHUNK_MASK) + chunks_to_commit == validation_chunks);
        }

        /**
         * @brief Awakens all threads parked at the validation barrier.
         * @details Exclusively called by the Elected Leader after preparing the initial Rebase state.
         */
        inline void broadcast_validation_complete() {
            valDone.notify_all();
        }

        /**
         * @brief Parks the follower thread until the entire Validation phase is complete.
         * @return True if a glitch was found and Rebase is required. False if skipped or aborted.
         */
        inline bool wait_for_validation_phase() {
            if (!wait_until_at_least(valDone, validation_chunks, VAL_CHUNK_MASK)) return false;
            return (valDone.load(std::memory_order_acquire) & REBASE_FLAG) != 0;
        }

        /**
         * @brief Reports a Rebase chunk/barrier as completed.
         * @details If the chunk mathematically completes a full iteration phase, it implicitly broadcasts 
         * to awaken followers parked at the iteration barrier.
         */
        inline void report_rebase_chunk() {
            uint64_t prev = rebaseDone.fetch_add(1, std::memory_order_acq_rel);
            
            // If this completes an iteration step (matrix + barriers), wake up waiting threads
            if (((prev & STD_CHUNK_MASK) + 1) % rebase_phase_chunks == 0) {
                rebaseDone.notify_all();
            }
        }

        /**
         * @brief Ensures a follower thread does not start iteration N until iteration N-1 has fully published.
         * @param required_iteration The 0-based iteration index that must be completed before proceeding.
         * @return True if safe to proceed, False if aborted while waiting.
         */
        inline bool wait_for_rebase_iteration(uint16_t required_iteration) {
            if (required_iteration == 0) return !aborted(); 
            uint64_t target_chunks = required_iteration * rebase_phase_chunks;
            return wait_until_at_least(rebaseDone, target_chunks, STD_CHUNK_MASK);
        }

        /**
         * @brief Reports a final Render chunk as completed and releases the implicit memory lock.
         * @details MUST be called exactly once for every successful claim_render_chunk() regardless of early aborts.
         */
        inline void report_render_chunk() {
            renderDone.fetch_add(1, std::memory_order_release);
        }
    };

} // namespace engine::job