#pragma once
#include <cstdint>
#include <cstddef>
#include "core/Pixel.hpp"
#include "dto/FrameView.hpp"

namespace engine::util {

/**
 * @brief Owns one contiguous allocation split into two screen-sized regions
 * (front + back) and ping-pongs between them.
 *
 * @details Workers render into the back region; the GUI displays the front via
 * `front_view()`. A completed frame is published by `swap()`, which merely flips
 * an index. Memory is sized to the current screen (not a hard maximum) and only
 * re-allocated on `resize_screen`.
 *
 * @warning `swap()` and `resize_screen()` are NOT MT-safe. By design a flip or a
 * reallocation may only happen while every render worker is quiesced (i.e. the
 * in-flight job is completed/aborted and drained); the class does no internal
 * locking.
 */
class DoubleCanvas {
private:
    core::Pixel* raw_memory;

    // Physical bounds of each region (the allocation is 2x this many pixels).
    size_t screen_width_;
    size_t screen_height_;

    // Logical dimensions of the last completed frame sitting in the front region.
    size_t front_logical_w;
    size_t front_logical_h;

    // Selects which region is currently the front (0 or 1).
    uint8_t front_index;

    size_t layer() const noexcept { return screen_width_ * screen_height_; }
    core::Pixel*       region(uint8_t idx)       noexcept { return raw_memory + (static_cast<size_t>(idx) * layer()); }
    const core::Pixel* region(uint8_t idx) const noexcept { return raw_memory + (static_cast<size_t>(idx) * layer()); }

public:
    DoubleCanvas(size_t initial_screen_width, size_t initial_screen_height)
        : screen_width_(initial_screen_width),
          screen_height_(initial_screen_height),
          front_logical_w(initial_screen_width),
          front_logical_h(initial_screen_height),
          front_index(0)
    {
        raw_memory = new core::Pixel[layer() * 2];
        for(size_t i = 0; i < layer() * 2; i++) 
            raw_memory[i] = core::Pixel{0,0,0,255};
    }

    ~DoubleCanvas() {
        delete[] raw_memory;
    }

    // --- Buffer access ---

    /// Raw pointer to the back region; the engine renders the next frame here.
    /// @note const method returning writable pixels: the canvas object is not
    /// modified, but the pixel memory it owns is the render target. A worker must
    /// hold a reserved chunk before calling this, so no swap can occur meanwhile.
    core::Pixel* back_ptr() const noexcept {
        return const_cast<core::Pixel*>(region(static_cast<uint8_t>(1 - front_index)));
    }

    /// Read-only view of the completed front frame for the GUI to upload.
    engine::dto::FrameView front_view(bool uptodate = false) const {
        return engine::dto::FrameView{ region(front_index), front_logical_w, front_logical_h, uptodate };
    }

    /**
     * @brief Publishes the back region as the new front, recording the render
     * dimensions that were actually completed.
     * @warning NOT MT-safe — see class note; call only with workers quiesced.
     */
    void swap(size_t completed_render_w, size_t completed_render_h) noexcept {
        front_index = static_cast<uint8_t>(1 - front_index);
        front_logical_w = completed_render_w;
        front_logical_h = completed_render_h;
    }

    /**
     * @brief Reallocates both regions to match a new screen size (grows OR shrinks).
     * @details Swaps in a freshly-sized allocation, so shrinking actually releases
     * memory. The front frame is discarded (cleared to black); the caller is
     * expected to re-dispatch a full frame afterwards.
     * @warning NOT MT-safe — see class note; call only with workers quiesced.
     */
    void resize_screen(size_t new_screen_w, size_t new_screen_h) {
        screen_width_  = new_screen_w;
        screen_height_ = new_screen_h;
        delete[] raw_memory;
        raw_memory = new core::Pixel[layer() * 2];

        front_index     = 0;
        front_logical_w = new_screen_w;
        front_logical_h = new_screen_h;
    }

    size_t screen_width()  const noexcept { return screen_width_; }
    size_t screen_height() const noexcept { return screen_height_; }

    DoubleCanvas(const DoubleCanvas&) = delete;
    DoubleCanvas& operator=(const DoubleCanvas&) = delete;
};

} // namespace engine::util
