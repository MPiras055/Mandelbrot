#pragma once
#include <raylib.h>
#include "UITheme.hpp"
#include "../RenderSettings.hpp"
#include <algorithm>

namespace gui::util {
    class SettingsSidebar {
    public:
        enum class Tab { ENGINE, EXPORT };
        bool isOpen{true};
        Tab activeTab{Tab::ENGINE};

        // Navigation mode (UI state, not a render parameter)
        bool allowDragging{true};

        // Render-quality knobs now live in a plain data struct.
        RenderSettings settings;

        float GetSidebarHeight(float scale) const {
            if (!isOpen) return 36.0f * scale;
            // Dynamically shrink-wraps background panel to exact widget height
            return (activeTab == Tab::ENGINE ? 395.0f : 165.0f) * scale;
        }

        struct DrawResult {
            bool needsInstantRerender{false};
            bool applyIterationsClicked{false};
        };

        DrawResult Draw(float scale) {
            if (IsKeyPressed(KEY_TAB)) isOpen = !isOpen;

            const float pad = 15.0f * scale;
            if (!isOpen) {
                Rectangle openTab = { pad, pad, 36*scale, 36*scale };
                DrawRectangleRec(openTab, UITheme::PanelBg); DrawRectangleLinesEx(openTab, 1.0f, UITheme::PanelBorder);
                DrawText(">>", static_cast<int>(pad + 10*scale), static_cast<int>(pad + 10*scale), static_cast<int>(14*scale), WHITE);
                if (CheckCollisionPointRec(GetMousePosition(), openTab) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) isOpen = true;
                return {};
            }

            DrawResult res;
            const float cardW = 320.0f * scale;
            const float cardH = GetSidebarHeight(scale);
            const int fs = std::max(10, static_cast<int>(13 * scale));
            Vector2 m = GetMousePosition();

            DrawRectangleRec({pad, pad, cardW, cardH}, UITheme::PanelBg);
            DrawRectangleLinesEx({pad, pad, cardW, cardH}, 1.0f, UITheme::PanelBorder);

            // Header & 2 Main Tabs
            DrawRectangleRec({pad, pad, cardW, 28*scale}, UITheme::HeaderBg);
            float tabW = cardW / 2.0f;
            Rectangle t1 = {pad, pad+28*scale, tabW, 26*scale}, t2 = {pad+tabW, pad+28*scale, tabW, 26*scale};

            if (CheckCollisionPointRec(m, t1) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) activeTab = Tab::ENGINE;
            if (CheckCollisionPointRec(m, t2) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) activeTab = Tab::EXPORT;

            DrawTabButton(t1, "Engine Setup", activeTab == Tab::ENGINE, fs);
            DrawTabButton(t2, "Export Phase", activeTab == Tab::EXPORT, fs);

            float curY = pad + 65 * scale;
            const float inX = pad + 12 * scale;
            const float subW = cardW - 24 * scale;

            if (activeTab == Tab::ENGINE) {
                // --- CARD 1: PANNING PASS ---
                float h1 = 160.0f * scale;
                DrawSubCard(inX, curY, subW, h1, "PANNING PASS", fs, scale);

                float py = curY + 28 * scale;
                DrawText(TextFormat("Nav Iterations: %u", settings.panningIters), static_cast<int>(inX + 8*scale), static_cast<int>(py), fs, UITheme::TextNormal); py += 18 * scale;
                if (DrawStepper({inX + 8*scale, py, subW - 16*scale, 24*scale}, "- 128", "+ 128", m, fs)) {
                    if (m.x < inX + (subW/2)) settings.panningIters = std::max(128u, settings.panningIters - 128); else settings.panningIters += 128;
                    res.needsInstantRerender = true; // Nav iterations update immediately mid-move
                }
                py += 30 * scale;
                DrawText(TextFormat("Downsample Scale: %.1fx", settings.upscaleFactor), static_cast<int>(inX + 8*scale), static_cast<int>(py), fs, UITheme::TextNormal); py += 18 * scale;
                if (DrawStepper({inX + 8*scale, py, subW - 16*scale, 24*scale}, "Sharper (-2x)", "Faster (+2x)", m, fs)) {
                    if (m.x < inX + (subW/2)) settings.upscaleFactor = std::max(2.0f, settings.upscaleFactor - 2.0f); else settings.upscaleFactor = std::min(16.0f, settings.upscaleFactor + 2.0f);
                    res.needsInstantRerender = true;
                }
                py += 30 * scale;
                // Mode toggle: Modifies state but intentionally does NOT request immediate re-render
                DrawCheckbox({inX + 8*scale, py, 16*scale, 16*scale}, "Allow Dragging (Box Zoom off)", allowDragging, fs, scale, m);

                // --- CARD 2: REFINEMENT PASS ---
                curY += h1 + 10 * scale;
                float h2 = 145.0f * scale;
                DrawSubCard(inX, curY, subW, h2, "REFINEMENT PASS", fs, scale);

                py = curY + 26 * scale;
                DrawText(TextFormat("Target Iterations: %u", settings.pendingRefiningIters), static_cast<int>(inX + 8*scale), static_cast<int>(py), fs, UITheme::TextNormal); py += 18 * scale;

                // Stepper only updates staging number
                if (DrawStepper({inX + 8*scale, py, subW - 16*scale, 24*scale}, "- 256", "+ 256", m, fs)) {
                    if (m.x < inX + (subW/2)) settings.pendingRefiningIters = std::max(256u, settings.pendingRefiningIters - 256); else settings.pendingRefiningIters += 256;
                }

                py += 28 * scale;
                // Apply Button (Only highlights if staged count != active engine count)
                bool isDirty = (settings.pendingRefiningIters != settings.activeRefiningIters);
                Rectangle applyBtn = {inX + 8*scale, py, subW - 16*scale, 26*scale};
                bool hovApply = CheckCollisionPointRec(m, applyBtn);

                DrawRectangleRec(applyBtn, hovApply ? UITheme::ButtonHover : (isDirty ? Color{35, 60, 20, 255} : UITheme::HeaderBg));
                DrawRectangleLinesEx(applyBtn, 1.0f, isDirty ? UITheme::AccentApply : UITheme::PanelBorder);

                const char* appTxt = TextFormat("APPLY ( %u -> %u )", settings.activeRefiningIters, settings.pendingRefiningIters);
                DrawText(appTxt, static_cast<int>(applyBtn.x + 12*scale), static_cast<int>(applyBtn.y + 6*scale), fs, isDirty ? UITheme::AccentApply : UITheme::TextMuted);

                if (isDirty && hovApply && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    settings.activeRefiningIters = settings.pendingRefiningIters;
                    res.applyIterationsClicked = true;
                }

                py += 32 * scale;
                // Refinement Toggle: Alters pipeline state but intentionally does NOT wipe canvas
                if(DrawCheckbox({inX + 8*scale, py, 16*scale, 16*scale}, "Disable Refinement Pass", settings.disableRefinement, fs, scale, m)) {
                    //if disableRefinement = true then we went from false to true
                    if(!settings.disableRefinement) res.needsInstantRerender = true;
                }
            }
            else {
                // --- TAB 2: EXPORT ---
                DrawText("High-Precision Render Artifacts", static_cast<int>(inX), static_cast<int>(curY), fs, UITheme::TextMuted); curY += 25 * scale;
                DrawButton({inX, curY, subW, 30*scale}, "Export Frame (PNG)", m, fs); curY += 42 * scale;
                DrawButton({inX, curY, subW, 30*scale}, "Export Path Animation", m, fs);
            }

            return res;
        }

