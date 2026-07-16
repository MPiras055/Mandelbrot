#include "engine/DoubleCanvas.hpp"

namespace engine::util {

DoubleCanvas::DoubleCanvas(size_t initial_screen_width, size_t initial_screen_height)
    : screen_width_(initial_screen_width),
      screen_height_(initial_screen_height),
      front_logical_w(initial_screen_width),
      front_logical_h(initial_screen_height),
      front_index(0)
{
    raw_memory = new core::Pixel[layer() * 2];
    for (size_t i = 0; i < layer() * 2; i++)
        raw_memory[i] = core::Pixel{ 0, 0, 0, 255 };
}

DoubleCanvas::~DoubleCanvas() {
    delete[] raw_memory;
}

void DoubleCanvas::resize_screen(size_t new_screen_w, size_t new_screen_h) {
    screen_width_  = new_screen_w;
    screen_height_ = new_screen_h;
    delete[] raw_memory;
    raw_memory = new core::Pixel[layer() * 2];

    front_index     = 0;
    front_logical_w = new_screen_w;
    front_logical_h = new_screen_h;
}

} // namespace engine::util
