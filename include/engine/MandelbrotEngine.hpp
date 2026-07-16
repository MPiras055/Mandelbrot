#pragma once
#include <cstdint>
#include <cassert>
#include "core/Numeric.hpp"
#include "concurrency/WorkerPool.hpp"
#include "engine/job/PerturbationJob.hpp"
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
 */
class MandelbrotEngine {

    enum class JobStrategy {ETA, PERTURBATION};

    struct TaggedJobSpecs {
        job::RenderJob::JobSpecs specs;
        JobStrategy strat;
    };

    static constexpr double ZOOM_ETA_DOUBLE_THRESH = 1e4;
    static constexpr double ZOOM_PTB_THRESH = 1e11;
    static constexpr size_t JOBSTACK_S_DEF = 60;

    util::Gradient global_gradient_{
        .stops = {
            { 0.00f, core::Pixel{ 5, 5, 15, 255 } },       // Abyssal Navy
            { 0.14f, core::Pixel{ 0, 190, 255, 255 } },    // Electric Cyan
            { 0.28f, core::Pixel{ 255, 255, 255, 255 } },  // Peak 1: Pure White (Contour Bounce)
            { 0.42f, core::Pixel{ 235, 45, 10, 255 } },    // Magma Red
            { 0.57f, core::Pixel{ 255, 215, 0, 255 } },    // Solar Gold
            { 0.71f, core::Pixel{ 15, 5, 20, 255 } },      // Peak 2: Obsidian Black (Contour Bounce)
            { 0.85f, core::Pixel{ 180, 0, 235, 255 } },    // Neon Violet
            { 1.00f, core::Pixel{ 110, 255, 50, 255 } }    // Radioactive Lime
        },
        .smooth_shading = true,
        .root_scaling = true
    };

    //Double buffer
    util::DoubleCanvas canvas;
    //pointer shared with the subEngines
    core::Pixel* backBuffer_shared;
    //threadpool
    concurrency::WorkerPool pool;
    //Job queue
    job::RenderJobStack jobStack;
    //subEngine ETA
    EscapeTimeEngine etaEngine;
    //subEngine PTB
    PerturbationEngine ptbEngine;

    uint64_t stampLastFlipped{0};

    /**
     * @brief Translate a FrameRequest into engine-space job specifications.
     * @details Reads center/zoom from the request's camera (read-only). Aspect is
     * taken from the render resolution (a uniform downscale of the screen, so the
     * ratio matches).
     */
    TaggedJobSpecs getTaggedJobSpecs(const dto::FrameRequest& req) {
        const double zoom = req.camera.currentZoom();
        double aspect = static_cast<double>(req.renderWidth) / req.renderHeight;
        double mathWidth = 3.0 / zoom;
        double mathHeight = mathWidth / aspect;

        // Downcast to double for ETA Engine absolute bounds (Safe: only used when zoom < 1e14)
        BigFloat xMin = req.camera.centerX() - (mathWidth / 2.0);
        BigFloat yMin = req.camera.centerY() - (mathHeight / 2.0);
        double stepX = mathWidth / req.renderWidth;
        double stepY = mathHeight / req.renderHeight;

        job::RenderJob::JobSpecs specs;
        specs.height             = req.renderHeight;
        specs.width              = req.renderWidth;
        specs.iterations         = req.iterations;
        specs.x                  = xMin;
        specs.y                  = yMin;
        specs.pixelStepX         = stepX;
        specs.pixelStepY         = stepY;
        specs.enableDeltaProbing = req.enableDeltaProbing;
        specs.useFloat           = zoom < ZOOM_ETA_DOUBLE_THRESH;

        JobStrategy strat = zoom < ZOOM_PTB_THRESH ? JobStrategy::ETA : JobStrategy::PERTURBATION;

        if (strat == JobStrategy::PERTURBATION) {
            specs.x = req.camera.centerX();
            specs.y = req.camera.centerY();
        }

        specs.chunks = (strat == JobStrategy::ETA ?
            EscapeTimeEngine::CalculateTotalChunks(req.renderWidth, req.renderHeight)
          : ptbEngine.CalculateTotalChunks(req.renderWidth, req.renderHeight));

        return {.specs = specs, .strat = strat};
    }

    bool dispatch(TaggedJobSpecs tagged) {
        // Bind the current back region to THIS job. Every worker of the job writes
        // through this captured pointer, so a later harvest flip can never split a
        // frame across regions or race a shared, re-pointed handle.

        bool done = false;
        if(tagged.strat == JobStrategy::ETA) {
            done = jobStack.try_push<job::RenderJob::ETAJob>(tagged.specs);
        } else if (tagged.strat == JobStrategy::PERTURBATION) {
            done = jobStack.try_push<job::RenderJob::PTBJob>(tagged.specs);
        } else {
            assert(false && "dispatch: unhandled job strategy");
        }
        return done;
    }

