#include "engine/EscapeTimeEngine.hpp"
#include <cassert>
#include <experimental/simd>
#include <algorithm>
#include "macro_util.hpp"

namespace engine {

/**
 * @brief Processes a fixed 2D tile with a standard ETA algorithm.
 * @note periodically (every 64 iterations) checks if the job was aborted.
 */
template<typename F>
void EscapeTimeEngine::processChunkSIMD(const job::RenderJob::ETAJob& job_ref,
                                        const job::RenderJob::JobSpecs specs, const size_t chunk_idx) {
    namespace stdx = std::experimental;
    static constexpr bool OPT_CARDIOID_BULB = true;

    const int signed_width = static_cast<int>(specs.width);
    const int signed_height = static_cast<int>(specs.height);

    // 1. Map 1D chunk_idx to 2D grid coordinates (Factoring out total_chunks entirely)
    const int grid_cols = (signed_width + TILE_SIZE - 1) / TILE_SIZE;
    const int grid_x = static_cast<int>(chunk_idx) % grid_cols;
    const int grid_y = static_cast<int>(chunk_idx) / grid_cols;

    // 2. Compute exact pixel boundaries for this specific tile
    const int start_x = grid_x * TILE_SIZE;
    const int end_x   = std::min(start_x + TILE_SIZE, signed_width);
    const int start_y = grid_y * TILE_SIZE;
    const int end_y   = std::min(start_y + TILE_SIZE, signed_height);

    const unsigned int max_iteration = specs.iterations;
    constexpr int V_WIDTH = static_cast<int>(stdx::simd<F>::size());
    using IntF = std::conditional_t<std::is_same_v<F, double>, unsigned long long, unsigned int>;

    const F f_xMin  = static_cast<F>(specs.reference.real());
    const F f_yMin  = static_cast<F>(specs.reference.imag());
    const F f_stepX = static_cast<F>(specs.pixelStep.real());
    const F f_stepY = static_cast<F>(specs.pixelStep.imag());

    const stdx::simd<F> x_step_offsets = stdx::simd<F>([](size_t i) { return static_cast<F>(i); }) * f_stepX;

    core::Pixel* const __restrict__ raw_canvas = back_buffer;
    const IntF iterMaxCast = static_cast<IntF>(max_iteration);

    // Restrict rendering purely to the bounds of the 2D tile
    for (int py = start_y; py < end_y; ++py) {
        core::Pixel* const row_ptr = raw_canvas + (py * signed_width);
        const stdx::simd<F> c_imag = f_yMin + (static_cast<F>(py) * f_stepY);
        const stdx::simd<F> c_imag_sq = c_imag * c_imag;

        for (int px = start_x; px < end_x; px += V_WIDTH) {

            // Safe: If px=100 and end_x=102, lanes 2..7 compute ghost coordinates (x=102..107).
            const stdx::simd<F> c_real = f_xMin + (static_cast<F>(px) * f_stepX) + x_step_offsets;

            stdx::simd_mask<F> inside_set{false};

            if constexpr (OPT_CARDIOID_BULB) {
                const stdx::simd<F> c_r_minus_0_25 = c_real - 0.25f;
                const stdx::simd<F> q = c_r_minus_0_25 * c_r_minus_0_25 + c_imag_sq;
                const stdx::simd_mask<F> in_cardioid = q * (q + c_r_minus_0_25) <= 0.25f * c_imag_sq;
                const stdx::simd<F> c_r_plus_1 = c_real + 1.0f;
                const stdx::simd_mask<F> in_bulb = c_r_plus_1 * c_r_plus_1 + c_imag_sq <= 0.0625f;

                inside_set = in_cardioid || in_bulb;

                if (stdx::all_of(inside_set)) {
                    // TAIL SAFE CLAMP: Prevent writing past tile edge / screen edge on early-out
                    const int valid_lanes = std::min(V_WIDTH, end_x - px);
                    for (int i = 0; i < valid_lanes; ++i) row_ptr[px + i] = core::PIXEL_BLACK;
                    continue;
                }
            }

            stdx::simd<F> x = 0.0f, y = 0.0f, x2 = 0.0f, y2 = 0.0f;
            stdx::simd<F> escape_r2 = 4.0f;

            stdx::simd<IntF> iterations = 0;
            stdx::where_expression(inside_set, iterations) = iterMaxCast;

            stdx::simd_mask<F> active = !inside_set;
            IntF curr_iter = 0;

            while (curr_iter < iterMaxCast) {
                for (int k = 0; k < 8 && curr_iter < iterMaxCast; ++k, ++curr_iter) {

                    y = (2.0f * x * y) + c_imag;
                    x = (x2 - y2) + c_real;
                    x2 = x * x;
                    y2 = y * y;

                    const stdx::simd<F> current_r2 = x2 + y2;
                    const stdx::simd_mask<F> within_bounds = (current_r2 <= 4.0f);
                    const stdx::simd_mask<F> just_escaped = active && !within_bounds;

                    stdx::where_expression(just_escaped, escape_r2) = current_r2;

                    active = active && within_bounds;
                    stdx::where_expression(active, iterations) += 1;
                }

                if (stdx::none_of(active)) break;
            }

            CACHE_ALIGN IntF iters[V_WIDTH];
            CACHE_ALIGN F final_r2[V_WIDTH];
            iterations.copy_to(iters, stdx::element_aligned);
            escape_r2.copy_to(final_r2, stdx::element_aligned);

            // TAIL SAFE CLAMP: Only write back lanes that exist in this tile/screen boundary
            const int valid_lanes = std::min(V_WIDTH, end_x - px);

            for (int i = 0; i < valid_lanes; ++i) {
                if (job_ref.aborted()) return;
                row_ptr[px + i] = util::ColorUtil::Compute(
                    static_cast<unsigned int>(iters[i]), max_iteration, static_cast<float>(final_r2[i]), gradient
                );
            }
        }
    }
}

template<typename F>
void EscapeTimeEngine::processEscapeTimeJob(job::RenderJob& job) {
    size_t chunk_id;
    auto& j = job.getState<job::RenderJob::ETAJob>();
    auto specs = job.getSpecs();

    while (j.get_chunk(specs.chunks,chunk_id)) {
        processChunkSIMD<F>(j, specs, chunk_id);
        j.mark_completed_chunk(chunk_id);
    }
}

// Explicit instantiations: the only two lane widths the engine dispatches.
template void EscapeTimeEngine::processEscapeTimeJob<float>(job::RenderJob&);
template void EscapeTimeEngine::processEscapeTimeJob<double>(job::RenderJob&);

} // namespace engine