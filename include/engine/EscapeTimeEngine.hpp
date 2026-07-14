#pragma once
#include <cassert>
#include <experimental/simd>
#include <algorithm>
#include "macro_util.hpp"
#include "core/Kernel.hpp"
#include "util/FrameBuffer.hpp"
#include "util/ColorUtil.hpp"
#include "job/RenderJob.hpp"

namespace engine {

class MandelbrotEngine;

/**
 * This class implements a (almost) stateless engine struct which can act as a rendering strategy
 * for a complete MandelbrotEngine. This class implements the classic Escape Time Algorithm, which 
 * computes, for each point in the requested render area, the standard mandelbrot succession. The
 * Engine takes a reference to the backBuffer (from the MandelbrotEngine) as well as a reference 
 * to the MandelbrotEngine current gradient. 
 * 
 * The engine provides 2 main methods to compute the rendering and both are accellerated from the 
 * std::simd (as for not std::experimental::simd) addition to Cpp26. This choice is made for two 
 * main reason:
 * 1. I never experimented with std::simd and was excited to write vectorized code without actually
 * become dyslexic in using  intrinsics (plus portability is not bad)
 * 2. Using simd allows us to accellerate differently the rendering at different zoom levels
 * 
 * Point (2) is a bit foggy, so I'll explain better. The ETA algorithm is dependant to the floating
 * point precision we choose. Floats allow for little precision but account for only 32 bits, while 
 * doubles account for 64 bits but guarantee more precision. This allows us to write the same method
 * and based on instantiation (supposing the compilers actually vectorizes and on a CPU with 256bit
 * vector registers) allows for 8 parallel float computation or 4 parallel double computation. This
 */
class EscapeTimeEngine {
private:
    util::FrameBuffer& backBuffer;
    const util::Gradient& gradient;

    /**
     * @brief Processes a chunk with a standard ETA algorithm
     * 
     * @note: periodically (high periodicity) checks if the current job was aborted (light check mostly cache hot) 
     */
    template<typename F>
    void processChunkSIMD(const job::RenderJob::ETAJob& job_ref, const job::RenderJob::JobSpecs specs, const size_t chunk_idx) {
        namespace stdx = std::experimental;
        static constexpr bool OPT_CARDIOID_BULB = true; //we throw out the cardioid and bulb optimization for high zoom optimization
        
        const int total_chunks = static_cast<int>(specs.chunks);
        const int start_y = (static_cast<int>(chunk_idx) * static_cast<int>(specs.height)) / total_chunks;
        const int end_y   = ((static_cast<int>(chunk_idx) + 1) * static_cast<int>(specs.height)) / total_chunks;
    
        const unsigned int max_iteration = specs.iterations;
        constexpr int V_WIDTH = static_cast<int>(stdx::simd<F>::size());
        using IntF = std::conditional_t<std::is_same_v<F, double>, unsigned long long, unsigned int>;
        
        const F f_xMin  = static_cast<F>(specs.x);
        const F f_yMin  = static_cast<F>(specs.y);
        const F f_stepX = static_cast<F>(specs.pixelStepX);
        const F f_stepY = static_cast<F>(specs.pixelStepY);
        
        const stdx::simd<F> x_step_offsets = stdx::simd<F>([](size_t i) { return static_cast<F>(i); }) * f_stepX;
        
        core::Pixel* const __restrict__ raw_canvas = backBuffer.data();
        const IntF iterMaxCast = static_cast<IntF>(max_iteration);
        const int signed_width = static_cast<int>(specs.width);
    
        for (int py = start_y; py < end_y; ++py) {
            core::Pixel* const row_ptr = raw_canvas + (py * signed_width);
            const stdx::simd<F> c_imag = f_yMin + (static_cast<F>(py) * f_stepY);
            const stdx::simd<F> c_imag_sq = c_imag * c_imag; 
    
            for (int px = 0; px < signed_width; px += V_WIDTH) {
                
                // Safe: If px=100 and width=102, lanes 2..7 compute ghost coordinates (x=102..107). 
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
                        // TAIL SAFE CLAMP: Prevent writing past screen edge on early-out
                        const int valid_lanes = std::min(V_WIDTH, signed_width - px);
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
                    if ((curr_iter & 63) == 0 && job_ref.aborted()) return; 
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
    
                // TAIL SAFE CLAMP: Only write back lanes that physically exist on the monitor
                const int valid_lanes = std::min(V_WIDTH, signed_width - px);
    
                for (int i = 0; i < valid_lanes; ++i) {
                    row_ptr[px + i] = util::ColorUtil::Compute(
                        static_cast<unsigned int>(iters[i]), max_iteration, static_cast<float>(final_r2[i]), gradient
                    );
                }
            }
        }
    }
public:
    EscapeTimeEngine(util::FrameBuffer& fbuffer, const util::Gradient& gradient):
        backBuffer(fbuffer), gradient(gradient) {}
    
    template<typename F>
    void processEscapeTimeJob(job::RenderJob& job) {
        size_t chunk_id;
        auto& j = job.getState<job::RenderJob::ETAJob>();
        auto specs = job.getSpecs();
        
        while (j.get_chunk(specs.chunks, chunk_id)) {
            processChunkSIMD<F>(j, specs, chunk_id);
            j.mark_completed_chunk(chunk_id);
        }
    }

    static inline size_t CalculateTotalChunks(unsigned int width, unsigned int height) noexcept {
        return core::CalculateTotalChunks(width, height);
    }
};

} // namespace mandelbrot_engine