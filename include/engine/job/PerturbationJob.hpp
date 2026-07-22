#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include "core/Numeric.hpp"
#include "Counters.hpp"
#include "../../macro_util.hpp"

namespace engine::job {

// forward declaration for the friend specifier
struct RenderJob;

/**
 * @brief Phased perturbation job.
 *
 * A perturbation frame runs in two work-stealing phases, coordinated purely by
 * monotonic `(idx, done)` atomic pairs — no mutexes, no OS barriers:
 *
 *   1. REBASE — iterative neighbourhood search for the frame's reference point. The
 *      matrix cell index carries the search iteration in its high 32 bits, and a
 *      per-iteration barrier separates the parallel matrix compute from the
 *      single-thread aggregation. The leader then publishes the built reference
 *      through `refCache`; followers park in `waitCache` until it lands.
 *   2. RENDER — per-pixel work-stealing; the render pair is also the job's lifetime
 *      gate (a thread books a render chunk up front, so the slot can't recycle until
 *      every worker has drained out). `renderDone == chunks` IS the completion
 *      condition — workers leave as soon as the claim queue is empty, with no barrier.
 *
 * Layout: all `*Idx` counters share one cache line and all `*Done` counters the next,
 * since the phases are mutually exclusive — this avoids cross-phase false sharing.
 *
 * @note Rebasing (see `PerturbationEngine::processChunkScalar`) resolves every pixel
 * inline, so there is no post-render glitch-resolution phase.
 */
struct PerturbationJob {
    using ComplexDouble = core::ComplexDouble;

    // --- bit protocol carried on the *Done counters ---
    static constexpr uint64_t ABORT_FLAG   = 1ull << 63;   // all done counters (drained by abort)
    static constexpr uint64_t STD_MASK     = ~ABORT_FLAG;

    // --- rebase index encoding: [ iteration : 32 | matrix cell : 32 ] ---
    static constexpr uint64_t REBASE_ITER_SHIFT = 32;
    static constexpr uint64_t REBASE_CELL_MASK  = 0xFFFFFFFFull;

    PerturbationJob()  noexcept = default;
    ~PerturbationJob() noexcept {}
    PerturbationJob(const PerturbationJob&) = delete;
    PerturbationJob& operator=(const PerturbationJob&) = delete;

