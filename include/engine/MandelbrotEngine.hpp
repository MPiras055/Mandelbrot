#pragma once
#include <cstdint>
#include "core/Numeric.hpp"
#include "concurrency/WorkerPool.hpp"
#include "util/ColorUtil.hpp"
#include "DoubleCanvas.hpp"
#include "EscapeTimeEngine.hpp"
#include "PerturbationEngine.hpp"
#include "job/RenderJobStack.hpp"
#include "dto/FrameRequest.hpp"
#include "dto/FrameView.hpp"

namespace engine {

using BigFloat = core::BigFloat;

/**
 * @brief Pure frame-generation core: takes FrameRequests, produces FrameViews.
 *
 * @details The engine is unaware of navigation — it neither owns nor mutates a
 * camera. The GUI owns the Camera and passes a read-only reference inside each
 * FrameRequest; the engine derives the complex-plane bounds from it. Rendered
 * pixels live in a DoubleCanvas (front/back regions sized to the current screen).
 *
 * The class layout (members, thresholds) lives here; the driver methods are
 * defined in MandelbrotEngine.cpp.
 */
class MandelbrotEngine {
public:
    MandelbrotEngine(unsigned int width, unsigned int height);
    ~MandelbrotEngine();

    MandelbrotEngine(const MandelbrotEngine&) = delete;
    MandelbrotEngine& operator=(const MandelbrotEngine&) = delete;

    /// Request one frame. Any in-flight frame is aborted (newest wins).
    /// @return false if the ring is momentarily full (backpressure) — request dropped.
    bool requestFrame(const dto::FrameRequest& req) {
        return dispatch(req);
    }

    /// Blocks until the latest submitted job has drained (completed/aborted).
    void waitFrameDone();

    /// Harvests the latest completed frame, flipping it to the front. `uptodate`
    /// is true only when a new frame was flipped.
    dto::FrameView harvestFrame();

    /// Reallocate the canvas to a new screen size (window resize). Drains the
    /// in-flight job first; call only from the GUI thread.
    void resizeCanvas(unsigned int width, unsigned int height);

    void SetPalette(util::Gradient new_gradient);

    static constexpr double getPerturbationThreshold()     { return ZOOM_PTB_THRESH; }
    static constexpr double getEscapeTimeDoubleThreshold() { return ZOOM_ETA_DOUBLE_THRESH; }

    /// Perturbation reference rebuilds so far (telemetry; one per rendered PTB frame).
    uint64_t ptbRebuildCount() const { return ptbEngine.rebuildCount(); }

    /**
     * @brief: returns the current job percentage
     * 
     * @note: only significative if the job is being process, doesn't account 
     * for aborted jobs
     */
    unsigned int latestJobStatus() const noexcept {
        return jobStack.get_latest_job().percentageStatus();
    }

private:
    enum class JobStrategy { ETA, PERTURBATION };

    static constexpr double ZOOM_ETA_DOUBLE_THRESH = 1e4;
    static constexpr double ZOOM_PTB_THRESH = 1e13;
    static constexpr size_t JOBSTACK_S_DEF = 60;


    /**
     * @brief dispatch a new rendering packed in a FrameRequest
     * @returns: true if the frame was correctly dispatched, false if the dispatch queue is full at the moment
     * 
     */
    bool dispatch(const dto::FrameRequest&);
    
    void workerRoutine(size_t thread_id);

    util::Gradient global_gradient_{
        .stops = {
            { 0.00f, core::Pixel{ 0, 2, 10, 255 } },       // The Abyss (Near Black)
            { 0.16f, core::Pixel{ 32, 107, 203, 255 } },   // Mid-tone Blue Ridge
            { 0.42f, core::Pixel{ 237, 255, 255, 255 } },  // Intense Icy Highlight
            { 0.64f, core::Pixel{ 255, 170, 0, 255 } },    // Warm Orange Slope
            { 0.85f, core::Pixel{ 10, 5, 0, 255 } },       // Deep Brown/Black Crevasse
            { 1.00f, core::Pixel{ 0, 2, 10, 255 } }        // Loop back to Abyss
        },
        .smooth_shading = true,
        .root_scaling = true
    };

    util::DoubleCanvas canvas;        // double buffer
    core::Pixel* backBuffer_shared;   // pointer shared with the sub-engines
    concurrency::WorkerPool pool;     // thread pool
    job::RenderJobStack jobStack;     // job queue
    EscapeTimeEngine etaEngine;       // sub-engine ETA
    PerturbationEngine ptbEngine;     // sub-engine PTB

    uint64_t stampLastFlipped{0};
};

} // namespace engine
