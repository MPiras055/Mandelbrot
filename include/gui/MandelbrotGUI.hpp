#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>
#include <engine/MandelbrotEngine.hpp>
#include <engine/Camera.hpp>
#include "RenderSettings.hpp"
#include "Presenter.hpp"
#include "util/SettingsSidebar.hpp"
#include "util/LegendPanel.hpp"

namespace gui {

/**
 * @brief Application front-end: window, input, and the render loop.
 *
 * @details Concerns are split so each is independently understandable:
 *   - engine::MandelbrotEngine  — CPU frame generation (owns the pixels).
 *   - engine::Camera            — viewport state; the GUI owns and drives it.
 *   - gui::Presenter            — all GPU/raylib display state (textures, shader).
 *   - SettingsSidebar / LegendPanel — the two on-screen widgets.
 *   - GUI (this class)          — glues them together and runs the loop.
 *
 * The render loop is a small state machine over two resolutions:
 *   - while navigating: render a fast, low-res PREVIEW (renderScale = upscale).
 *   - once settled:     render one sharp FULL-RES pass (renderScale = 1).
 * Only one frame is computed at a time (`inFlight`); a settle may preempt an
 * in-flight preview so refinement starts immediately.
 */
class GUI {
public:
    GUI(int startW, int startH)
        : width_(startW), height_(startH),
          engine_(static_cast<unsigned>(startW), static_cast<unsigned>(startH)) {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(width_, height_, "Mandelbrot Engine");
        SetTargetFPS(60);
        presenter_.emplace(width_, height_);

        // Render the first (full-res) frame synchronously so the window opens with
        // a picture. Mark it in-flight so the first harvest uploads and clears it.
        engine_.requestFrame({ cam_, uWidth(), uHeight(), sidebar_.settings.activeRefiningIters });
        engine_.waitFrameDone();
        inFlight_ = true;
    }

    ~GUI() {
        presenter_.reset();   // free GPU resources before the GL context dies
        CloseWindow();
    }

    void Run() {
        while (!WindowShouldClose()) {
            if (redAlertTimer_ > 0.0f) redAlertTimer_ -= GetFrameTime();

            handleInput();       // navigation + hotkeys -> camera, renderScale, needsUpdate
            handleResize();      // window resize -> textures + canvas
            harvestAndUpload();  // pull a finished frame from the engine, upload to GPU
            scheduleRender();    // request the next frame if needed

            presenter_->composite();   // frame -> offscreen target (outside BeginDrawing)

            BeginDrawing();
                ClearBackground(BLACK);
                presenter_->blitToScreen();   // target -> window (+ diffusion shader)
                if (!uiHidden_) drawUI();
            EndDrawing();
        }
    }

private:
    // ---- Window / screen ---------------------------------------------------
    static constexpr unsigned MAX_WIDTH = 3840, MAX_HEIGHT = 2160;
    int width_, height_;
    float uiScale_{1.0f};
    bool  uiHidden_{false};

    unsigned uWidth()  const { return static_cast<unsigned>(width_); }
    unsigned uHeight() const { return static_cast<unsigned>(height_); }

    // ---- Core collaborators ------------------------------------------------
    engine::MandelbrotEngine engine_;
    engine::Camera           cam_;                 // GUI owns navigation
    std::optional<Presenter> presenter_;           // built after InitWindow

    util::SettingsSidebar sidebar_;
    util::LegendPanel     legend_;

    // ---- Navigation history (box-zoom undo) --------------------------------
    std::vector<engine::Camera::Snapshot> historyStack_;
    float redAlertTimer_{0.0f};                    // "no history" flash
    bool  isBoxSelecting_{false};
    Vector2 boxStart_{0, 0}, boxEnd_{0, 0};

    // ---- Render scheduling state ------------------------------------------
    bool  needsUpdate_{false};   // a (re)dispatch is wanted
    bool  inFlight_{false};      // a frame is currently being computed
    float renderScale_{1.0f};    // 1 = full-res, >1 = downscaled preview
    float inFlightScale_{1.0f};  // scale of the in-flight frame
    bool  wasMoving_{false};     // edge-detect the moving -> settled transition

