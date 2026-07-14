#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <macro_util.hpp>
#include "engine/MandelbrotEngine.hpp"
// #include "util/LegendPanel.hpp"
// #include "util/SettingsSidebar.hpp"

namespace gui::util {
struct UITheme {
    static constexpr Color PanelBg       { 15, 15, 20, 235 };
    static constexpr Color SubCardBg     { 24, 24, 34, 255 };
    static constexpr Color PanelBorder   { 65, 65, 85, 255 };
    static constexpr Color HeaderBg      { 32, 32, 48, 255 };
    static constexpr Color TextNormal    { 225, 225, 235, 255 };
    static constexpr Color TextMuted     { 135, 135, 155, 255 };
    static constexpr Color AccentActive  { 0, 190, 255, 255 };
    static constexpr Color AccentApply   { 110, 255, 50, 255 };
    static constexpr Color ButtonHover   { 52, 52, 74, 255 };
};
}


namespace gui::util {
    class SettingsSidebar {
    public:
        enum class Tab { ENGINE, EXPORT };
        bool isOpen{true};
        Tab activeTab{Tab::ENGINE};
    
        // Panning Pass State
        unsigned int panningIters{1024};
        float upscaleFactor{8.0f};
        bool allowDragging{true};
    
        // Refinement Pass State (Staged vs Active)
        unsigned int activeRefiningIters{2048};
        unsigned int pendingRefiningIters{2048};
        bool disableRefinement{false};
    
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
                DrawText(TextFormat("Nav Iterations: %u", panningIters), static_cast<int>(inX + 8*scale), static_cast<int>(py), fs, UITheme::TextNormal); py += 18 * scale;
                if (DrawStepper({inX + 8*scale, py, subW - 16*scale, 24*scale}, "- 128", "+ 128", m, fs)) {
                    if (m.x < inX + (subW/2)) panningIters = std::max(128u, panningIters - 128); else panningIters += 128;
                    res.needsInstantRerender = true; // Nav iterations update immediately mid-move
                }
                py += 30 * scale;
                DrawText(TextFormat("Downsample Scale: %.1fx", upscaleFactor), static_cast<int>(inX + 8*scale), static_cast<int>(py), fs, UITheme::TextNormal); py += 18 * scale;
                if (DrawStepper({inX + 8*scale, py, subW - 16*scale, 24*scale}, "Sharper (-2x)", "Faster (+2x)", m, fs)) {
                    if (m.x < inX + (subW/2)) upscaleFactor = std::max(2.0f, upscaleFactor - 2.0f); else upscaleFactor = std::min(16.0f, upscaleFactor + 2.0f);
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
                DrawText(TextFormat("Target Iterations: %u", pendingRefiningIters), static_cast<int>(inX + 8*scale), static_cast<int>(py), fs, UITheme::TextNormal); py += 18 * scale;
                
                // Stepper only updates staging number
                if (DrawStepper({inX + 8*scale, py, subW - 16*scale, 24*scale}, "- 256", "+ 256", m, fs)) {
                    if (m.x < inX + (subW/2)) pendingRefiningIters = std::max(256u, pendingRefiningIters - 256); else pendingRefiningIters += 256;
                }
                
                py += 28 * scale;
                // Apply Button (Only highlights if staged count != active engine count)
                bool isDirty = (pendingRefiningIters != activeRefiningIters);
                Rectangle applyBtn = {inX + 8*scale, py, subW - 16*scale, 26*scale};
                bool hovApply = CheckCollisionPointRec(m, applyBtn);
    
                DrawRectangleRec(applyBtn, hovApply ? UITheme::ButtonHover : (isDirty ? Color{35, 60, 20, 255} : UITheme::HeaderBg));
                DrawRectangleLinesEx(applyBtn, 1.0f, isDirty ? UITheme::AccentApply : UITheme::PanelBorder);
                
                const char* appTxt = TextFormat("APPLY ( %u -> %u )", activeRefiningIters, pendingRefiningIters);
                DrawText(appTxt, static_cast<int>(applyBtn.x + 12*scale), static_cast<int>(applyBtn.y + 6*scale), fs, isDirty ? UITheme::AccentApply : UITheme::TextMuted);
    
                if (isDirty && hovApply && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    activeRefiningIters = pendingRefiningIters;
                    res.applyIterationsClicked = true;
                }
    
                py += 32 * scale;
                // Refinement Toggle: Alters pipeline state but intentionally does NOT wipe canvas
                if(DrawCheckbox({inX + 8*scale, py, 16*scale, 16*scale}, "Disable Refinement Pass", disableRefinement, fs, scale, m)) {
                    //if disableRefinement = true then we went from false to true
                    if(!disableRefinement) res.needsInstantRerender = true; 
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


namespace gui {

// ============================================================================
// MAIN RAYLIB GUI FRONTEND
// ============================================================================
class GUI {
private:
    static constexpr unsigned int MAX_WIDTH = 3840, MAX_HEIGHT = 2160;

    int width{1080}, height{720}; 
    float uiScale{1.0f};
    bool uiHidden{false};

    Texture2D tex_hires{}, tex_lores{};  
    Texture2D* active_tex{nullptr}; 

    RenderTexture2D screen_target{}; 
    Shader diffusion_shader{};       
    int screen_size_loc{-1};         
    bool diffusion_active{true};     

    engine::MandelbrotEngine engine;

    util::LegendPanel legend;
    util::SettingsSidebar sidebar;

    std::vector<engine::MandelbrotEngine::CameraSnapshot> historyStack;
    float redAlertTimer{0.0f}; 

    bool needsUpdate{false}, isComputing{false}, wasInteracting{false};
    float renderScale{1.0f};
    int currentlyComputingScale{1};
    
    bool isBoxSelecting{false};
    Vector2 boxStart{0,0}, boxEnd{0,0};

    void ReallocateGPUTextures(int newW, int newH) {
        if (tex_hires.id != 0) UnloadTexture(tex_hires); if (tex_lores.id != 0) UnloadTexture(tex_lores);
        if (screen_target.id != 0) UnloadRenderTexture(screen_target);

        int lo_w = std::max(1, static_cast<int>(newW / sidebar.upscaleFactor));
        int lo_h = std::max(1, static_cast<int>(newH / sidebar.upscaleFactor));

        Image img_hi = GenImageColor(newW, newH, BLANK), img_lo = GenImageColor(lo_w, lo_h, BLANK);
        tex_hires = LoadTextureFromImage(img_hi); tex_lores = LoadTextureFromImage(img_lo);
        UnloadImage(img_hi); UnloadImage(img_lo);

        SetTextureFilter(tex_hires, TEXTURE_FILTER_POINT); SetTextureFilter(tex_lores, TEXTURE_FILTER_BILINEAR);
        screen_target = LoadRenderTexture(newW, newH);
        SetTextureFilter(screen_target.texture, TEXTURE_FILTER_BILINEAR);
        active_tex = &tex_hires;
    }

    bool IsMouseOverUI(Vector2 m) const {
        if (uiHidden) return false;
        if (sidebar.isOpen && CheckCollisionPointRec(m, sidebar.GetBoundingBox(uiScale))) return true;
        if (legend.isOpen) {
            float lw = 240 * uiScale, lh = 135 * uiScale, lx = width - lw - (15 * uiScale);
            if (CheckCollisionPointRec(m, {lx, 15*uiScale, lw, lh})) return true;
        }
        return false;
    }

    void HandleInput() {
        bool inputActive = false;
        Vector2 mousePos = GetMousePosition();
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

        // 1. UI Hiding Override (Ctrl + H)
        if (ctrl && IsKeyPressed(KEY_H)) uiHidden = !uiHidden;

        // 2. Global UI Scaling & Undo
        if (ctrl && !uiHidden) {
            bool plus  = IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)      || IsKeyPressed(KEY_RIGHT_BRACKET);
            bool minus = IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT) || IsKeyPressed(KEY_SLASH);

            if (plus)  uiScale = std::min(2.5f, uiScale + 0.15f);
            if (minus) uiScale = std::max(0.6f, uiScale - 0.15f);

            if (IsKeyPressed(KEY_Z)) {
                if (!sidebar.allowDragging && !historyStack.empty()) {
                    engine.warpCamera(historyStack.back());
                    historyStack.pop_back();
                    isBoxSelecting = false; needsUpdate = true;
                } else if (!sidebar.allowDragging && historyStack.empty()) {
                    redAlertTimer = 1.5f; 
                }
            }
        }

        if (sidebar.allowDragging && !historyStack.empty()) historyStack.clear();

        // 3. Canvas Navigation
        bool overUI = IsMouseOverUI(mousePos);

        if (sidebar.allowDragging) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !overUI) {
                Vector2 d = GetMouseDelta();
                if (d.x != 0 || d.y != 0) { engine.pan(d.x, d.y, width, height); inputActive = true; }
            }
        } else {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUI) {
                isBoxSelecting = true; boxStart = mousePos; boxEnd = mousePos;
            }
            if (isBoxSelecting) {
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) boxEnd = mousePos;
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    isBoxSelecting = false;
                    if (std::abs(boxEnd.x - boxStart.x) > 6 && std::abs(boxEnd.y - boxStart.y) > 6) {
                        historyStack.push_back(engine.getCurrentSnapshot());
                        engine.warpCamera(engine.calculateBoxSnapshot(boxStart.x, boxStart.y, boxEnd.x, boxEnd.y, width, height));
                        needsUpdate = true;
                    }
                }
            }
        }

        float wheel = GetMouseWheelMove() * 0.1f; 
        if (wheel != 0.0f && !ctrl) {
            engine.applyZoom(wheel, mousePos.x, mousePos.y, width, height);
            inputActive = true;
        }

        bool cameraMoving = engine.updateCamera();
        if (inputActive || cameraMoving) {
            renderScale = sidebar.upscaleFactor; needsUpdate = true; wasInteracting = true;
        } else if (wasInteracting) {
            renderScale = 1.0f; needsUpdate = true; wasInteracting = false; 
        }
    }

    void LoadDiffusionShader() {
        const char* code = R"(
            #version 330
            in vec2 fragTexCoord; in vec4 fragColor; uniform sampler2D texture0; uniform vec2 screenSize; out vec4 finalColor;
            void main() {
                vec2 o = (1.0 / screenSize) * 1.0; vec4 c = texture(texture0, fragTexCoord) * 0.226943;
                c += texture(texture0, fragTexCoord + vec2(o.x, 0)) * 0.133221 + texture(texture0, fragTexCoord + vec2(-o.x, 0)) * 0.133221;
                c += texture(texture0, fragTexCoord + vec2(0, o.y)) * 0.133221 + texture(texture0, fragTexCoord + vec2(0, -o.y)) * 0.133221;
                c += texture(texture0, fragTexCoord + vec2(o.x, o.y)) * 0.085316 + texture(texture0, fragTexCoord + vec2(-o.x, o.y)) * 0.085316;
                c += texture(texture0, fragTexCoord + vec2(o.x, -o.y)) * 0.085316 + texture(texture0, fragTexCoord + vec2(-o.x, -o.y)) * 0.085316;
                finalColor = c * fragColor;
            }
        )";
        diffusion_shader = LoadShaderFromMemory(nullptr, code);
        screen_size_loc = GetShaderLocation(diffusion_shader, "screenSize");
    }

