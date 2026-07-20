#pragma once
#include "core/Kernel.hpp"
#include "util/ColorUtil.hpp"
#include "job/RenderJob.hpp"

namespace engine {

/**
 * @brief Escape-Time rendering strategy (classic z = z^2 + c), SIMD-accelerated.
 *
 * @details Stateless apart from two references: the current gradient and the
 * MandelbrotEngine's live back-region pointer (re-pointed on every swap). The
 * per-chunk kernel is templated on the lane type F (float or double) so the same
 * code runs 8-wide or 4-wide depending on the zoom. Those kernels are defined and
 * explicitly instantiated in EscapeTimeEngine.cpp.
 */
class EscapeTimeEngine {
public:
    explicit EscapeTimeEngine(const util::Gradient& gradient, core::Pixel*& back_buf_ref)
        : gradient(gradient), back_buffer(back_buf_ref) {}

    /// Drains this job's chunks, rendering each at lane width F. Instantiated for
    /// float and double in EscapeTimeEngine.cpp.
    template<typename F>
    void processEscapeTimeJob(job::RenderJob& job);

    //Return the number of chunks computed based off the width and height
    static constexpr unsigned int getChunks(unsigned int width, unsigned int height) noexcept {
        return core::ComputeTotalChunks(width,height);
    }

private:
    const util::Gradient& gradient;
    // Reference to the MandelbrotEngine's current back-region pointer. The engine
    // re-points it on every swap, so workers always see the live back buffer.
    core::Pixel*& back_buffer;

    template<typename F>
    void processChunkSIMD(const job::RenderJob::ETAJob& job_ref,
                          const job::RenderJob::JobSpecs specs, const size_t chunk_idx);
};

} // namespace engine