    // Render resolution for the current scale (a uniform downscale of the window).
    std::pair<unsigned, unsigned> renderDims() const {
        return { static_cast<unsigned>(std::max(1, static_cast<int>(width_  / renderScale_))),
                 static_cast<unsigned>(std::max(1, static_cast<int>(height_ / renderScale_))) };
    }

    // ========================================================================
    // Render loop steps
    // ========================================================================

    void handleResize() {
        if (!IsWindowResized()) return;
        width_  = std::min(GetScreenWidth(),  static_cast<int>(MAX_WIDTH));
        height_ = std::min(GetScreenHeight(), static_cast<int>(MAX_HEIGHT));
        presenter_->resize(width_, height_);
        engine_.resizeCanvas(uWidth(), uHeight());
        // resizeScreen aborts the in-flight frame; it will never arrive, so clear
        // the flag and request a fresh one.
        inFlight_ = false;
        needsUpdate_ = true;
    }

    void harvestAndUpload() {
        auto frame = engine_.harvestFrame();
        if (frame.uptodate) {
            presenter_->upload(frame);
            inFlight_ = false;
        }
    }

    void scheduleRender() {
        if (!needsUpdate_) return;

        const bool fullRes = (renderScale_ == 1.0f);
        if (fullRes && sidebar_.settings.disableRefinement) {
            needsUpdate_ = false;   // refinement disabled: keep the preview
            return;
        }

        // Newest-wins preemption. A newer frame preempts the in-flight one when:
        //   - the in-flight frame is a slow FULL-RES pass (any new frame preempts it —
        //     this is what keeps input responsive during a heavy render), or
        //   - a settle preempts an in-flight preview.
        // A preview does NOT preempt another in-flight preview (previews are fast; that
        // would just thrash aborts) — the latest camera is picked up when it completes.
        if (inFlight_) {
            const bool inflightFullRes = (inFlightScale_ == 1.0f);
            const bool preempt = inflightFullRes || fullRes;
            if (!preempt) return;
        }

        auto [rw, rh] = renderDims();
        const unsigned iters = fullRes ? sidebar_.settings.activeRefiningIters
                                       : sidebar_.settings.panningIters;
        // try_push aborts whatever is in flight (newest wins); backpressure via false.
        if (engine_.requestFrame({ cam_, rw, rh, iters })) {
            inFlight_      = true;
            inFlightScale_ = renderScale_;
            needsUpdate_   = false;
        }
    }

    // ========================================================================
    // Input
    // ========================================================================

    void handleInput() {
        const Vector2 mouse = GetMousePosition();
        const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

        if (ctrl && IsKeyPressed(KEY_H)) uiHidden_ = !uiHidden_;
        if (ctrl && !uiHidden_) handleUiScaleAndUndo();

        // Box-zoom history only exists while dragging is OFF.
        if (sidebar_.allowDragging && !historyStack_.empty()) historyStack_.clear();

        bool moved = false;
        const bool overUI = isMouseOverUI(mouse);
        if (sidebar_.allowDragging) moved |= handlePan(mouse, overUI);
        else                        handleBoxSelect(mouse, overUI);
        moved |= handleZoom(mouse, ctrl);

        // Advance the damped camera and translate motion into a render request.
        const bool animating = cam_.updateCamera(std::min(GetFrameTime(), 0.1f));
        if (moved || animating) {
            renderScale_ = sidebar_.settings.upscaleFactor;   // fast preview
            needsUpdate_ = true;
            wasMoving_   = true;
        } else if (wasMoving_) {
            renderScale_ = 1.0f;                              // settled: sharp pass
            needsUpdate_ = true;
            wasMoving_   = false;
        }
    }

