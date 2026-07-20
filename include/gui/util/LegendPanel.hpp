#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include "UITheme.hpp"
#include "Widgets.hpp"
#include "../../engine/MandelbrotEngine.hpp"

namespace gui::util {
    // ============================================================================
    // RIGHT PANEL: TELEMETRY & TRIPLEX STATUS BADGE
    // ============================================================================
    class LegendPanel {
    public:
        bool isOpen{true};

        static constexpr float WIDTH  = 240.0f;
        static constexpr float HEIGHT = 135.0f;

        /// Screen rect this panel occupies; empty when closed. `isMouseOverUI` reads this
        /// rather than re-deriving the geometry (which used to drift out of sync).
        Rectangle GetBoundingBox(int screenW, float scale) const {
            if (!isOpen) return {0, 0, 0, 0};
            const float w = WIDTH * scale, h = HEIGHT * scale;
            return { screenW - w - widgets::PANEL_MARGIN * scale, widgets::PANEL_MARGIN * scale, w, h };
        }

        bool Draw(int screenW, float scale, double zoom, std::optional<unsigned int> refinementPercentage, float redTimer) {
            if (IsKeyPressed(KEY_L)) isOpen = !isOpen;
            if (!isOpen) return false;

            const Rectangle box = GetBoundingBox(screenW, scale);
            const float x = box.x, y = box.y, w = box.width;
            const int fs = widgets::FontSize(scale, 14);

            widgets::DrawPanel(box, "TELEMETRY  (L to close)", 26.0f * scale, fs);

            int ty = static_cast<int>(y + 32 * scale);
            const double z = zoom;
            const bool ptb = z > engine::MandelbrotEngine::getPerturbationThreshold();
            DrawText(TextFormat("Zoom: 1e%.1f %s", std::log10(z), ptb ? "(PTB)" : "(ETA)"),
                     static_cast<int>(x + 10*scale), ty, fs, ptb ? ORANGE : SKYBLUE);

            // --- Triplex Status Badge ---
            ty += static_cast<int>(22 * scale);
            const char* sTxt = "Status: Ready"; Color sCol = GREEN;
            if (redTimer > 0.0f) { sTxt = "Status: No History!"; sCol = RED; }
            else if (refinementPercentage) {
                sTxt = TextFormat("Refining... %u %%", refinementPercentage.value());
                sCol = YELLOW;
            }
            DrawText(sTxt, static_cast<int>(x + 10*scale), ty, fs, sCol);

            // Reset Button
            ty += static_cast<int>(24 * scale);
            const Rectangle rBtn = { x + 8*scale, static_cast<float>(ty), w - 16*scale, 26*scale };
            const bool clicked = widgets::DrawButton(rBtn, "RESET CAMERA (R)", GetMousePosition(), fs, 22.0f * scale);

            return clicked || IsKeyPressed(KEY_R);
        }
    };

}
