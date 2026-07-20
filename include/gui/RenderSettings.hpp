#pragma once

/**
 * @file RenderSettings.hpp
 * @brief Render-quality knobs, owned as plain data rather than by a UI widget.
 *
 * @details These fields previously lived as public members of SettingsSidebar,
 * blurring the line between UI control state and render parameters. They are
 * relocated here unchanged (same defaults, same semantics); the SettingsSidebar
 * now edits a RenderSettings instance and the app reads it at dispatch time.
 *
 * @note The palette still lives in the engine (`SetPalette`); wiring a palette
 * selector through here is a Phase-2 task ("Add palette change to GUI").
 */
namespace gui {

struct RenderSettings {
    // Panning (fast) pass
    unsigned int panningIters{1024};
    float upscaleFactor{8.0f};

    // Refinement (full-res) pass — staged (pending) vs applied (active)
    unsigned int activeRefiningIters{2048};
    unsigned int pendingRefiningIters{2048};
    bool disableRefinement{false};

    // When on, editing the Target (refining) iterations also sets the Nav (panning) count.
    bool matchIters{false};

    // Index into gui::Presets (Palettes.hpp); 0 = engine default ("Glacier").
    int paletteIndex{0};
};

} // namespace gui
