#pragma once
#include <array>
#include "engine/util/ColorUtil.hpp"

/**
 * @file Palettes.hpp
 * @brief A small fixed set of selectable colour-gradient presets for the GUI.
 *
 * @details Each preset is a fully-formed `engine::util::Gradient` (stops +
 * shading flags). The sidebar cycles an index into `Presets`; the app pushes the
 * chosen gradient to the engine via `MandelbrotEngine::SetPalette`, which
 * re-`prepare()`s and re-dispatches. The default (index 0) mirrors the engine's
 * built-in "Glacier" scheme so switching away and back is lossless.
 */
namespace gui {

using engine::util::Gradient;
using core::Pixel;

struct Palette {
    const char* name;
    Gradient    gradient;
};

inline const std::array<Palette, 5> Presets = {{
    { "Glacier", {
        .stops = {
            { 0.00f, Pixel{ 0, 2, 10, 255 } },
            { 0.16f, Pixel{ 32, 107, 203, 255 } },
            { 0.42f, Pixel{ 237, 255, 255, 255 } },
            { 0.64f, Pixel{ 255, 170, 0, 255 } },
            { 0.85f, Pixel{ 10, 5, 0, 255 } },
            { 1.00f, Pixel{ 0, 2, 10, 255 } }
        },
        .smooth_shading = true,
        .root_scaling = true
    }},
    { "Ember", {
        .stops = {
            { 0.00f, Pixel{ 0, 0, 0, 255 } },
            { 0.25f, Pixel{ 120, 20, 0, 255 } },
            { 0.50f, Pixel{ 235, 90, 0, 255 } },
            { 0.75f, Pixel{ 255, 200, 40, 255 } },
            { 1.00f, Pixel{ 255, 255, 235, 255 } }
        },
        .smooth_shading = true,
        .root_scaling = true
    }},
    { "Ultra Fractal", {
        .stops = {
            { 0.00f, Pixel{ 0, 7, 100, 255 } },
            { 0.16f, Pixel{ 32, 107, 203, 255 } },
            { 0.42f, Pixel{ 237, 255, 255, 255 } },
            { 0.6425f, Pixel{ 255, 170, 0, 255 } },
            { 0.8575f, Pixel{ 0, 2, 0, 255 } },
            { 1.00f, Pixel{ 0, 7, 100, 255 } }
        },
        .smooth_shading = true,
        .root_scaling = true
    }},
    { "Monochrome", {
        .stops = {
            { 0.00f, Pixel{ 0, 0, 0, 255 } },
            { 0.50f, Pixel{ 130, 130, 130, 255 } },
            { 1.00f, Pixel{ 255, 255, 255, 255 } }
        },
        .smooth_shading = true,
        .root_scaling = false
    }},
    { "Neon", {
        .stops = {
            { 0.00f, Pixel{ 5, 0, 20, 255 } },
            { 0.30f, Pixel{ 200, 0, 160, 255 } },
            { 0.55f, Pixel{ 0, 220, 220, 255 } },
            { 0.80f, Pixel{ 120, 255, 0, 255 } },
            { 1.00f, Pixel{ 5, 0, 20, 255 } }
        },
        .smooth_shading = true,
        .root_scaling = true
    }}
}};

} // namespace gui
