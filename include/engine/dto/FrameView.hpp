#pragma once
#include <cstddef>
#include "core/Pixel.hpp"

namespace engine::dto {

/**
 * @brief Read-only handle to the completed front frame, handed to the GUI.
 *
 * @details Non-owning window over the DoubleCanvas front region. `pixels` is
 * valid until the next harvest flip. `uptodate` is true only when this harvest
 * produced a newly-completed frame (the GUI uploads only then).
 */
struct FrameView {
    const core::Pixel* pixels;
    size_t width;
    size_t height;
    bool uptodate;
};

} // namespace engine::dto