    // ---- claim counters (one cache line) ----
    CACHE_ALIGN std::atomic<uint64_t> rebaseIdx{0};
    std::atomic<uint64_t> renderIdx{0};
    char pad__[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>) * 2];
    // ---- completion counters (next cache line) ----
    CACHE_ALIGN std::atomic<uint64_t> rebaseDone{0};
    std::atomic<uint64_t> renderDone{0};
    char pad1__[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>) * 2];
    std::atomic<void*> rebaseMatrix{nullptr}; //no need for padding only setted one time

    // Reference-cache publication: the frame's leader builds the reference into its own
    // worker-local ReferenceCache and publishes the POINTER here; followers park until
    // it's non-null, then render against it. `nullptr` = not yet published;
    // CACHE_ABORT = the job aborted before a cache was published.
    inline static void* const CACHE_ABORT = reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1));
    std::atomic<void*> refCache{nullptr};

    /// 0..100 progress through the reference-orbit build, so `percentageStatus` can report
    /// the reference phase instead of sitting at 0 until render starts.
    std::atomic<uint32_t> refBuildPercent{0};

    /// Leader: publish the reference-cache pointer. CAS nullptr→cache; loses to abort.
    /// @return false if the job was aborted before publishing.
    bool publishCache(void* cache) noexcept {
        void* expected = nullptr;
        const bool won = refCache.compare_exchange_strong(expected, cache, std::memory_order_release);
        refCache.notify_all();
        return won;   // false => expected == CACHE_ABORT
    }

    /// Follower: block until the leader publishes the cache (or the job aborts).
    /// @return the published ReferenceCache* (as void*), or CACHE_ABORT.
    void* waitCache() noexcept {
        void* s = refCache.load(std::memory_order_acquire);
        while (s == nullptr) {
            refCache.wait(nullptr, std::memory_order_acquire);
            s = refCache.load(std::memory_order_acquire);
        }
        return s;
    }

    // =========================================================================
    // LIFECYCLE (called by RenderJob via std::visit; the render pair is the gate)
    // =========================================================================

    /// True once every render ticket is accounted for (completed or abort-drained): the
    /// slot is then safe to recycle and the frame safe to harvest.
    bool done(size_t render_total) const noexcept {
        return (renderDone.load(std::memory_order_acquire) & STD_MASK) == render_total;
    }

    /// True when the render phase finished cleanly (no abort bit set).
    bool completed(size_t render_total) const noexcept {
        return renderDone.load(std::memory_order_acquire) == render_total;
    }

    /// True once abort() has been issued on this job.
    bool aborted() const noexcept {
        return (renderDone.load(std::memory_order_acquire) & ABORT_FLAG) != 0;
    }

    // =========================================================================
    // PHASE 3 — RENDER (render pair)
    // =========================================================================

    [[nodiscard]] bool claimRender(size_t total, size_t& out) noexcept {
        const uint64_t i = renderIdx.fetch_add(1, std::memory_order_acq_rel);
        if (i >= total) return false;
        out = static_cast<size_t>(i);
        return true;
    }

    void reportRender() noexcept {
        renderDone.fetch_add(1, std::memory_order_release);
    }

    // =========================================================================
    // PHASE 1 — REBASE (rebase pair; iteration in high 32 bits of rebaseIdx)
    // =========================================================================

    /// Claim a matrix cell of the CURRENT rebase iteration.
    /// @param matrix_total number of cells per iteration (e.g. 9*9 = 81).
    /// @return false when this iteration's matrix is exhausted (caller waits / aggregates).
    [[nodiscard]] bool claimRebase(size_t matrix_total, uint32_t& iter_out, size_t& cell_out) noexcept {
        const uint64_t v = rebaseIdx.fetch_add(1, std::memory_order_acq_rel);
        iter_out = static_cast<uint32_t>(v >> REBASE_ITER_SHIFT);
        const uint64_t cell = v & REBASE_CELL_MASK;
        if (cell >= matrix_total) return false;
        cell_out = static_cast<size_t>(cell);
        return true;
    }

    /// Commit a matrix cell. @return true iff it was the last cell of the iteration
    /// (that thread is elected aggregator).
    bool reportRebase(size_t matrix_total) noexcept {
        const uint64_t prev = rebaseDone.fetch_add(1, std::memory_order_acq_rel) & STD_MASK;
        return ((prev + 1) % matrix_total) == 0;
    }

    /// Aggregator: publish the next search iteration — reset the cell index to
    /// [next_iter : 0] and wake the followers parked on the iteration barrier.
    /// @note This store is the ONLY signal that the current iteration's aggregation
    /// (shrinkAndCenter / cache publish) is complete: it happens-after all of that on
    /// the aggregator, and its release pairs with the acquire in waitRebaseIteration.
    void advanceRebase(uint32_t next_iter) noexcept {
        rebaseIdx.store(static_cast<uint64_t>(next_iter) << REBASE_ITER_SHIFT, std::memory_order_release);
        rebaseIdx.notify_all();
    }

    /// Follower barrier: block until the aggregator has published a search iteration
    /// strictly newer than `iteration` (i.e. advanceRebase stored `iteration+1` into
    /// the high bits of rebaseIdx). Waiting on rebaseIdx — not rebaseDone — is what
    /// makes this a real barrier: rebaseDone reaches its per-iteration total *before*
    /// the aggregator runs shrinkAndCenter/publish, so only the advanceRebase store
    /// safely gates followers against that work. @return false on abort.
    /// @note the acquire below synchronises-with advanceRebase's release, so on return
    /// the caller sees the aggregator's matrix writes and any cache it published.
    bool waitRebaseIteration(uint32_t iteration, size_t /*matrix_total*/) noexcept {
        uint64_t s = rebaseIdx.load(std::memory_order_acquire);
        while (((s & STD_MASK) >> REBASE_ITER_SHIFT) <= iteration) {
            if (s & ABORT_FLAG) return false;
            rebaseIdx.wait(s, std::memory_order_acquire);   // spurious wakeups from claimRebase are re-checked
            s = rebaseIdx.load(std::memory_order_acquire);
        }
        return (s & ABORT_FLAG) == 0;
    }

    /**
     * @brief Completion percentage of the whole job, 0..100.
     *
     * @details A perturbation frame is reference-build THEN render, and the build can be a
     * large share of the wall time at high iteration counts. Reporting render alone left the
     * bar pinned at 0 and then jumping, so the two phases share the range:
     *   - 0..REF_PHASE_PCT   — reference: probe cells, then the BigFloat orbit build.
     *   - REF_PHASE_PCT..100 — render chunks.
     * The probe runs an unknown number of shrink passes (the high-res search loops until the
     * step size is small enough), so its contribution is a bounded ramp rather than an exact
     * fraction — it can stall, but it never goes backwards and never overshoots the phase.
     */
    unsigned int percentageStatus(size_t total_chunks) const noexcept {
        static constexpr unsigned int REF_PHASE_PCT = 30;

        // Reference published? Then we are rendering.
        if (refCache.load(std::memory_order_relaxed) != nullptr) {
            const uint64_t done = renderDone.load(std::memory_order_relaxed) & STD_MASK;
            return REF_PHASE_PCT +
                   (counters::percentOf(done, total_chunks) * (100 - REF_PHASE_PCT)) / 100;
        }
        // Still building the reference: the orbit build reports real progress; before it
        // starts we are probing, which only gets the first slice of the phase.
        const uint32_t built = refBuildPercent.load(std::memory_order_relaxed);
        return (built * REF_PHASE_PCT) / 100;
    }



    protected:
    friend RenderJob;

    /**
     * @brief: the rebaseMatrix has to be handled by the leader for now either with hazard pointers or Idx
     */
    void reset(size_t render_total) noexcept {
        assert((render_total & ABORT_FLAG) == 0 && "render_total too high");
        //reset the rebaseMatrix + published reference-cache pointer
        rebaseMatrix.store(nullptr,std::memory_order_relaxed);
        refCache.store(nullptr, std::memory_order_relaxed);
        refBuildPercent.store(0, std::memory_order_relaxed);
        //reset all the counters
        rebaseIdx.store(    0, std::memory_order_relaxed);
        renderIdx.store(    0, std::memory_order_relaxed);
        rebaseDone.store(   0, std::memory_order_relaxed);
        renderDone.store(   0, std::memory_order_relaxed);
    }

    /**
     * @brief Force-abort every phase.
     * @details Poisons all three claim indices (future claims fail), commits the
     * unclaimed render tickets with ABORT_FLAG so done() converges, and wakes the two
     * phase barriers. @return true iff the render phase had not already completed.
     */
    bool abort(size_t render_total) noexcept {
        const uint64_t last = renderIdx.fetch_add(ABORT_FLAG, std::memory_order_acq_rel);

        // stop validation/rebase claims and wake their barriers
        rebaseIdx.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        rebaseIdx.notify_all();   // followers now park on rebaseIdx, not rebaseDone
        rebaseDone.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        rebaseDone.notify_all();
        // wake anyone parked waiting for the reference-cache publication
        // No valid pointer has the lsb setted to 1
        refCache.store(CACHE_ABORT,std::memory_order_release);
        refCache.notify_all();

        if ((last & STD_MASK) < render_total) {
            const uint64_t unclaimed = render_total - (last & STD_MASK);
            renderDone.fetch_add(unclaimed | ABORT_FLAG, std::memory_order_release);
            renderDone.notify_all();
            return true;
        }
        return false;
    }
};

} // namespace engine::job
