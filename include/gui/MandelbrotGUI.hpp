#pragma once
#include <raylib.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <engine/MandelbrotEngine.hpp>
#include <engine/Camera.hpp>
#include "RenderSettings.hpp"
#include "Presenter.hpp"
#include "Exporter.hpp"
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
        // ESC is raylib's DEFAULT exit key and it sets a sticky close flag, so every
        // `while (!WindowShouldClose())` — including the main loop — falls through. That
        // made ESC quit the app when it was meant to cancel an export or a field edit.
        // Disable it; the window close button still drives WindowShouldClose() normally.
        SetExitKey(KEY_NULL);
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
            if (exportMessageTimer_ > 0.0f) exportMessageTimer_ -= GetFrameTime();

            // Run a requested export before this frame's work — it owns the window while
            // it runs (its own Begin/EndDrawing) and leaves the canvas needing a repaint.
            if (pendingExport_ != Pending::NONE) {
                const Pending what = pendingExport_;
                pendingExport_ = Pending::NONE;
                if (what == Pending::FRAME) runFrameExport();
                else                        runPathExport();
                continue;
            }

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

    // ---- Export state --------------------------------------------------------
    enum class Pending { NONE, FRAME, PATH };
    Pending     pendingExport_{Pending::NONE};
    std::string exportMessage_;
    bool        exportOk_{false};
    float       exportMessageTimer_{0.0f};
    double      lastExportDraw_{0.0};   // throttles the modal redraw during an export

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
        // Full-res settles build the full reference; low-res previews use the central point.
        if (engine_.requestFrame({ cam_, rw, rh, iters, fullRes })) {
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
        if (sidebar_.allowDragging) {
            moved |= handlePan(mouse, overUI);
            // Wheel zoom is a DRAG-MODE affordance only. In box-zoom mode it would move the
            // camera out from under the rubber band, so `calculateBoxSnapshot` would resolve
            // against a viewport that no longer matches the pixels the user framed.
            moved |= handleZoom(mouse, ctrl);
        } else {
            handleBoxSelect(mouse, overUI);
        }

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
            // Re-check `overUI` on RELEASE too: the press-time check alone lets a drag that
            // started on canvas commit a zoom when it is released over a panel.
            if (!overUI &&
                std::abs(boxEnd_.x - boxStart_.x) > 6 && std::abs(boxEnd_.y - boxStart_.y) > 6) {
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

    /// True when both panels are hidden, so a single "restore everything" chip is shown.
    bool allPanelsClosed() const { return !sidebar_.isOpen && !legend_.isOpen; }

    /// Top-right chip that restores both panels. Mirrors the legend's own anchor.
    Rectangle reopenChipRect() const {
        const float mg = util::widgets::PANEL_MARGIN * uiScale_;
        const float sz = util::widgets::CHIP_SIZE   * uiScale_;
        return { width_ - sz - mg, mg, sz, sz };
    }

    bool isMouseOverUI(Vector2 m) const {
        if (uiHidden_) return false;
        if (sidebar_.isOpen && CheckCollisionPointRec(m, sidebar_.GetBoundingBox(uiScale_))) return true;
        // Ask the panel for its own rect rather than re-deriving it here — the duplicated
        // geometry used to need hand-syncing with LegendPanel.
        if (CheckCollisionPointRec(m, legend_.GetBoundingBox(width_, uiScale_))) return true;
        // The reopen chips are UI too: without this, clicking one also pans / box-selects.
        if (allPanelsClosed() && CheckCollisionPointRec(m, reopenChipRect())) return true;
        if (!sidebar_.isOpen && legend_.isOpen) {
            const float mg = util::widgets::PANEL_MARGIN * uiScale_;
            const float sz = util::widgets::CHIP_SIZE   * uiScale_;
            if (CheckCollisionPointRec(m, { mg, mg, sz, sz })) return true;
        }
        return false;
    }

    // ========================================================================
    // Export
    // ========================================================================

    void finishExport(const Exporter::Result& r) {
        exportMessage_      = r.message;
        exportOk_           = r.ok;
        exportMessageTimer_ = 4.0f;
        // The export dispatched its own frames, so whatever the loop thought was in
        // flight is gone; force a clean repaint of the live view.
        inFlight_    = false;
        renderScale_ = 1.0f;
        needsUpdate_ = true;
    }

    void runFrameExport() {
        const std::string path = Exporter::SaveDialog("Export Frame", "mandelbrot.png");
        if (path.empty()) { finishExport({ false, "Export cancelled" }); return; }

        drawModalStatus("Rendering frame...", 0.0f);
        finishExport(Exporter::ExportFrame(engine_, cam_, uWidth(), uHeight(),
                                           sidebar_.settings.activeRefiningIters, path));
    }

    void runPathExport() {
        // Length is derived from the zoom span so the perceived zoom rate is constant —
        // no prompt. Deeper views simply take proportionally longer.
        const unsigned frames = Exporter::FrameCountFor(cam_.currentZoom());

        const std::string path = Exporter::SaveDialog("Export Path Animation", "mandelbrot.mp4");
        if (path.empty()) { finishExport({ false, "Export cancelled" }); return; }

        // Static trampoline: the C-style callback carries `this` through `ctx`.
        auto progress = [](unsigned done, unsigned total, void* ctx) -> bool {
            auto* self = static_cast<GUI*>(ctx);
            if (WindowShouldClose()) return false;   // window closed -> cancel
            // Redraw at ~20 Hz, NOT once per exported frame: EndDrawing honours
            // SetTargetFPS(60), so drawing every frame would cap export throughput at 60
            // frames/sec and idle every core in between (worst at the shallow, fast start
            // of a zoom-in). Input is still polled every iteration so ESC stays responsive.
            const double now = GetTime();
            if (now - self->lastExportDraw_ > 0.05 || done == total) {
                self->lastExportDraw_ = now;
                self->drawModalStatus(
                    TextFormat("Rendering frame %u / %u  —  ESC to cancel", done, total),
                    static_cast<float>(done) / static_cast<float>(total));
            } else {
                PollInputEvents();
            }
            return !IsKeyPressed(KEY_ESCAPE);
        };

        lastExportDraw_ = 0.0;
        finishExport(Exporter::ExportPath(engine_, cam_, uWidth(), uHeight(),
                                          sidebar_.settings.activeRefiningIters,
                                          frames, path, progress, this));
    }

    /// One modal frame: the last rendered view, dimmed, with a status line and bar.
    void drawModalStatus(const char* text, float progress01) {
        const int fs = util::widgets::FontSize(uiScale_, 16);
        const float bw = 480 * uiScale_, bh = 110 * uiScale_;
        const Rectangle box = { (width_ - bw) * 0.5f, (height_ - bh) * 0.5f, bw, bh };

        BeginDrawing();
            presenter_->blitToScreen();
            DrawRectangle(0, 0, width_, height_, Color{0, 0, 0, 170});
            util::widgets::DrawPanel(box, "EXPORTING", 26.0f * uiScale_, fs);
            DrawText(text, static_cast<int>(box.x + 14*uiScale_),
                     static_cast<int>(box.y + 42*uiScale_), fs, util::UITheme::TextNormal);

            const Rectangle bar = { box.x + 14*uiScale_, box.y + 74*uiScale_,
                                    bw - 28*uiScale_, 14*uiScale_ };
            DrawRectangleRec(bar, util::UITheme::SubCardBg);
            DrawRectangleRec({bar.x, bar.y, bar.width * progress01, bar.height},
                             util::UITheme::AccentActive);
            DrawRectangleLinesEx(bar, 1.0f, util::UITheme::PanelBorder);
        EndDrawing();
    }

    // ========================================================================
    // UI overlay (drawn inside BeginDrawing)
    // ========================================================================

    void drawUI() {
        const bool refining = inFlight_ && inFlightScale_ == 1.0f;
        std::optional<unsigned int> refinementPercentage = std::nullopt;
        if(refining) refinementPercentage = engine_.latestJobStatus();
        const bool reset = legend_.Draw(width_, uiScale_, cam_.currentZoom(), refinementPercentage, redAlertTimer_);
        // The sidebar suppresses its own ">>" chip when the legend is also closed, so the
        // unified "restore both" chip below is the single affordance in that state.
        const auto s = sidebar_.Draw(uiScale_, legend_.isOpen);

        // Reset only dispatches when it actually changes the view.
        if (reset && !cam_.isAtHome()) { cam_.reset(); historyStack_.clear(); needsUpdate_ = true; }
        if (s.needsInstantRerender)    { needsUpdate_ = true; }  // Presenter resizes textures on upload
        if (s.applyIterationsClicked)  { needsUpdate_ = true; }
        if (s.paletteChanged) {
            // SetPalette only swaps the gradient now; dispatching through the normal path
            // gives the recolour frame progress reporting and preemption for free.
            engine_.SetPalette(gui::Presets[sidebar_.settings.paletteIndex].gradient);
            renderScale_ = 1.0f;
            needsUpdate_ = true;
        }

        if (allPanelsClosed()) {
            if (util::widgets::DrawChip(reopenChipRect(), "UI", GetMousePosition(),
                                        util::widgets::FontSize(uiScale_, 14))) {
                sidebar_.isOpen = true;
                legend_.isOpen  = true;
            }
        }

        // Export requests are deferred to the top of the next frame: both run modal loops
        // with their own Begin/EndDrawing, which must not nest inside this one.
        if (s.exportFrameClicked) pendingExport_ = Pending::FRAME;
        if (s.exportPathClicked)  pendingExport_ = Pending::PATH;

        if (exportMessageTimer_ > 0.0f) {
            const int fs = util::widgets::FontSize(uiScale_, 14);
            DrawText(exportMessage_.c_str(), static_cast<int>(20 * uiScale_),
                     height_ - static_cast<int>(30 * uiScale_), fs,
                     exportOk_ ? GREEN : ORANGE);
        }

        if (isBoxSelecting_ && !sidebar_.allowDragging) {
            const float bx = std::min(boxStart_.x, boxEnd_.x), by = std::min(boxStart_.y, boxEnd_.y);
            const float bw = std::abs(boxEnd_.x - boxStart_.x), bh = std::abs(boxEnd_.y - boxStart_.y);
            DrawRectangleRec({ bx, by, bw, bh }, Color{ 0, 190, 255, 45 });
            DrawRectangleLinesEx({ bx, by, bw, bh }, 1.5f, Color{ 0, 190, 255, 220 });
        }
    }
};

} // namespace gui
