#pragma once
#include <cmath>
#include <algorithm>
#include <boost/multiprecision/cpp_bin_float.hpp>
#include "core/Numeric.hpp"

/**
 * @file Camera.hpp
 * @brief Viewport state (center + zoom) with critically-damped navigation.
 *
 * @details Extracted verbatim from MandelbrotEngine so the viewport math lives in
 * one raylib-free entity. The center is BigFloat (survives deep zoom); zoom is a
 * double (its magnitude fits the exponent). `updateCamera` takes the frame delta
 * as a parameter rather than reading raylib's clock — the caller supplies dt, so
 * the core stays toolkit-independent. Behaviour is identical to the previous
 * in-engine implementation.
 *
 * @note The undo/box-zoom history is intentionally NOT owned here — it stays in
 * the GUI layer; Camera only vends/consumes `Snapshot` values.
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

private:
    // Current (damped) state — what the renderer sees this frame.
    BigFloat offsetX{-0.5};
    BigFloat offsetY{0.0};
    double zoom{1.0};

    // Target state — where navigation input is steering toward.
    BigFloat targetOffsetX{offsetX};
    BigFloat targetOffsetY{offsetY};
    double targetZoom{zoom};

public:
    // --- Accessors: current (damped) state, used for dispatching a render ---
    const BigFloat& centerX() const { return offsetX; }
    const BigFloat& centerY() const { return offsetY; }
    double currentZoom()      const { return zoom; }

    // --- Accessors: target state, used for UI tracking queries ---
    // (matches the legacy getOffsetX/getOffsetY which reported the target)
    double uiOffsetX() const { return static_cast<double>(targetOffsetX); }
    double uiOffsetY() const { return static_cast<double>(targetOffsetY); }

    void pan(float mouseDeltaX, float mouseDeltaY, unsigned int screenWidth, unsigned int screenHeight) {
        // Boost perfectly handles implicit math mixing floats, doubles, and BigFloats
        targetOffsetX -= BigFloat((mouseDeltaX / screenWidth) * (3.0 / targetZoom));
        targetOffsetY -= BigFloat((mouseDeltaY / screenHeight) * (3.0 * screenHeight / screenWidth) / targetZoom);
    }

    void applyZoom(float wheelMove,
        unsigned int mouseX, unsigned int mouseY,
        unsigned int screenWidth, unsigned int screenHeight
    ) {

        if(wheelMove == 0.0f) return;
        double aspect     = static_cast<double>(screenWidth) / screenHeight;
        double mathWidth  = 3.0 / targetZoom;
        double mathHeight = mathWidth / aspect;

        // Boost handles precision mapping seamlessly
        BigFloat mouseMathX_before = targetOffsetX - (mathWidth / 2.0) + ((double)mouseX / screenWidth) * mathWidth;
        BigFloat mouseMathY_before = targetOffsetY - (mathHeight / 2.0) + ((double)mouseY / screenHeight) * mathHeight;

        targetZoom *= (wheelMove > 0) ? 1.2 : (1.0 / 1.2);

        mathWidth = 3.0 / targetZoom;
        mathHeight = mathWidth / aspect;
        BigFloat mouseMathX_after = targetOffsetX - (mathWidth / 2.0) + ((double)mouseX / screenWidth) * mathWidth;
        BigFloat mouseMathY_after = targetOffsetY - (mathHeight / 2.0) + ((double)mouseY / screenHeight) * mathHeight;

        targetOffsetX += (mouseMathX_before - mouseMathX_after);
        targetOffsetY += (mouseMathY_before - mouseMathY_after);
    }

    /**
     * @brief Advances the damped camera toward its target.
     * @param dt Frame delta time in seconds (clamped by the caller).
     * @return true while still animating, false once settled onto the target.
     */
    bool updateCamera(float dt) {
        double diffZoom = targetZoom - zoom;
        BigFloat diffX = targetOffsetX - offsetX;
        BigFloat diffY = targetOffsetY - offsetY;
        double damping = 1.0 - std::exp(-15.0 * dt);

        zoom += diffZoom * damping;
        offsetX += diffX * damping;
        offsetY += diffY * damping;

        double moveThreshold = (3.0 / zoom) * 0.0001;

        // Use boost::multiprecision::abs for BigFloat bounds checking
        if (std::abs(diffZoom / zoom) < 0.0001 && boost::multiprecision::abs(diffX) < moveThreshold && boost::multiprecision::abs(diffY) < moveThreshold) {
            zoom = targetZoom;
            offsetX = targetOffsetX;
            offsetY = targetOffsetY;
            return false;
        }
        return true;
    }

    /**
     * @brief Resets the camera space variables immediately to their initial state.
     * Synchronizes targets to prevent erratic linear interpolation on the next tick.
     */
    void reset() {
        targetOffsetX = -0.5;
        targetOffsetY = 0.0;
        targetZoom    = 1.0;

        offsetX = targetOffsetX;
        offsetY = targetOffsetY;
        zoom    = targetZoom;
    }

    /**
     * @brief Instantly snaps the camera to a snapshot, bypassing linear damping.
     */
    void warp(const Snapshot& snap) {
        targetOffsetX = snap.x; targetOffsetY = snap.y; targetZoom = snap.z;
        offsetX       = snap.x; offsetY       = snap.y; zoom       = snap.z;
    }

    /**
     * @brief Calculates target snapshot from a screen bounding box WITHOUT modifying state.
     */
    Snapshot calculateBoxSnapshot(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2,
                                  unsigned int screenW, unsigned int screenH) const {
        unsigned int minX = std::min(x1, x2), maxX = std::max(x1, x2);
        unsigned int minY = std::min(y1, y2), maxY = std::max(y1, y2);
        double boxW = maxX - minX, boxH = maxY - minY;

        double aspect = static_cast<double>(screenW) / screenH;
        double mathW  = 3.0 / targetZoom, mathH = mathW / aspect;
        double centerPxX = minX + (boxW / 2.0), centerPxY = minY + (boxH / 2.0);

        Snapshot next;
        next.x = targetOffsetX - (mathW / 2.0) + (centerPxX / screenW) * mathW;
        next.y = targetOffsetY - (mathH / 2.0) + (centerPxY / screenH) * mathH;
        next.z = targetZoom / std::max(boxW / screenW, boxH / screenH);
        return next;
    }

    Snapshot currentSnapshot() const {
        return {targetOffsetX, targetOffsetY, targetZoom};
    }
};

} // namespace engine