public:
    GUI(int startW, int startH) : width(startW), height(startH), engine{engine::MandelbrotEngine::create(startH, startW)} {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(width, height, "Mandelbrot Engine");
        SetTargetFPS(60); 

        ReallocateGPUTextures(width, height);
        LoadDiffusionShader();

        engine.tryDispatchFrame(width, height, width, height, sidebar.activeRefiningIters);
        engine.waitFrameDone(); 
        (void)engine.harvestFrame();
    }

    ~GUI() {
        if (tex_hires.id != 0) UnloadTexture(tex_hires); if (tex_lores.id != 0) UnloadTexture(tex_lores);
        if (screen_target.id != 0) UnloadRenderTexture(screen_target);
        UnloadShader(diffusion_shader); CloseWindow();
    }

    void Run() {
        while (!WindowShouldClose()) {
            if (redAlertTimer > 0.0f) redAlertTimer -= GetFrameTime();

            if (IsWindowResized()) {
                width = std::min(GetScreenWidth(), static_cast<int>(MAX_WIDTH));
                height = std::min(GetScreenHeight(), static_cast<int>(MAX_HEIGHT));
                ReallocateGPUTextures(width, height); needsUpdate = true;
            }

            int renderW = std::max(1, static_cast<int>(width / renderScale));
            int renderH = std::max(1, static_cast<int>(height / renderScale));

            // 1. HARVEST
            auto frame = engine.harvestFrame();
            if (frame.uptodate) {
                UpdateTexture(frame.width == static_cast<size_t>(width) ? tex_hires : tex_lores, frame.pixels);
                active_tex = frame.width == static_cast<size_t>(width) ? &tex_hires : &tex_lores;
                isComputing = false; 
            }

            // 2. DISPATCH
            if (needsUpdate) {
                bool isFullRes = (renderScale == 1.0f);
                if (isFullRes && sidebar.disableRefinement) {
                    needsUpdate = false;
                } else {
                    bool overridePreempt = isComputing && (currentlyComputingScale > 1) && isFullRes;
                    if (!isComputing || overridePreempt) {
                        unsigned int iters = isFullRes ? sidebar.activeRefiningIters : sidebar.panningIters;
                        if (engine.tryDispatchFrame(renderW, renderH, width, height, iters)) {
                            isComputing = true; needsUpdate = false; currentlyComputingScale = static_cast<int>(renderScale);
                        }
                    }
                }
            }

            // 3. RENDER SCENE
            BeginTextureMode(screen_target);
                ClearBackground(BLACK);
                if (active_tex) DrawTexturePro(*active_tex, {0,0,(float)active_tex->width,(float)active_tex->height}, {0,0,(float)width,(float)height}, {0,0}, 0, WHITE);
            EndTextureMode();

            BeginDrawing();
                ClearBackground(BLACK);
                Rectangle fboRec = {0, 0, (float)screen_target.texture.width, -(float)screen_target.texture.height};
                if (diffusion_active) {
                    float res[2] = {(float)width, (float)height};
                    SetShaderValue(diffusion_shader, screen_size_loc, res, SHADER_UNIFORM_VEC2);
                    BeginShaderMode(diffusion_shader); DrawTexturePro(screen_target.texture, fboRec, {0,0,(float)width,(float)height}, {0,0}, 0, WHITE); EndShaderMode();
                } else {
                    DrawTexturePro(screen_target.texture, fboRec, {0,0,(float)width,(float)height}, {0,0}, 0, WHITE);
                }

                if (!uiHidden) {
                    bool isRefining = isComputing && (currentlyComputingScale == 1);
                    bool triggerReset = legend.Draw(width, uiScale, engine, isRefining, redAlertTimer);
                    auto sRes = sidebar.Draw(uiScale);

                    if (triggerReset)                 { engine.resetCamera(); historyStack.clear(); needsUpdate = true; }
                    if (sRes.needsInstantRerender)    { ReallocateGPUTextures(width, height); needsUpdate = true; }
                    if (sRes.applyIterationsClicked)  { needsUpdate = true; } // Trigger high-res re-render on Apply click

                    if (isBoxSelecting && !sidebar.allowDragging) {
                        float bx = std::min(boxStart.x, boxEnd.x), by = std::min(boxStart.y, boxEnd.y);
                        float bw = std::abs(boxEnd.x - boxStart.x), bh = std::abs(boxEnd.y - boxStart.y);
                        DrawRectangleRec({bx, by, bw, bh}, Color{0, 190, 255, 45});
                        DrawRectangleLinesEx({bx, by, bw, bh}, 1.5f, Color{0, 190, 255, 220});
                    }
                }

                HandleInput(); 
            EndDrawing();
        }
    }
};

} // namespace mandelbrot_engine