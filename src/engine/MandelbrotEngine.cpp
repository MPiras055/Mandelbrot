#include "engine/MandelbrotEngine.hpp"
#include <cassert>
#include <utility>
#include <thread>

namespace engine {

MandelbrotEngine::MandelbrotEngine(unsigned int width, unsigned int height)
  : canvas(width, height),
    backBuffer_shared(canvas.back_ptr()),
    jobStack(JOBSTACK_S_DEF),
    etaEngine(global_gradient_, backBuffer_shared),
    ptbEngine(global_gradient_, backBuffer_shared)
{
    global_gradient_.prepare();

    // Spawn workers only now that every collaborator the routine touches
    // (jobStack, engines, canvas) is fully constructed.
    pool.start([this](size_t id) { workerRoutine(id); });
}

MandelbrotEngine::~MandelbrotEngine() {
    pool.requestStop();
    job::RenderJob::JobSpecs dummy;
    jobStack.try_push<job::PerturbationJob>(dummy);
    pool.join();
}

MandelbrotEngine::TaggedJobSpecs MandelbrotEngine::getTaggedJobSpecs(const dto::FrameRequest& req) {
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

bool MandelbrotEngine::dispatch(TaggedJobSpecs tagged) {
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

void MandelbrotEngine::workerRoutine(size_t /*thread_id*/) {
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

bool MandelbrotEngine::requestFrame(const dto::FrameRequest& req) {
    return dispatch(getTaggedJobSpecs(req));
}

void MandelbrotEngine::waitFrameDone() {
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

dto::FrameView MandelbrotEngine::harvestFrame() {
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

void MandelbrotEngine::resizeCanvas(unsigned int width, unsigned int height) {
    jobStack.abort_latest();
    waitFrameDone();
    canvas.resize_screen(width, height);
    backBuffer_shared = canvas.back_ptr();
    // The next dispatch binds its target to the freshly-allocated back region.
    // Nothing valid is on-screen; don't let harvest flip the aborted job.
    stampLastFlipped = jobStack.get_latest_job().getStamp();
}

void MandelbrotEngine::SetPalette(util::Gradient new_gradient) {
    jobStack.abort_latest();
    const auto& lastJob = jobStack.get_latest_job();
    JobStrategy strat = (lastJob.holds<job::RenderJob::ETAJob>() ? JobStrategy::ETA : JobStrategy::PERTURBATION);
    TaggedJobSpecs tagged{ .specs = lastJob.getSpecs(), .strat = strat };
    waitFrameDone();
    global_gradient_ = std::move(new_gradient);
    global_gradient_.prepare();
    dispatch(tagged);
}

} // namespace engine
