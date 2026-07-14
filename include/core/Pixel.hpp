#pragma once
#include <cstdint>

/**
 * @file Pixel.hpp
 * @brief Engine-owned RGBA8 pixel — the compute core's color type.
 *
 * @details The core previously spoke raylib's `Color`, dragging <raylib.h> into
 * FrameBuffer/ColorUtil/the engines. `Pixel` is byte-for-byte identical to raylib
 * `Color` (RGBA, 4x uint8_t), so the GUI can hand a `Pixel*` straight to
 * UpdateTexture (which takes `const void*`) with no conversion — the swap is
 * bit-preserving.
 */
namespace core {

    struct Pixel {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    static_assert(sizeof(Pixel) == 4, "Pixel must be tightly packed RGBA8");

    // Interior / non-escaping pixels (matches raylib BLACK = {0,0,0,255}).
    inline constexpr Pixel PIXEL_BLACK{0, 0, 0, 255};

} // namespace core
