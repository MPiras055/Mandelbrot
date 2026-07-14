#pragma once
#include <cstddef>

/**
 * @file Kernel.hpp
 * @brief Shared Mandelbrot primitives (tiling + iteration step).
 *
 * @details Both rendering strategies previously carried a byte-identical
 * `CalculateTotalChunks`, and the z = z^2 + c recurrence is spelled out several
 * times across the engines. This header is the single home for those primitives.
 *
 * @note Phase-1 (soft) scope: only the chunk-count formula is rewired here — it
 * is a pure numeric helper with no behavioural effect. The hand-tuned inner loops
 * (the ETA SIMD kernel, the perturbation reference/delta loops) keep their own
 * spellings for now; they will consolidate onto `mandelStep` during the Phase-2
 * engine rewrites, where those loops are being rewritten regardless.
 */
namespace core {

    // Tiling granularity: work is diced into 32x32 blocks.
    inline constexpr unsigned int CHUNK_BLOCK = 32;

    /**
     * @brief Number of 32x32 tiles covering a width x height render area.
     * @details Single source for both engines' chunk counting (identical to the
     * formula they each used before).
     */
    inline size_t CalculateTotalChunks(unsigned int width, unsigned int height) noexcept {
        size_t blocks_x = (width + (CHUNK_BLOCK - 1)) / CHUNK_BLOCK;
        size_t blocks_y = (height + (CHUNK_BLOCK - 1)) / CHUNK_BLOCK;
        return blocks_x * blocks_y;
    }

    /**
     * @brief Canonical z = z^2 + c step, in place.
     * @tparam T scalar float type (double / BigFloat).
     * @note Provided as the shared definition the Phase-2 rewrites consolidate on;
     * not yet wired into the hot loops (see file note).
     */
    template <typename T>
    inline void mandelStep(T& zr, T& zi, const T& cr, const T& ci) noexcept {
        const T zr2 = zr * zr;
        const T zi2 = zi * zi;
        const T next_zi = T(2) * zr * zi + ci;
        zr = zr2 - zi2 + cr;
        zi = next_zi;
    }

} // namespace core
