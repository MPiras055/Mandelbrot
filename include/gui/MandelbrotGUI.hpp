#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <macro_util.hpp>
#include "engine/MandelbrotEngine.hpp"
#include "RenderSettings.hpp"
#include "util/UITheme.hpp"
#include "util/SettingsSidebar.hpp"
#include "util/LegendPanel.hpp"

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

        int lo_w = std::max(1, static_cast<int>(newW / sidebar.settings.upscaleFactor));
        int lo_h = std::max(1, static_cast<int>(newH / sidebar.settings.upscaleFactor));

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
            renderScale = sidebar.settings.upscaleFactor; needsUpdate = true; wasInteracting = true;
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

        engine.tryDispatchFrame(width, height, width, height, sidebar.settings.activeRefiningIters);
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
                if (isFullRes && sidebar.settings.disableRefinement) {
                    needsUpdate = false;
                } else {
                    bool overridePreempt = isComputing && (currentlyComputingScale > 1) && isFullRes;
                    if (!isComputing || overridePreempt) {
                        unsigned int iters = isFullRes ? sidebar.settings.activeRefiningIters : sidebar.settings.panningIters;
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

} // namespace gui