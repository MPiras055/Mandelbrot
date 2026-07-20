#pragma once
#include <cstddef>

/**
 * @file Kernel.hpp
 * @brief Shared Mandelbrot primitives (tiling + iteration step).
 *
 * @details Both rendering strategies previously carried byte-identical copies of the
 * chunk-count formula and the chunk-index-to-tile-bounds mapping, and the z = z^2 + c
 * recurrence is spelled out several times across the engines. This header is the single
 * home for those primitives.
 *
 * @note The hand-tuned inner loops keep their own spellings on purpose: the ETA kernel
 * runs on `stdx::simd` and carries `x2`/`y2` across the iteration boundary (which
 * `mandelStep` would recompute), and the perturbation loop needs the fused delta form
 * `2(zr*dx - zi*dy) + dx^2 - dy^2 + dc`. Only the non-hot BigFloat orbit builders
 * consolidate onto `mandelStep`.
 */
namespace core {

    // Tiling granularity: work is diced into CHUNK_BLOCK-square tiles.
    inline constexpr unsigned int CHUNK_BLOCK = 64;

    /**
     * @brief Number of tiles covering a width x height render area.
     * @details Single source for both engines' chunk counting (identical to the
     * formula they each used before).
     */
    inline size_t ComputeTotalChunks(unsigned int width, unsigned int height) noexcept {
        size_t blocks_x = (width + (CHUNK_BLOCK - 1)) / CHUNK_BLOCK;
        size_t blocks_y = (height + (CHUNK_BLOCK - 1)) / CHUNK_BLOCK;
        return blocks_x * blocks_y;
    }

    /**
     * @brief Shared cadence for cooperative abort polling inside long loops.
     * @details Poll when `(i & ABORT_POLL_MASK) == 0`. Every abort-pollable loop uses this
     * one stride so the responsiveness/overhead trade-off is stated in a single place —
     * the sites previously used 256, 100, every-pixel, and (in the probe) never.
     */
    inline constexpr unsigned int ABORT_POLL_MASK = 0xFF;   // every 256 iterations

    /// Half-open pixel bounds of one tile: [x0, x1) x [y0, y1).
    struct TileBounds { int x0, y0, x1, y1; };

    /**
     * @brief Map a 1D chunk index to its 2D tile bounds, clamped to the render area.
     * @details Both engines derived this identically; sharing it also removes a
     * size_t->int narrowing that the perturbation copy performed twice. Computed once
     * per chunk (never in an inner loop), so there is no hot-path cost.
     */
    inline TileBounds ChunkTile(size_t chunk_idx, unsigned int width, unsigned int height) noexcept {
        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        const int block = static_cast<int>(CHUNK_BLOCK);
        const int cols = (w + block - 1) / block;
        const int x0 = (static_cast<int>(chunk_idx) % cols) * block;
        const int y0 = (static_cast<int>(chunk_idx) / cols) * block;
        return { x0, y0, (x0 + block < w) ? x0 + block : w,
                         (y0 + block < h) ? y0 + block : h };
    }

    /**
     * @brief Canonical z = z^2 + c step, in place.
     * @tparam T scalar float type (double / BigFloat).
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
