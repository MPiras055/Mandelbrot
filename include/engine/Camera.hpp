#pragma once
#include "core/Numeric.hpp"

/**
 * @file Camera.hpp
 * @brief Viewport state (center + zoom) with critically-damped navigation.
 *
 * @details The center is BigFloat (survives deep zoom); zoom is a double (its
 * magnitude fits the exponent). `updateCamera` takes the frame delta as a
 * parameter rather than reading raylib's clock — the caller supplies dt, so the
 * core stays toolkit-independent.
 *
 * @note The undo/box-zoom history is intentionally NOT owned here — it stays in
 * the GUI layer; Camera only vends/consumes `Snapshot` values. Trivial accessors
 * stay inline; the navigation math lives in Camera.cpp.
 */
namespace engine {

class Camera {
public:
    using BigFloat = core::BigFloat;

    struct Snapshot {
        BigFloat x;
        BigFloat y;
        double z;
    };

    // --- Accessors: current (damped) state, used for dispatching a render ---
    const BigFloat& centerX() const { return offsetX; }
    const BigFloat& centerY() const { return offsetY; }
    double currentZoom()      const { return zoom; }

    /// The TARGET state (not the damped current one) — this is what undo snapshots store.
    Snapshot currentSnapshot() const { return { targetOffsetX, targetOffsetY, targetZoom }; }

    /// True when the target is already the default view, so a reset would be a no-op.
    /// Lets the GUI skip dispatching a frame identical to the one on screen.
    bool isAtHome() const {
        return targetZoom == 1.0
            && targetOffsetX == BigFloat(-0.5)
            && targetOffsetY == BigFloat(0.0);
    }

    // --- Navigation (defined in Camera.cpp) ---
    void pan(float mouseDeltaX, float mouseDeltaY, unsigned int screenWidth, unsigned int screenHeight);
    void applyZoom(float wheelMove, unsigned int mouseX, unsigned int mouseY,
                   unsigned int screenWidth, unsigned int screenHeight);
    /// Advances the damped camera toward its target; returns true while animating.
    bool updateCamera(float dt);
    void reset();
    void warp(const Snapshot& snap);
    /// Target snapshot from a screen bounding box, WITHOUT modifying state.
    Snapshot calculateBoxSnapshot(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2,
                                  unsigned int screenW, unsigned int screenH) const;

private:
    // Current (damped) state — what the renderer sees this frame.
    BigFloat offsetX{-0.5};
    BigFloat offsetY{0.0};
    double zoom{1.0};

    // Target state — where navigation input is steering toward.
    BigFloat targetOffsetX{offsetX};
    BigFloat targetOffsetY{offsetY};
    double targetZoom{zoom};
};

} // namespace engine
