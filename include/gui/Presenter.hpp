#pragma once
#include <raylib.h>
#include "engine/dto/FrameView.hpp"

namespace gui {

/**
 * @file Presenter.hpp
 * @brief Owns all GPU/raylib display state and turns engine frames into pixels.
 *
 * @details The engine renders a CPU frame (possibly at a reduced "preview"
 * resolution) into a DoubleCanvas; the Presenter uploads it to a GPU texture and
 * blits it — upscaled to the window and post-processed by a small Gaussian
 * "diffusion" shader.
 *
 * Key robustness choice: a SINGLE frame texture that is recreated to match the
 * incoming frame's EXACT width/height. Uploading an NxM frame always targets an
 * NxM texture, so the row stride can never disagree — this is what makes the
 * old "sheared / striped" artifacts impossible.
 */
class Presenter {
public:
    Presenter(int screenW, int screenH) {
        loadDiffusionShader();
        resize(screenW, screenH);
    }

    ~Presenter() {
        if (frame_tex_.id != 0) UnloadTexture(frame_tex_);
        if (target_.id != 0)    UnloadRenderTexture(target_);
        UnloadShader(diffusion_);
    }

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    /// Recreate the screen-sized composition target after a window resize.
    void resize(int screenW, int screenH) {
        screen_w_ = screenW;
        screen_h_ = screenH;
        if (target_.id != 0) UnloadRenderTexture(target_);
        target_ = LoadRenderTexture(screenW, screenH);
        SetTextureFilter(target_.texture, TEXTURE_FILTER_BILINEAR);
    }

    /**
     * @brief Upload a freshly-completed frame.
     * @details Recreates the frame texture iff the frame's dimensions changed
     * (e.g. preview <-> full resolution), so the upload is always tightly packed.
     */
    void upload(const engine::dto::FrameView& frame) {
        const int fw = static_cast<int>(frame.width);
        const int fh = static_cast<int>(frame.height);

        if (frame_tex_.id == 0 || frame_tex_.width != fw || frame_tex_.height != fh) {
            if (frame_tex_.id != 0) UnloadTexture(frame_tex_);
            Image blank = GenImageColor(fw, fh, BLANK);
            frame_tex_ = LoadTextureFromImage(blank);
            UnloadImage(blank);
            // Crisp when already full-res, smooth when it's an upscaled preview.
            const bool preview = fw < screen_w_;
            SetTextureFilter(frame_tex_, preview ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
        }
        // raylib Color and core::Pixel share layout (RGBA8); upload the raw bytes.
        UpdateTexture(frame_tex_, frame.pixels);
        has_frame_ = true;
    }

    /// Composite the current frame (upscaled to the window) into the offscreen
    /// target. Call OUTSIDE BeginDrawing (it uses BeginTextureMode).
    void composite() const {
        if (!has_frame_) return;
        BeginTextureMode(target_);
            ClearBackground(BLACK);
            DrawTexturePro(frame_tex_,
                           { 0, 0, (float)frame_tex_.width, (float)frame_tex_.height },
                           { 0, 0, (float)screen_w_, (float)screen_h_ },
                           { 0, 0 }, 0.0f, WHITE);
        EndTextureMode();
    }

    /// Blit the composited target to the screen (optionally through the diffusion
    /// shader). Call INSIDE BeginDrawing.
    void blitToScreen() const {
        // Render textures are stored bottom-up, hence the negative source height.
        const Rectangle src = { 0, 0, (float)target_.texture.width, -(float)target_.texture.height };
        const Rectangle dst = { 0, 0, (float)screen_w_, (float)screen_h_ };

        if (diffusion_on_) {
            float res[2] = { (float)screen_w_, (float)screen_h_ };
            SetShaderValue(diffusion_, screen_size_loc_, res, SHADER_UNIFORM_VEC2);
            BeginShaderMode(diffusion_);
                DrawTexturePro(target_.texture, src, dst, { 0, 0 }, 0.0f, WHITE);
            EndShaderMode();
        } else {
            DrawTexturePro(target_.texture, src, dst, { 0, 0 }, 0.0f, WHITE);
        }
    }

private:
    void loadDiffusionShader() {
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
        diffusion_ = LoadShaderFromMemory(nullptr, code);
        screen_size_loc_ = GetShaderLocation(diffusion_, "screenSize");
    }

    int screen_w_{0};
    int screen_h_{0};

    Texture2D       frame_tex_{};   // sized to the current frame's exact dims
    RenderTexture2D target_{};      // screen-sized composition buffer
    Shader          diffusion_{};
    int             screen_size_loc_{-1};
    bool            diffusion_on_{true};
    bool            has_frame_{false};
};

} // namespace gui
