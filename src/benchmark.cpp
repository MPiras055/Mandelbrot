#include <iostream>
#include <string>
#include <chrono>
#include "engine/Camera.hpp"
#include "engine/MandelbrotEngine.hpp"
#include "engine/dto/FrameRequest.hpp"

// Two interesting points to use as defaults
const std::string SHALLOW_X = "-0.743643887037151";
const std::string SHALLOW_Y = "0.131825904205330";

// A very deep dive into a mini-brot on the spike
const std::string DEEP_X = "-1.7499576837060935036022145060706997072711057972483";
const std::string DEEP_Y = "0.0";

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " -t <num_threads> [options]\n\n"
              << "Required:\n"
              << "  -t, --threads <N>      Number of worker threads\n\n"
              << "Options:\n"
              << "  -w, --width <N>        Render width (default: 1920)\n"
              << "  -h, --height <N>       Render height (default: 1080)\n"
              << "  -i, --iterations <N>   Max iterations (default: 10000)\n"
              << "  -z, --zoom <F>         Zoom level (default: 1e10)\n"
              << "  -x, --x-coord <S>      X coordinate (default based on zoom)\n"
              << "  -y, --y-coord <S>      Y coordinate (default based on zoom)\n"
              << "  --help                 Show this help message\n\n"
              << "Example:\n"
              << "  " << progName << " -t 16 -w 3840 -h 2160 -i 5000 -z 1e12 -x " << DEEP_X << " -y " << DEEP_Y << "\n";
}

int main(int argc, char** argv) {
    // 1. Defaults
    unsigned int numThreads = 0;
    bool threadsProvided = false;
    unsigned int renderWidth = 1920;
    unsigned int renderHeight = 1080;
    unsigned int iterations = 10000;
    double zoom = 1e10;
    std::string x_str = "";
    std::string y_str = "";

    // 2. Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            numThreads = std::stoul(argv[++i]);
            threadsProvided = true;
        } else if ((arg == "-w" || arg == "--width") && i + 1 < argc) {
            renderWidth = std::stoul(argv[++i]);
        } else if ((arg == "-h" || arg == "--height") && i + 1 < argc) {
            renderHeight = std::stoul(argv[++i]);
        } else if ((arg == "-i" || arg == "--iterations") && i + 1 < argc) {
            iterations = std::stoul(argv[++i]);
        } else if ((arg == "-z" || arg == "--zoom") && i + 1 < argc) {
            zoom = std::stod(argv[++i]);
        } else if ((arg == "-x" || arg == "--x-coord") && i + 1 < argc) {
            x_str = argv[++i];
        } else if ((arg == "-y" || arg == "--y-coord") && i + 1 < argc) {
            y_str = argv[++i];
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Error: Unknown or incomplete argument '" << arg << "'\n\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // 3. Enforce mandatory argument
    if (!threadsProvided) {
        std::cerr << "Error: Number of threads (-t) is mandatory.\n\n";
        printUsage(argv[0]);
        return 1;
    }

    // 4. Resolve fallback coordinates if not provided
    if (x_str.empty() || y_str.empty()) {
        if (zoom > engine::MandelbrotEngine::getPerturbationThreshold()) {
            x_str = DEEP_X;
            y_str = DEEP_Y;
        } else {
            x_str = SHALLOW_X;
            y_str = SHALLOW_Y;
        }
    }

    // Route diagnostics to cerr so they don't pollute stdout
    std::cerr << "[Config] Threads: " << numThreads 
              << " | Res: " << renderWidth << "x" << renderHeight 
              << " | Iters: " << iterations 
              << " | Zoom: " << zoom << "\n";

    // 5. Setup Camera and FrameRequest
    engine::Camera camera;
    engine::Camera::Snapshot snap;
    
    snap.x = engine::Camera::BigFloat(x_str);
    snap.y = engine::Camera::BigFloat(y_str);
    snap.z = zoom;
    
    camera.warp(snap);

    engine::dto::FrameRequest request {
        camera,
        renderWidth,
        renderHeight,
        iterations,
        true // fullReference
    };

    // 6. Benchmark
    engine::MandelbrotEngine engine(renderWidth, renderHeight, numThreads);
    
    std::cerr << "[Info] Starting benchmark rendering...\n";
    
    auto startTime = std::chrono::high_resolution_clock::now();
    engine.requestFrame(request);
    engine.waitFrameDone();
    auto endTime = std::chrono::high_resolution_clock::now();

    // 7. Calculate time difference
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    
    std::cerr << "[Result] Render completed in: " << durationMs << " ms\n";

    // 8. Output cleanly formatted JSON to standard output for Python extraction
    std::cout << "{"
              << "\"threads\": " << numThreads << ", "
              << "\"width\": " << renderWidth << ", "
              << "\"height\": " << renderHeight << ", "
              << "\"iterations\": " << iterations << ", "
              << "\"zoom\": " << zoom << ", "
              << "\"time_ms\": " << durationMs
              << "}\n";

    return 0; // Return 0 on success, time is passed via stdout
}