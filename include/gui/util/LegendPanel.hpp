#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include "UITheme.hpp"
#include "../../engine/MandelbrotEngine.hpp"

namespace gui::util {
    // ============================================================================
    // RIGHT PANEL: TELEMETRY & TRIPLEX STATUS BADGE
    // ============================================================================
    class LegendPanel {
    public:
        bool isOpen{true};

        bool Draw(int screenW, float scale, const engine::MandelbrotEngine& eng, bool isRefining, float redTimer) {
            if (IsKeyPressed(KEY_L)) isOpen = !isOpen;
            if (!isOpen) return false;

            const float w = 240.0f * scale, h = 135.0f * scale;
            const float x = screenW - w - (15.0f * scale), y = 15.0f * scale;
            const int fs = std::max(10, static_cast<int>(14 * scale));

            DrawRectangleRec({x, y, w, h}, UITheme::PanelBg);
            DrawRectangleLinesEx({x, y, w, h}, 1.0f, UITheme::PanelBorder);
            DrawRectangleRec({x, y, w, 26.0f * scale}, UITheme::HeaderBg);
            DrawText("TELEMETRY (L)", static_cast<int>(x + 8*scale), static_cast<int>(y + 5*scale), fs, WHITE);

            int ty = static_cast<int>(y + 32 * scale);
            DrawText(TextFormat("FPS: %i", GetFPS()), static_cast<int>(x + 10*scale), ty, fs, UITheme::AccentActive);

            ty += static_cast<int>(20 * scale);
            double z = eng.getZoom();
            DrawText(TextFormat("Zoom: 1e%.1f %s", std::log10(z), z > engine::MandelbrotEngine::getPerturbationThreshold() ? "(PTB)" : "(ETA)"),
                     static_cast<int>(x + 10*scale), ty, fs, z > engine::MandelbrotEngine::getPerturbationThreshold() ? ORANGE : SKYBLUE);

            // --- Triplex Status Badge ---
            ty += static_cast<int>(22 * scale);
            const char* sTxt = "Status: Ready"; Color sCol = GREEN;
            if (redTimer > 0.0f) { sTxt = "Status: No History!"; sCol = RED; }
            else if (isRefining) { sTxt = "Status: Refining..."; sCol = YELLOW; }
            DrawText(sTxt, static_cast<int>(x + 10*scale), ty, fs, sCol);

            // Reset Button
            ty += static_cast<int>(24 * scale);
            Rectangle rBtn = { x + 8*scale, static_cast<float>(ty), w - 16*scale, 26*scale };
            Vector2 m = GetMousePosition();
            bool hov = CheckCollisionPointRec(m, rBtn);

            DrawRectangleRec(rBtn, hov ? UITheme::ButtonHover : UITheme::HeaderBg);
            DrawRectangleLinesEx(rBtn, 1.0f, hov ? UITheme::AccentActive : UITheme::PanelBorder);
            DrawText("RESET CAMERA (R)", static_cast<int>(rBtn.x + 22*scale), static_cast<int>(rBtn.y + 5*scale), fs, WHITE);

            return (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) || IsKeyPressed(KEY_R);
        }
    };

}
