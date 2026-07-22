#include "engine/MandelbrotEngine.hpp"
#include "engine/EscapeTimeEngine.hpp"
#include "engine/PerturbationEngine.hpp"
#include "engine/dto/FrameRequest.hpp"
#include "engine/job/EscapeTimeJob.hpp"
#include "engine/job/RenderJobStack.hpp"
#include <complex>
#include <cassert>
#include <chrono>
#include <thread>
#include <utility>

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
    //push a ghost frame that threads won't see
    jobStack.try_push<job::EscapeTimeJob>(job::RenderJob::JobSpecs());
    pool.join();
}

bool MandelbrotEngine::dispatch(const dto::FrameRequest& req) {
    const double zoom = req.camera.currentZoom();
    double aspect = static_cast<double>(req.renderWidth) / req.renderHeight;
    double mathWidth     = 3.0 / zoom;
    double mathHeight    = mathWidth / aspect;

    //determine the strategy
    const JobStrategy strat = zoom < ZOOM_PTB_THRESH ? JobStrategy::ETA : JobStrategy::PERTURBATION;

    //used to extract the min or the center point
    std::complex<double> relative_ref = strat == MandelbrotEngine::JobStrategy::ETA?
        std::complex<double>{mathWidth / 2.0, mathHeight / 2.0} : std::complex<double>{0.0};

    unsigned int chunks = strat == JobStrategy::ETA? EscapeTimeEngine::getChunks(req.renderWidth,req.renderHeight) :
        PerturbationEngine::getChunks(req.renderWidth,req.renderHeight);
    
    auto reference  = std::complex<BigFloat>{req.camera.centerX(), req.camera.centerY()} - static_cast<std::complex<BigFloat>>(relative_ref);
    std::complex<double> pixelStep  = {mathWidth / req.renderWidth, mathHeight / req.renderHeight};
    unsigned int iterations = req.iterations;

    job::RenderJob::JobSpecs specs(reference,pixelStep,req.renderWidth,req.renderHeight,req.iterations,chunks,zoom< ZOOM_ETA_DOUBLE_THRESH, req.fullReference);

    // JobStrategy has exactly these two enumerators — no default needed.
    return (strat == JobStrategy::ETA)
        ? jobStack.try_push<job::RenderJob::ETAJob>(specs)
        : jobStack.try_push<job::RenderJob::PTBJob>(specs);
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
            if (job.holds<job::RenderJob::ETAJob>()) {
                if(job.getSpecs().useFloat)
                    etaEngine.processEscapeTimeJob<float>(job);
                else
                    etaEngine.processEscapeTimeJob<double>(job);
            } else if (job.holds<job::RenderJob::PTBJob>()) {
                ptbEngine.processPerturbationJob(job);
            }
            job.release();
        } else {
            jobStack.wait_for_job(last_version);
        }
    }
}

/**
 * @brief Block until the newest job finishes.
 *
 * @details Backs off in three stages rather than spinning flat out: a short `pause` spin
 * keeps latency low for the common quick wait (the constructor, `resizeCanvas`), then
 * yielding, then a real sleep. A pure spin pinned a whole core for the duration of every
 * deep render — very visible during a path export, where this is called once per frame.
 */
void MandelbrotEngine::waitFrameDone() {
    unsigned spins = 0;
    while(!jobStack.is_latest_done()) {
        if (spins < 512) {
            ++spins;
            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #elif defined(__arm__) || defined(__aarch64__)
                asm volatile("yield" ::: "memory");
            #else
                std::this_thread::yield();
            #endif
        } else if (spins < 1024) {
            ++spins;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
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

/**
 * @brief Swap the active colour gradient. Does NOT dispatch — the caller requests the
 * repaint through the normal frame path, so it gets progress reporting and preemption
 * like any other frame.
 *
 * @note The abort + wait are load-bearing, not ceremony: workers hold a
 * `const util::Gradient&` into `global_gradient_` for the life of a job, so swapping it
 * while one is in flight is a data race. Aborting first keeps the wait short and bounded.
 */
void MandelbrotEngine::SetPalette(util::Gradient new_gradient) {
    jobStack.abort_latest();
    waitFrameDone();
    global_gradient_ = std::move(new_gradient);
    global_gradient_.prepare();
}

} // namespace engine
