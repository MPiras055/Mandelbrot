#pragma once
#include <cstddef>
#include <cassert>
#include "core/Pixel.hpp"

namespace engine::util {

using core::Pixel;

/**
 * @brief Non-owning window over a contiguous Pixel region.
 *
 * @details The pixel memory is owned by DoubleCanvas; a FrameBuffer is just a
 * (data, width, height) handle onto one of its regions. MandelbrotEngine holds a
 * single FrameBuffer that it re-points (`reset`) at the current back region after
 * every harvest flip, so the render strategies can keep a stable `FrameBuffer&`
 * reference across frames.
 */
struct FrameBuffer {
    private:
    Pixel* data_{nullptr};
    size_t width_{0};
    size_t height_{0};

    public:
    FrameBuffer() = default;
    FrameBuffer(Pixel* data, size_t width, size_t height) noexcept
        : data_{data}, width_{width}, height_{height} {}

    /// Re-point this handle at a different region (non-owning; frees nothing).
    void reset(Pixel* data, size_t width, size_t height) noexcept {
        data_ = data;
        width_ = width;
        height_ = height;
    }

    Pixel* data() noexcept { return data_; }
    const Pixel* data() const noexcept { return data_; }

    Pixel& operator[](size_t index) noexcept {
        assert(index < width_ * height_ && "FrameBuffer: out of bounds set");
        return data_[index];
    }
    Pixel operator[](size_t index) const noexcept {
        assert(index < width_ * height_ && "FrameBuffer: out of bounds access");
        return data_[index];
    }

    size_t size()   const noexcept { return width_ * height_; }
    size_t width()  const noexcept { return width_; }
    size_t height() const noexcept { return height_; }
};

} // namespace engine::util