    void workerRoutine(size_t /*thread_id*/) {
        uint64_t last_version = jobStack.get_latest_job().getStamp();

        while(!pool.stopRequested()) {
            auto& job = jobStack.get_latest_job();
            last_version = job.getStamp();
            if (job.done()) {
                jobStack.wait_for_job(last_version);
                continue;
            }

            if(job.acquire(last_version)) {
                bool wait = false;
                if (job.holds<job::RenderJob::ETAJob>()) {
                    if(job.getSpecs().useFloat)
                        etaEngine.processEscapeTimeJob<float>(job);
                    else
                        etaEngine.processEscapeTimeJob<double>(job);
                } else if (job.holds<job::RenderJob::PTBJob>()) {
                    ptbEngine.processPerturbationJob(job);
                    wait = ptbEngine.hasToWait();
                }

                wait? job.release() : job.releaseWait();

            } else {
                jobStack.wait_for_job(last_version);
            }
        }
    }

    public:
    MandelbrotEngine(unsigned int width, unsigned int height)
      : canvas(width, height),
        jobStack(JOBSTACK_S_DEF),
        backBuffer_shared(canvas.back_ptr()),
        etaEngine(global_gradient_,backBuffer_shared),
        ptbEngine(global_gradient_,backBuffer_shared)
    {
        global_gradient_.prepare();

        // Spawn workers only now that every collaborator the routine touches
        // (jobStack, engines, canvas) is fully constructed.
        pool.start([this](size_t id) { workerRoutine(id); });
    }

    ~MandelbrotEngine() {
        pool.requestStop();
        job::RenderJob::JobSpecs dummy;
        jobStack.try_push<job::PerturbationJob>(dummy);
        pool.join();
    }

    /**
     * @brief Request one frame. Any in-flight frame is aborted (newest wins).
     * @return false if the ring is momentarily full (backpressure) — request dropped.
     */
    bool requestFrame(const dto::FrameRequest& req) {
        return dispatch(getTaggedJobSpecs(req));
    }

    /**
     * @brief Blocks until the latest submitted job has drained (completed/aborted).
     */
    void waitFrameDone() {
        while(!jobStack.is_latest_done()) {
            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #elif defined(__arm__) || defined(__aarch64__)
                asm volatile("yield" ::: "memory");
            #else
                std::this_thread::yield();
            #endif
        }
    }

    /**
     * @brief Harvests the latest completed frame, flipping it to the front.
     * @return A FrameView with uptodate=true only when a new frame was flipped.
     * @note The flip is done here on the (single) caller thread while workers are
     * quiesced on the completed job — see DoubleCanvas's MT-safety note.
     */
    dto::FrameView harvestFrame() {
        auto& latestJob = jobStack.get_latest_job();
        const uint64_t latest_stamp = latestJob.getStamp();
        bool upToDate = false;

        if (latest_stamp > stampLastFlipped && latestJob.completed()) {
            upToDate = true;
            job::RenderJob::JobSpecs j_specs = latestJob.getSpecs();
            canvas.swap(j_specs.width, j_specs.height); //swap backBuffer with the frontBuffer
            backBuffer_shared = canvas.back_ptr();
            stampLastFlipped = latest_stamp;
        }
        return canvas.front_view(upToDate);
    }

    /**
     * @brief Reallocates the canvas to a new screen size (window resize).
     * @warning Drains the current job first; call only from the GUI thread.
     */
    void resizeScreen(unsigned int width, unsigned int height) {
        jobStack.abort_latest();
        waitFrameDone();
        canvas.resize_screen(width, height);
        backBuffer_shared = canvas.back_ptr();
        // The next dispatch binds its target to the freshly-allocated back region.
        // Nothing valid is on-screen; don't let harvest flip the aborted job.
        stampLastFlipped = jobStack.get_latest_job().getStamp();
    }

    void SetPalette(util::Gradient new_gradient) {
        jobStack.abort_latest();
        const auto& lastJob = jobStack.get_latest_job();
        JobStrategy strat = (lastJob.holds<job::RenderJob::ETAJob>() ? JobStrategy::ETA : JobStrategy::PERTURBATION);
        TaggedJobSpecs tagged{ .specs = lastJob.getSpecs(), .strat = strat };
        waitFrameDone();
        global_gradient_ = std::move(new_gradient);
        global_gradient_.prepare();
        dispatch(tagged);
    }

    static constexpr double getPerturbationThreshold() { return ZOOM_PTB_THRESH; }
    static constexpr double getEscapeTimeDoubleThreshold() { return ZOOM_ETA_DOUBLE_THRESH; }
};

} // namespace engine
