#pragma once
#include "../Camera.hpp"

namespace engine::dto {

/**
 * @brief A request for the engine to render one frame.
 *
 * @details The engine is intentionally unaware of navigation: it neither owns nor
 * mutates the camera. The GUI owns the Camera, drives it, and passes a read-only
 * reference here alongside the desired render resolution. The engine reads
 * center/zoom to derive the job's complex-plane bounds.
 *
 * @note Holds a reference — build it transiently at dispatch time; do not store.
 */
struct FrameRequest {
    const engine::Camera& camera;
    unsigned int renderWidth;
    unsigned int renderHeight;
    unsigned int iterations;
    bool fullReference{true};   // false for low-res previews (central-point cache, no search)
};

} // namespace engine::dto
