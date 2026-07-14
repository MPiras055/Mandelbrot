#pragma once
#include <raylib.h>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>
#include <cmath>
// 1. Include the multiprecision library
#include <boost/multiprecision/cpp_bin_float.hpp>
#include "core/Numeric.hpp"
#include "Camera.hpp"
#include "engine/job/PerturbationJob.hpp"
#include "util/ColorUtil.hpp"
#include "EscapeTimeEngine.hpp"
#include "PerturbationEngine.hpp"
#include "util/FrameBuffer.hpp"
#include "job/RenderJobStack.hpp"


namespace engine {

using BigFloat = core::BigFloat;

class MandelbrotEngine {

    enum class JobStrategy {ETA, PERTURBATION};

    // 2. Add string payload containers to the TaggedJobSpecs
    struct TaggedJobSpecs {
        job::RenderJob::JobSpecs specs;
        JobStrategy strat;
        std::string cx_str;
        std::string cy_str;
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
    
    std::vector<std::thread> threadPool;
    CACHE_ALIGN std::atomic_bool stopPool{false};
    CACHE_PAD(std::atomic_bool)

    // Camera state (center as BigFloat, zoom as double) extracted into its own
    // entity; MandelbrotEngine forwards the public navigation API to it.
    Camera cam;
    bool deltaProbingOn{true};

    uint64_t stampLastFlipped;
    util::FrameBuffer frontBuffer;
    size_t frontWidth{0},frontHeight{0};
    util::FrameBuffer backBuffer;
    job::RenderJobStack jobStack;
    
    EscapeTimeEngine etaEngine;
    PerturbationEngine ptbEngine;

    static bool getSystemMaxResolution(unsigned int& height, unsigned int& width) {
        InitWindow(0,0,"SystemProbe");
        unsigned int maxWidth{},maxHeight{},maxArea{};
        unsigned int currWidth{},currHeight{},currArea{};
        for(unsigned int i = 0; i < GetMonitorCount(); i++) {
            currWidth   = GetMonitorWidth(i);
            currHeight  = GetMonitorHeight(i);
            currArea    = currWidth * currHeight;
            if(currArea > maxArea) {
                maxWidth    = currWidth;
                maxHeight   = currHeight;
                maxArea     = currArea;
            }
        }
        if(maxArea == 0) return false;
        width    = maxWidth;
        height   = maxHeight;
        CloseWindow();
        return true;
    }

    TaggedJobSpecs getTaggedJobSpecs(
            unsigned int renderHeight, unsigned int renderWidth,
            unsigned int screenHeight, unsigned int screenWidth,
            unsigned int iterations
        ) {
        double aspect = (double)screenWidth / screenHeight;
        double mathWidth = 3.0 / cam.currentZoom();
        double mathHeight = mathWidth / aspect;

        // Downcast to double for ETA Engine absolute bounds (Safe: only used when zoom < 1e14)
        BigFloat xMin = cam.centerX() - (mathWidth / 2.0);
        BigFloat yMin = cam.centerY() - (mathHeight / 2.0);
        double stepX = mathWidth / renderWidth;
        double stepY = mathHeight / renderHeight;
        
        job::RenderJob::JobSpecs specs;
        specs.height             = renderHeight;
        specs.width              = renderWidth;
        specs.iterations         = iterations;
        specs.x                  = xMin;
        specs.y                  = yMin;
        specs.pixelStepX         = stepX;
        specs.pixelStepY         = stepY;
        specs.enableDeltaProbing = deltaProbingOn;
        
        JobStrategy strat = cam.currentZoom() < ZOOM_PTB_THRESH ? JobStrategy::ETA : JobStrategy::PERTURBATION;


        if (strat == JobStrategy::PERTURBATION) {
            specs.x = cam.centerX();
            specs.y = cam.centerY();
        }

        specs.chunks     = (strat == JobStrategy::ETA ? 
            EscapeTimeEngine::CalculateTotalChunks(renderWidth, renderHeight) : ptbEngine.CalculateTotalChunks(renderWidth,renderHeight));
        
        return {.specs = specs, .strat = strat};
    }

    bool tryDispatchFrame(TaggedJobSpecs tagged) {
        bool done = false;
        if(tagged.strat == JobStrategy::ETA) {
            // Standard ETA dispatch
            done = jobStack.try_push<job::RenderJob::ETAJob>(tagged.specs);
        } else if (tagged.strat == JobStrategy::PERTURBATION) {
            // 5. VARIADIC DISPATCH: Push the strings into the stack!
            done = jobStack.try_push<job::RenderJob::PTBJob>(tagged.specs);
        } else {
            assert(false && "tryDispatchFrame: unhandled job strategy");
        }
        return done;
    }

