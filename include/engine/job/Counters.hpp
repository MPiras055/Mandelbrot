#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

/**
 * @file Counters.hpp
 * @brief The small pieces of the lock-free claim protocol that are genuinely identical
 * across job types.
 *
 * @details `EscapeTimeJob` and `PerturbationJob` each carried their own copy of the abort
 * bit, the plain `fetch_add` claim, the modular "was I last?" report, and the percentage
 * formula (down to a verbatim-copied doc comment, and the same missing divide-by-zero
 * guard). Those four are shared here.
 *
 * @note Deliberately NOT shared: the round-gated CAS claim, the glitch-steal report, and
 * `abort()`. Those have distinct shapes and per-phase ordering arguments; folding them
 * behind one signature would hide exactly the reasoning that has already caused bugs in
 * this code. Sharing is limited to operations with no ordering *intent* to lose.
 */
namespace engine::job::counters {

/// MSB of a done-counter, set when a phase is abort-drained.
inline constexpr uint64_t ABORT_BIT  = 1ull << 63;
/// Mask selecting the count itself.
inline constexpr uint64_t COUNT_MASK = ~ABORT_BIT;

/**
 * @brief Take the next work ticket. @return false once the queue is exhausted.
 * @note A plain `fetch_add` — monotonic, so an exhausted queue simply keeps failing.
 */
[[nodiscard]] inline bool claim(std::atomic<uint64_t>& idx, size_t total, size_t& out) noexcept {
    const uint64_t i = idx.fetch_add(1, std::memory_order_acq_rel);
    if (i >= total) return false;
    out = static_cast<size_t>(i);
    return true;
}

/**
 * @brief Commit one unit of work.
 * @return true iff this call completed a full round of @p total (the caller is the leader).
 */
inline bool report(std::atomic<uint64_t>& done, size_t total) noexcept {
    const uint64_t prev = done.fetch_add(1, std::memory_order_acq_rel) & COUNT_MASK;
    return ((prev + 1) % total) == 0;
}

/// Integer percentage, saturating at 100 and safe when @p total is 0.
inline unsigned int percentOf(uint64_t done, size_t total) noexcept {
    if (total == 0) return 100;
    const uint64_t pct = (done * 100) / total;
    return static_cast<unsigned int>(pct > 100 ? 100 : pct);
}

} // namespace engine::job::counters
