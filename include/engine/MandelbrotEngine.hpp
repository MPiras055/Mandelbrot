#pragma once
#include <raylib.h>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>
#include <cmath>
// 1. Include the multiprecision library
#include <boost/multiprecision/cpp_bin_float.hpp> 
#include "engine/job/PerturbationJob.hpp"
#include "util/ColorUtil.hpp"
#include "EscapeTimeEngine.hpp"
#include "PerturbationEngine.hpp"
#include "util/FrameBuffer.hpp"
#include "job/RenderJobStack.hpp"


namespace engine {

using BigFloat = boost::multiprecision::cpp_bin_float_50;

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
            { 0.00f, Color{ 5, 5, 15, 255 } },       // Abyssal Navy
            { 0.14f, Color{ 0, 190, 255, 255 } },    // Electric Cyan
            { 0.28f, Color{ 255, 255, 255, 255 } },  // Peak 1: Pure White (Contour Bounce)
            { 0.42f, Color{ 235, 45, 10, 255 } },    // Magma Red
            { 0.57f, Color{ 255, 215, 0, 255 } },    // Solar Gold
            { 0.71f, Color{ 15, 5, 20, 255 } },      // Peak 2: Obsidian Black (Contour Bounce)
            { 0.85f, Color{ 180, 0, 235, 255 } },    // Neon Violet
            { 1.00f, Color{ 110, 255, 50, 255 } }    // Radioactive Lime
        },
        .smooth_shading = true,
        .root_scaling = true
    };
    
    std::vector<std::thread> threadPool;
    CACHE_ALIGN std::atomic_bool stopPool{false};
    CACHE_PAD(std::atomic_bool)

    // 3. UPGRADE CAMERA STATE TO BIGFLOAT
    // Zoom remains double because scale magnitude (e.g., 1e30) fits cleanly in 64-bit exponent boundaries
    BigFloat offsetX{-0.5};
    BigFloat offsetY{0.0};
    double zoom{1.0};
    bool deltaProbingOn{true};
    
    BigFloat targetOffsetX{offsetX};
    BigFloat targetOffsetY{offsetY};
    double targetZoom{zoom};

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
        double mathWidth = 3.0 / zoom; 
        double mathHeight = mathWidth / aspect;
        
        // Downcast to double for ETA Engine absolute bounds (Safe: only used when zoom < 1e14)
        BigFloat xMin = offsetX - (mathWidth / 2.0);
        BigFloat yMin = offsetY - (mathHeight / 2.0);
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
        
        JobStrategy strat = zoom < ZOOM_PTB_THRESH ? JobStrategy::ETA : JobStrategy::PERTURBATION;
        
    
        if (strat == JobStrategy::PERTURBATION) {
            specs.x = offsetX;
            specs.y = offsetY;
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
                    bool use_float = zoom < ZOOM_ETA_DOUBLE_THRESH;
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
    double getOffsetX() const {return static_cast<double>(targetOffsetX);}
    double getOffsetY() const {return static_cast<double>(targetOffsetY);}
    double getZoom()    const {return zoom;}
    
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

    bool updateCamera() {
        double diffZoom = targetZoom - zoom;
        BigFloat diffX = targetOffsetX - offsetX;
        BigFloat diffY = targetOffsetY - offsetY;
        float dt = std::min(GetFrameTime(), 0.1f);
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
        * Synchronizes targets to prevent erratic linear interpolation on the next updateCamera tick.
        */
    void resetCamera() {
        targetOffsetX = -0.5;
        targetOffsetY = 0.0;
        targetZoom    = 1.0;
        
        offsetX = targetOffsetX;
        offsetY = targetOffsetY;
        zoom    = targetZoom;
    }

    struct CameraSnapshot {
            BigFloat x;
            BigFloat y;
            double z;
    };

    /**
        * @brief Instantly snaps the camera to a snapshot, bypassing linear damping.
        */
    void warpCamera(const CameraSnapshot& snap) {
        targetOffsetX = snap.x; targetOffsetY = snap.y; targetZoom = snap.z;
        offsetX       = snap.x; offsetY       = snap.y; zoom       = snap.z;
    }

    /**
        * @brief Calculates target snapshot from a screen bounding box WITHOUT modifying state.
        */
    CameraSnapshot calculateBoxSnapshot(unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2, 
                                        unsigned int screenW, unsigned int screenH) const {
        unsigned int minX = std::min(x1, x2), maxX = std::max(x1, x2);
        unsigned int minY = std::min(y1, y2), maxY = std::max(y1, y2);
        double boxW = maxX - minX, boxH = maxY - minY;

        double aspect = static_cast<double>(screenW) / screenH;
        double mathW  = 3.0 / targetZoom, mathH = mathW / aspect;
        double centerPxX = minX + (boxW / 2.0), centerPxY = minY + (boxH / 2.0);

        CameraSnapshot next;
        next.x = targetOffsetX - (mathW / 2.0) + (centerPxX / screenW) * mathW;
        next.y = targetOffsetY - (mathH / 2.0) + (centerPxY / screenH) * mathH;
        next.z = targetZoom / std::max(boxW / screenW, boxH / screenH);
        return next;
    }
    
    CameraSnapshot getCurrentSnapshot() const {
        return {targetOffsetX, targetOffsetY, targetZoom};
    }
};
    
} //namespace mandelbrot_engine