#include "engine/Camera.hpp"
#include <cmath>
#include <algorithm>
#include <boost/multiprecision/cpp_bin_float.hpp>

namespace engine {

void Camera::pan(float mouseDeltaX, float mouseDeltaY, unsigned int screenWidth, unsigned int screenHeight) {
    // Boost perfectly handles implicit math mixing floats, doubles, and BigFloats
    targetOffsetX -= BigFloat((mouseDeltaX / screenWidth) * (3.0 / targetZoom));
    targetOffsetY -= BigFloat((mouseDeltaY / screenHeight) * (3.0 * screenHeight / screenWidth) / targetZoom);
}

void Camera::applyZoom(float wheelMove,
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

bool Camera::updateCamera(float dt) {
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

void Camera::reset() {
    targetOffsetX = -0.5;
    targetOffsetY = 0.0;
    targetZoom    = 1.0;

    offsetX = targetOffsetX;
    offsetY = targetOffsetY;
    zoom    = targetZoom;
}

void Camera::warp(const Snapshot& snap) {
    targetOffsetX = snap.x; targetOffsetY = snap.y; targetZoom = snap.z;
    offsetX       = snap.x; offsetY       = snap.y; zoom       = snap.z;
}

Camera::Snapshot Camera::calculateBoxSnapshot(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2,
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

} // namespace engine