        Rectangle GetBoundingBox(float scale) const {
            if (!isOpen) return {0,0,0,0};
            return { 15*scale, 15*scale, 320*scale, GetSidebarHeight(scale) };
        }

    private:
        void DrawSubCard(float x, float y, float w, float h, const char* title, int fs, float s) {
            DrawRectangleRec({x, y, w, h}, UITheme::SubCardBg);
            DrawRectangleLinesEx({x, y, w, h}, 1.0f, UITheme::PanelBorder);
            DrawRectangleRec({x, y, w, 22.0f * s}, UITheme::HeaderBg);
            DrawText(title, static_cast<int>(x + 8*s), static_cast<int>(y + 4*s), fs, UITheme::AccentActive);
        }

        void DrawTabButton(Rectangle b, const char* txt, bool active, int fs) {
            DrawRectangleRec(b, active ? UITheme::AccentActive : UITheme::HeaderBg);
            DrawRectangleLinesEx(b, 1.0f, UITheme::PanelBorder);
            DrawText(txt, static_cast<int>(b.x + 18), static_cast<int>(b.y + 6), fs, active ? BLACK : WHITE);
        }

        bool DrawButton(Rectangle b, const char* txt, Vector2 m, int fs) {
            bool h = CheckCollisionPointRec(m, b);
            DrawRectangleRec(b, h ? UITheme::ButtonHover : UITheme::HeaderBg); DrawRectangleLinesEx(b, 1.0f, h ? UITheme::AccentActive : UITheme::PanelBorder);
            DrawText(txt, static_cast<int>(b.x + 15), static_cast<int>(b.y + 7), fs, WHITE);
            return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        }

        bool DrawStepper(Rectangle b, const char* l, const char* r, Vector2 m, int fs) {
            float half = (b.width / 2.0f) - 2;
            Rectangle bL = {b.x, b.y, half, b.height}, bR = {b.x + half + 4, b.y, half, b.height};
            bool hL = CheckCollisionPointRec(m, bL), hR = CheckCollisionPointRec(m, bR);
            DrawRectangleRec(bL, hL ? UITheme::ButtonHover : UITheme::HeaderBg); DrawRectangleLinesEx(bL, 1.0f, UITheme::PanelBorder);
            DrawText(l, static_cast<int>(bL.x + 8), static_cast<int>(bL.y + 5), fs, WHITE);
            DrawRectangleRec(bR, hR ? UITheme::ButtonHover : UITheme::HeaderBg); DrawRectangleLinesEx(bR, 1.0f, UITheme::PanelBorder);
            DrawText(r, static_cast<int>(bR.x + 8), static_cast<int>(bR.y + 5), fs, WHITE);
            return (hL || hR) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        }

        bool DrawCheckbox(Rectangle cb, const char* txt, bool& val, int fs, float s, Vector2 m) {
            DrawRectangleLinesEx(cb, 1.5f, UITheme::PanelBorder);
            if (val) DrawRectangleRec({cb.x + 3*s, cb.y + 3*s, cb.width - 6*s, cb.height - 6*s}, UITheme::AccentActive);
            DrawText(txt, static_cast<int>(cb.x + cb.width + 8*s), static_cast<int>(cb.y + 1*s), fs, UITheme::TextNormal);
            if (CheckCollisionPointRec(m, {cb.x, cb.y, cb.width + 180*s, cb.height}) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { val = !val; return true; }
            return false;
        }
    };
}
