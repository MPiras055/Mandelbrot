#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include "core/Numeric.hpp"
#include "../../macro_util.hpp"

namespace engine::job {

// forward declaration for the friend specifier
struct RenderJob;

/**
 * @brief Phased perturbation job.
 *
 * A perturbation frame runs in up to three work-stealing phases, coordinated purely
 * by three monotonic `(idx, done)` atomic pairs — no mutexes, no OS barriers:
 *
 *   1. VALIDATION — sample 1/16 of the screen with the current (persistent) cache and
 *      look for glitches. The `done` counter reserves a GLITCH_FOUND bit; the last
 *      committer wakes everyone (validation barrier) and the result routes each thread
 *      to render (clean) or rebase (glitch).
 *   2. REBASE — iterative 9x9 neighbourhood search; the matrix cell index carries the
 *      search iteration in its high 32 bits, and a per-iteration barrier separates the
 *      parallel matrix compute from the single-thread aggregation.
 *   3. RENDER — per-pixel work-stealing; the render pair is also the job's lifetime
 *      gate (a thread books a render chunk up front, so the slot can't recycle until
 *      every worker has drained out).
 *
 * Layout: all `*Idx` counters share one cache line and all `*Done` counters the next,
 * since the phases are mutually exclusive — this avoids cross-phase false sharing.
 *
 * @note Transitional: the legacy `getProbeChunk/markCompletedProbe/probeCompleted/
 * getChunk/markCompletedChunk` methods are thin adapters over the validation/render
 * pairs so the current engine keeps compiling & rendering while the 3-phase engine is
 * built out. They will be removed once the engine adopts the phase API below.
 */
struct PerturbationJob {
    using ComplexDouble = core::ComplexDouble;

    // --- bit protocol carried on the *Done counters ---
    static constexpr uint64_t ABORT_FLAG   = 1ull << 63;   // all done counters (drained by abort)
    static constexpr uint64_t GLITCH_FOUND = 1ull << 62;   // validation only
    static constexpr uint64_t VAL_MASK     = ~(ABORT_FLAG | GLITCH_FOUND);
    static constexpr uint64_t STD_MASK     = ~ABORT_FLAG;

    // --- rebase index encoding: [ iteration : 32 | matrix cell : 32 ] ---
    static constexpr uint64_t REBASE_ITER_SHIFT = 32;
    static constexpr uint64_t REBASE_CELL_MASK  = 0xFFFFFFFFull;

    // --- resolve index encoding: [ round : 32 | range : 32 ] (mirrors rebase) ---
    static constexpr uint64_t RESOLVE_ROUND_SHIFT = 32;
    static constexpr uint32_t RESOLVE_TERMINAL     = 0xFFFFFFFFu;   // "no more rounds"

    PerturbationJob()  noexcept = default;
    ~PerturbationJob() noexcept {}
    PerturbationJob(const PerturbationJob&) = delete;
    PerturbationJob& operator=(const PerturbationJob&) = delete;