    void workerRoutine(size_t thread_id) {
        uint64_t last_version = jobStack.get_latest_job().getStamp();
        
        while(!stopPool.load(std::memory_order_relaxed)) {
            auto& job = jobStack.get_latest_job();
            last_version = job.getStamp();
            if (job.done()) {
                jobStack.wait_for_job(last_version);
                continue; 
            }
            
            if(job.acquire(last_version)) { 
                bool wait = false;
                if (job.holds<job::RenderJob::ETAJob>()) {
                    bool use_float = cam.currentZoom() < ZOOM_ETA_DOUBLE_THRESH;
                    if(use_float) 
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
    void setDeltaProbing(bool deltaProbing) {
        deltaProbingOn = deltaProbing;
    }
    

    MandelbrotEngine(
        unsigned int initialWidth, unsigned int initialHeight,
        unsigned int maxWidth, unsigned int maxHeight
    ) : 
        stampLastFlipped{0},
        frontWidth{initialWidth},
        frontHeight{initialHeight},
        frontBuffer {maxWidth * maxHeight,initialHeight, initialWidth},
        backBuffer  {maxWidth * maxHeight, initialHeight, initialWidth},
        etaEngine(backBuffer,global_gradient_),
        ptbEngine(backBuffer,global_gradient_),
        jobStack(JOBSTACK_S_DEF)
    {
        global_gradient_.prepare();

        unsigned int numCores = std::thread::hardware_concurrency();
        unsigned int poolSize = (numCores > 2) ? (numCores - 1) : 2;

        threadPool.reserve(poolSize);
        for (size_t i = 0; i < poolSize; ++i) {
            threadPool.emplace_back(&MandelbrotEngine::workerRoutine, this, i);
        }
    }

    static MandelbrotEngine create(unsigned int height,unsigned int width) {
        unsigned int sysWidth{0};
        unsigned int sysHeight{0};
        if(!getSystemMaxResolution(sysWidth, sysHeight)) {
            throw std::runtime_error("");
        }
        return MandelbrotEngine(height,width,sysHeight,sysWidth);
    }

    ~MandelbrotEngine() {
        stopPool.store(true, std::memory_order_release);
        TaggedJobSpecs t;
        jobStack.try_push<job::PerturbationJob>(t.specs);
        for (std::thread& worker : threadPool) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
    
    // Convert BigFloat back to double ONLY for UI tracking queries
    double getOffsetX() const { return cam.uiOffsetX(); }
    double getOffsetY() const { return cam.uiOffsetY(); }
    double getZoom()    const { return cam.currentZoom(); }

    void pan(float mouseDeltaX, float mouseDeltaY, unsigned int screenWidth, unsigned int screenHeight) {
        cam.pan(mouseDeltaX, mouseDeltaY, screenWidth, screenHeight);
    }

    void applyZoom(float wheelMove,
        unsigned int mouseX, unsigned int mouseY,
        unsigned int screenWidth, unsigned int screenHeight
    ) {
        cam.applyZoom(wheelMove, mouseX, mouseY, screenWidth, screenHeight);
    }

    bool updateCamera() {
        // The frame-clock read stays at the raylib boundary; Camera itself is
        // toolkit-independent and consumes the clamped delta.
        return cam.updateCamera(std::min(GetFrameTime(), 0.1f));
    }

    bool tryDispatchFrame(
        unsigned int renderWidth, unsigned int renderHeight,
        unsigned int screenWidth, unsigned int screenHeight,
        unsigned int iterations
    ) {
        return tryDispatchFrame(
            getTaggedJobSpecs(
                renderHeight,renderWidth,
                screenHeight,screenWidth,
                iterations
            )
        );
    }

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

    void SetPalette(util::Gradient new_gradient) 
    {
        jobStack.abort_latest();
        const auto& lastJob = jobStack.get_latest_job();
        JobStrategy strat = (lastJob.holds<job::RenderJob::ETAJob>()? JobStrategy::ETA : JobStrategy::PERTURBATION);
        TaggedJobSpecs tagged {.specs = lastJob.getSpecs(),.strat = strat};
        waitFrameDone(); 
        global_gradient_ = std::move(new_gradient);
        global_gradient_.prepare();
        tryDispatchFrame(tagged);
    }

    util::FrameBuffer::View harvestFrame() {
        // Look at the job that matches our last flipped stamp + 1, 
        // or look back into the stack history to find the completed frame
        auto& latestJob = jobStack.get_latest_job();
        const uint64_t latest_stamp = latestJob.getStamp();
        
        // Check if the current front buffer is outdated
        if (stampLastFlipped != latest_stamp) {
            // Find the specific job context for our pending stamp
            // If your jobStack allows querying by stamp or checking completion of historical entries:
            if (latestJob.completed() && latest_stamp > stampLastFlipped) {
                job::RenderJob::JobSpecs j_specs = latestJob.getSpecs();
                frontWidth  = j_specs.width;
                frontHeight = j_specs.height;
                
                util::FrameBuffer::swap(frontBuffer, backBuffer);
                stampLastFlipped = latest_stamp;
                return frontBuffer.getView(frontHeight, frontWidth, true);
            }
        }
        
        return frontBuffer.getView(frontHeight, frontWidth, false);
    }

    static constexpr double getPerturbationThreshold() {
        return ZOOM_PTB_THRESH;
    }

    static constexpr double getEscapeTimeDoubleThreshold() {
        return ZOOM_ETA_DOUBLE_THRESH;
    }

    /**
        * @brief Resets the camera space variables immediately to their initial state.
        */
    void resetCamera() { cam.reset(); }

    // Preserve the MandelbrotEngine::CameraSnapshot name (the GUI's undo history is
    // typed on it); the type itself now lives in Camera.
    using CameraSnapshot = Camera::Snapshot;

    /**
        * @brief Instantly snaps the camera to a snapshot, bypassing linear damping.
        */
    void warpCamera(const CameraSnapshot& snap) { cam.warp(snap); }

    /**
        * @brief Calculates target snapshot from a screen bounding box WITHOUT modifying state.
        */
    CameraSnapshot calculateBoxSnapshot(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2,
                                        unsigned int screenW, unsigned int screenH) const {
        return cam.calculateBoxSnapshot(x1, y1, x2, y2, screenW, screenH);
    }

    CameraSnapshot getCurrentSnapshot() const { return cam.currentSnapshot(); }
};
    
} //namespace mandelbrot_engine