    void handleUiScaleAndUndo() {
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)      || IsKeyPressed(KEY_RIGHT_BRACKET))
            uiScale_ = std::min(2.5f, uiScale_ + 0.15f);
        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT) || IsKeyPressed(KEY_SLASH))
            uiScale_ = std::max(0.6f, uiScale_ - 0.15f);

        if (IsKeyPressed(KEY_Z) && !sidebar_.allowDragging) {
            if (!historyStack_.empty()) {
                cam_.warp(historyStack_.back());
                historyStack_.pop_back();
                isBoxSelecting_ = false;
                needsUpdate_ = true;
            } else {
                redAlertTimer_ = 1.5f;   // nothing to undo
            }
        }
    }

    // Drag to pan. Returns true if the camera target moved.
    bool handlePan(Vector2 mouse, bool overUI) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !overUI) {
            Vector2 d = GetMouseDelta();
            if (d.x != 0 || d.y != 0) {
                cam_.pan(d.x, d.y, width_, height_);
                return true;
            }
        }
        return false;
    }

    // Drag a rectangle to zoom into it (records an undo snapshot).
    void handleBoxSelect(Vector2 mouse, bool overUI) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUI) {
            isBoxSelecting_ = true; boxStart_ = mouse; boxEnd_ = mouse;
        }
        if (!isBoxSelecting_) return;

        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) boxEnd_ = mouse;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            isBoxSelecting_ = false;
            if (std::abs(boxEnd_.x - boxStart_.x) > 6 && std::abs(boxEnd_.y - boxStart_.y) > 6) {
                historyStack_.push_back(cam_.currentSnapshot());
                cam_.warp(cam_.calculateBoxSnapshot(boxStart_.x, boxStart_.y,
                                                    boxEnd_.x, boxEnd_.y, width_, height_));
                needsUpdate_ = true;
            }
        }
    }

    // Wheel to zoom toward the cursor. Returns true if it zoomed.
    bool handleZoom(Vector2 mouse, bool ctrl) {
        const float wheel = GetMouseWheelMove() * 0.1f;
        if (wheel == 0.0f || ctrl) return false;
        cam_.applyZoom(wheel, mouse.x, mouse.y, width_, height_);
        return true;
    }

    bool isMouseOverUI(Vector2 m) const {
        if (uiHidden_) return false;
        if (sidebar_.isOpen && CheckCollisionPointRec(m, sidebar_.GetBoundingBox(uiScale_))) return true;
        if (legend_.isOpen) {
            const float lw = 240 * uiScale_, lh = 135 * uiScale_, lx = width_ - lw - (15 * uiScale_);
            if (CheckCollisionPointRec(m, { lx, 15 * uiScale_, lw, lh })) return true;
        }
        return false;
    }

    // ========================================================================
    // UI overlay (drawn inside BeginDrawing)
    // ========================================================================

    void drawUI() {
        const bool refining = inFlight_ && inFlightScale_ == 1.0f;
        const bool reset = legend_.Draw(width_, uiScale_, cam_.currentZoom(), refining, redAlertTimer_);
        const auto s = sidebar_.Draw(uiScale_);

        if (reset)                   { cam_.reset(); historyStack_.clear(); needsUpdate_ = true; }
        if (s.needsInstantRerender)  { needsUpdate_ = true; }  // Presenter resizes textures on upload
        if (s.applyIterationsClicked){ needsUpdate_ = true; }

        if (isBoxSelecting_ && !sidebar_.allowDragging) {
            const float bx = std::min(boxStart_.x, boxEnd_.x), by = std::min(boxStart_.y, boxEnd_.y);
            const float bw = std::abs(boxEnd_.x - boxStart_.x), bh = std::abs(boxEnd_.y - boxStart_.y);
            DrawRectangleRec({ bx, by, bw, bh }, Color{ 0, 190, 255, 45 });
            DrawRectangleLinesEx({ bx, by, bw, bh }, 1.5f, Color{ 0, 190, 255, 220 });
        }
    }
};

} // namespace gui