    // ---- claim counters (one cache line) ----
    CACHE_ALIGN std::atomic<uint64_t> valIdx{0};
    std::atomic<uint64_t> rebaseIdx{0};
    std::atomic<uint64_t> renderIdx{0};
    char pad__[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>) * 3];
    // ---- completion counters (next cache line) ----
    CACHE_ALIGN std::atomic<uint64_t> valDone{0};
    std::atomic<uint64_t> rebaseDone{0};
    std::atomic<uint64_t> renderDone{0};
    char pad1__[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>) * 3];
    std::atomic<void*> rebaseMatrix{nullptr}; //no need for padding only setted one time

    // Reference-cache publication: the frame's leader builds the reference into its own
    // worker-local ReferenceCache and publishes the POINTER here; followers park until
    // it's non-null, then render against it. `nullptr` = not yet published;
    // CACHE_ABORT = the job aborted before a cache was published.
    inline static void* const CACHE_ABORT = reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1));
    std::atomic<void*> refCache{nullptr};

    // ---- Phase 4: RESOLVE (frame-global glitch resolution) ----
    // A separate work-stealing phase that runs AFTER render: stranded pixels (recorded in
    // an engine-owned global buffer) are re-perturbed against ONE shared secondary
    // reference per round, so a glitched blob is resolved coherently (no per-tile seams).
    // Round readiness is published in `resolveReady` (= highest ready round + 1;
    // RESOLVE_TERMINAL once finished); within a round, ranges are claimed via `resolveIdx`
    // and counted in `resolveDone` exactly like the rebase pair. `resolveComplete` gates
    // job completion so a frame is never harvested mid-resolution.
    CACHE_ALIGN std::atomic<uint64_t> resolveIdx{0};   // [round:32 | range:32]
    std::atomic<uint64_t> resolveDone{0};              // ranges reported (per-round, mod range_total)
    std::atomic<uint32_t> resolveReady{0};             // highest ready round + 1 (0 = none)
    std::atomic<bool>     resolveComplete{false};      // resolution finished (or aborted)

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

    /// True once every render ticket is accounted for (completed or abort-drained) AND the
    /// resolution phase has finished: the slot is then safe to recycle and the frame safe
    /// to harvest (never mid-resolution).
    bool done(size_t render_total) const noexcept {
        return ((renderDone.load(std::memory_order_acquire) & STD_MASK) == render_total)
            && resolveComplete.load(std::memory_order_acquire);
    }

    /// True when the render phase finished cleanly (no abort bit) and resolution is done.
    bool completed(size_t render_total) const noexcept {
        return (renderDone.load(std::memory_order_acquire) == render_total)
            && resolveComplete.load(std::memory_order_acquire);
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
    // PHASE 1 — VALIDATION (validation pair; GLITCH_FOUND bit)
    // =========================================================================

    [[nodiscard]] bool claimValidation(size_t total, size_t& out) noexcept {
        const uint64_t i = valIdx.fetch_add(1, std::memory_order_acq_rel);
        if (i >= total) return false;
        out = static_cast<size_t>(i);
        return true;
    }

    /// Commit a validation chunk. On the first glitch, raise GLITCH_FOUND and steal the
    /// remaining chunks so the barrier trips early.
    /// @return true iff this call pushed the validation phase to completion (leader).
    bool reportValidation(size_t total, bool glitch) noexcept {
        uint64_t commit = 1;
        if (glitch) {   //this thread found a glitch
            const uint64_t prev = valDone.fetch_or(GLITCH_FOUND, std::memory_order_acq_rel);
            if ((prev & GLITCH_FOUND) == 0) {                       // first to glitch
                const uint64_t claimed = valIdx.exchange(total, std::memory_order_acq_rel);
                if (total > (claimed & STD_MASK)) commit += total - (claimed & STD_MASK);
            }
        }
        const uint64_t prev = valDone.fetch_add(commit, std::memory_order_acq_rel);
        const bool last = ((prev & VAL_MASK) + commit) == total;
        if (last) valDone.notify_all();
        return last;
    }

    enum class Validation { PROCEED, REBASE, ABORTED };

    /// Barrier: park until validation finishes (or the job aborts). Routes the caller
    /// to render (PROCEED), rebase (REBASE), or exit (ABORTED).
    Validation waitValidation(size_t total) noexcept {
        uint64_t s = valDone.load(std::memory_order_acquire);
        while ((s & VAL_MASK) < total) {
            if (s & ABORT_FLAG) return Validation::ABORTED;
            valDone.wait(s, std::memory_order_acquire);
            s = valDone.load(std::memory_order_acquire);
        }
        if (s & ABORT_FLAG)   return Validation::ABORTED;
        if (s & GLITCH_FOUND) return Validation::REBASE;
        return Validation::PROCEED;
    }

    // =========================================================================
    // PHASE 2 — REBASE (rebase pair; iteration in high 32 bits of rebaseIdx)
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

    // =========================================================================
    // PHASE 4 — RESOLVE (resolve pair + round-readiness signal)
    // =========================================================================

    /// Barrier: spin until every render ticket is accounted for (so the engine's global
    /// unresolved buffer is complete) before the resolution phase reads it.
    /// @return false if the job aborted.
    bool waitRenderComplete(size_t render_total) noexcept {
        while ((renderDone.load(std::memory_order_acquire) & STD_MASK) < render_total) {
            if (aborted()) return false;
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
        return !aborted();
    }

    /// Claim a range of resolve round @p expect_round. Ranges 0..range_total-1 partition the
    /// round's unresolved list (empty ranges still claim/report to keep the barrier count).
    /// CAS-bounded to @p expect_round so a worker never over-claims into the next round after
    /// the aggregator advances (which would corrupt that round's buffers / barrier count).
    /// @return false when this round is exhausted / already advanced / the job aborted.
    [[nodiscard]] bool claimResolve(uint32_t expect_round, size_t range_total, size_t& range_out) noexcept {
        uint64_t v = resolveIdx.load(std::memory_order_acquire);
        for (;;) {
            if (v & ABORT_FLAG) return false;
            if (static_cast<uint32_t>(v >> RESOLVE_ROUND_SHIFT) != expect_round) return false;
            const uint64_t range = v & REBASE_CELL_MASK;
            if (range >= range_total) return false;
            if (resolveIdx.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                range_out = static_cast<size_t>(range);
                return true;
            }
            // v reloaded by compare_exchange_weak on failure; retry.
        }
    }

    /// Commit a resolve range. @return true iff it was the last range of the round
    /// (that thread becomes the aggregator: swap buffers / build the next secondary).
    bool reportResolve(size_t range_total) noexcept {
        const uint64_t prev = resolveDone.fetch_add(1, std::memory_order_acq_rel) & STD_MASK;
        return ((prev + 1) % range_total) == 0;
    }

    /// Aggregator: open round @p round for claiming (reset the range index) and publish it
    /// ready. The engine MUST have published that round's shared secondary (release) before
    /// calling this — the store below pairs with the acquire in waitResolveReady.
    void beginResolveRound(uint32_t round) noexcept {
        resolveIdx.store(static_cast<uint64_t>(round) << RESOLVE_ROUND_SHIFT, std::memory_order_release);
        resolveReady.store(round + 1, std::memory_order_release);
        resolveReady.notify_all();
    }

    /// Follower barrier: block until round @p round is ready (or resolution ends / aborts).
    /// @return true if round @p round is now claimable; false if the phase is over.
    bool waitResolveReady(uint32_t round) noexcept {
        uint32_t s = resolveReady.load(std::memory_order_acquire);
        while (s <= round) {
            if (resolveComplete.load(std::memory_order_acquire) || aborted()) return false;
            resolveReady.wait(s, std::memory_order_acquire);
            s = resolveReady.load(std::memory_order_acquire);
        }
        return (s != RESOLVE_TERMINAL) && !aborted();
    }

    /// Aggregator: the resolution phase is finished — unblock followers and let the job
    /// complete. Idempotent.
    void markResolveComplete() noexcept {
        resolveComplete.store(true, std::memory_order_release);
        resolveReady.store(RESOLVE_TERMINAL, std::memory_order_release);
        resolveReady.notify_all();
    }

    bool isResolveComplete() const noexcept {
        return resolveComplete.load(std::memory_order_acquire);
    }

    /**
     * @brief: get the completion percentage of the current job
     * 
     * @returns: a number from 0 to 100 of the chunks processed in respect of the 
     * total chunks of the job
     * @note: doesn't check if the job has been aborted
     */
    unsigned int percentageStatus(size_t total_chunks) const noexcept {
        //proc : total_chunks = x : 100
        return ((renderDone.load(std::memory_order_relaxed) & STD_MASK) * 100) / total_chunks;
    }
    
    // =========================================================================
    // LEGACY ADAPTERS (transitional — remove once the engine uses the phase API)
    //   probe  -> validation pair   |   chunk -> render pair
    // =========================================================================

    

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
        //reset all the counters
        valIdx.store(       0, std::memory_order_relaxed);
        rebaseIdx.store(    0, std::memory_order_relaxed);
        renderIdx.store(    0, std::memory_order_relaxed);
        valDone.store(      0, std::memory_order_relaxed);
        rebaseDone.store(   0, std::memory_order_relaxed);
        renderDone.store(   0, std::memory_order_relaxed);
        //reset the resolve phase
        resolveIdx.store(     0,     std::memory_order_relaxed);
        resolveDone.store(    0,     std::memory_order_relaxed);
        resolveReady.store(   0,     std::memory_order_relaxed);
        resolveComplete.store(false, std::memory_order_relaxed);
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
        valIdx.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        valDone.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        valDone.notify_all();
        rebaseIdx.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        rebaseIdx.notify_all();   // followers now park on rebaseIdx, not rebaseDone
        rebaseDone.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        rebaseDone.notify_all();
        // wake anyone parked waiting for the reference-cache publication
        refCache.store(CACHE_ABORT, std::memory_order_release);
        refCache.notify_all();
        // poison + drain the resolve phase and unblock its barriers
        resolveIdx.fetch_or(ABORT_FLAG, std::memory_order_acq_rel);
        markResolveComplete();   // resolveComplete + resolveReady=TERMINAL + notify

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
