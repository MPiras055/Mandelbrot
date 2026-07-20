#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "UITheme.hpp"

/**
 * @file Widgets.hpp
 * @brief Shared immediate-mode widget primitives for the GUI panels.
 *
 * @details `SettingsSidebar` and `LegendPanel` previously each carried their own
 * copy of the same three-call panel chrome (background + border + header strip)
 * and the same hover/press button body. These are those primitives, extracted so
 * both panels are layout-only. Everything here is pure raylib immediate mode —
 * a few dozen widgets at 60 Hz, so there is no performance consideration.
 */
namespace gui::util::widgets {

/// Shared metrics so the two panels can't drift apart.
inline constexpr float PANEL_MARGIN = 15.0f;   // screen-edge inset, scaled
inline constexpr float CHIP_SIZE    = 36.0f;   // collapsed-panel reopen chip, scaled

/// Body font size for a given UI scale (floored so it stays legible when zoomed out).
inline int FontSize(float scale, int base = 13) {
    return std::max(10, static_cast<int>(base * scale));
}

/// Panel chrome: background, border, and a filled header strip with an optional title.
/// Pass `headerH == 0` to draw a plain bordered card with no strip.
inline void DrawPanel(Rectangle b, const char* title, float headerH, int fs,
                      Color bg = UITheme::PanelBg, Color titleColor = WHITE) {
    DrawRectangleRec(b, bg);
    DrawRectangleLinesEx(b, 1.0f, UITheme::PanelBorder);
    if (headerH > 0.0f) {
        DrawRectangleRec({b.x, b.y, b.width, headerH}, UITheme::HeaderBg);
        if (title) DrawText(title, static_cast<int>(b.x + 8), static_cast<int>(b.y + 4), fs, titleColor);
    }
}

/// Hover-highlighted button. @return true on left-press while hovered.
inline bool DrawButton(Rectangle b, const char* txt, Vector2 m, int fs, float textInset = 15.0f) {
    const bool h = CheckCollisionPointRec(m, b);
    DrawRectangleRec(b, h ? UITheme::ButtonHover : UITheme::HeaderBg);
    DrawRectangleLinesEx(b, 1.0f, h ? UITheme::AccentActive : UITheme::PanelBorder);
    DrawText(txt, static_cast<int>(b.x + textInset), static_cast<int>(b.y + 6), fs, WHITE);
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

/// Tab header — a button without the hover affordance, inverted when active.
inline void DrawTabButton(Rectangle b, const char* txt, bool active, int fs) {
    DrawRectangleRec(b, active ? UITheme::AccentActive : UITheme::HeaderBg);
    DrawRectangleLinesEx(b, 1.0f, UITheme::PanelBorder);
    DrawText(txt, static_cast<int>(b.x + 18), static_cast<int>(b.y + 6), fs, active ? BLACK : WHITE);
}

/// Which half of a two-up stepper was clicked.
enum class Step { NONE, LEFT, RIGHT };

/// Two buttons side by side. Returns which one was pressed, so the caller does not
/// have to re-derive it from the mouse position.
inline Step DrawStepper(Rectangle b, const char* l, const char* r, Vector2 m, int fs) {
    const float half = (b.width / 2.0f) - 2.0f;
    const Rectangle bL{b.x, b.y, half, b.height};
    const Rectangle bR{b.x + half + 4.0f, b.y, half, b.height};
    const bool pL = DrawButton(bL, l, m, fs, 8.0f);
    const bool pR = DrawButton(bR, r, m, fs, 8.0f);
    if (pL) return Step::LEFT;
    if (pR) return Step::RIGHT;
    return Step::NONE;
}

/// Labelled checkbox. Toggles `val` on click anywhere in the row. @return true if toggled.
inline bool DrawCheckbox(Rectangle cb, const char* txt, bool& val, int fs, float s, Vector2 m) {
    DrawRectangleLinesEx(cb, 1.5f, UITheme::PanelBorder);
    if (val) DrawRectangleRec({cb.x + 3*s, cb.y + 3*s, cb.width - 6*s, cb.height - 6*s}, UITheme::AccentActive);
    DrawText(txt, static_cast<int>(cb.x + cb.width + 8*s), static_cast<int>(cb.y + 1*s), fs, UITheme::TextNormal);
    if (CheckCollisionPointRec(m, {cb.x, cb.y, cb.width + 180*s, cb.height}) &&
        IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { val = !val; return true; }
    return false;
}

/// Square chip used to reopen a collapsed panel (the `>>` affordance).
/// @return true on left-press while hovered.
inline bool DrawChip(Rectangle b, const char* glyph, Vector2 m, int fs) {
    const bool h = CheckCollisionPointRec(m, b);
    DrawRectangleRec(b, h ? UITheme::ButtonHover : UITheme::PanelBg);
    DrawRectangleLinesEx(b, 1.0f, h ? UITheme::AccentActive : UITheme::PanelBorder);
    const int tw = MeasureText(glyph, fs);
    DrawText(glyph, static_cast<int>(b.x + (b.width - tw) * 0.5f),
             static_cast<int>(b.y + (b.height - fs) * 0.5f), fs, WHITE);
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

} // namespace gui::util::widgets
