// bench_frame — headless single-frame render timer for the MandelbrotEngine.
//
// One process performs exactly ONE measured render and prints its wall-clock time
// (nanoseconds) to stdout. Running one frame per process is deliberate: it keeps every
// measurement independent of allocator/cache warmth left by a previous frame, which a
// long-lived process would accumulate. The Python aggregator launches this once per
// (engine, point, iterations, threads, repetition) and derives mean/stddev/speedup.
//
// Engine selection is implicit: MandelbrotEngine::dispatch picks ETA vs PTB from the
// camera zoom (< 1e13 -> ETA, >= 1e13 -> PTB), so a point's zoom decides the path. The
// --engine flag is validated against the point's regime purely as a guard.
//
// Usage:
//   bench_frame --engine {eta|ptb} --point NAME --threads N --iters M
//               [--width W] [--height H] [--checksum]
//   bench_frame --list-points          # print the canonical point table as CSV, then exit

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine/MandelbrotEngine.hpp"
#include "engine/Camera.hpp"
#include "engine/dto/FrameRequest.hpp"

using engine::BigFloat;

namespace {

// ---------------------------------------------------------------------------
// Canonical benchmark points — the single source of truth for coordinates.
// The Python side discovers these via --list-points; it never hard-codes them.
//
// ETA points sit in [1e4, 1e13): the representative double-SIMD escape-time path, framed on
// boundary structure so a large share of pixels reach high iteration counts (heavy enough
// that parallelism matters). PTB points sit >= 1e13: deep views that exercise the full
// reference search + BigFloat orbit build + rebasing render.
//
// Coordinates are given as decimal strings (parsed into 100-digit BigFloat). For a strong-
// scaling study the requirement is a heavy, thread-count-independent workload, not a
// specific visual target, so these may be swapped freely.
// ---------------------------------------------------------------------------
struct Point {
    const char* name;
    const char* engine;   // "eta" or "ptb" — the regime this point's zoom lands in
    const char* cx;       // centre real (decimal string)
    const char* cy;       // centre imag (decimal string)
    double      zoom;
};

constexpr Point POINTS[] = {
    // --- Escape-Time (double regime, 1e4 <= zoom < 1e13) ---
    { "eta_seahorse", "eta", "-0.743517833",              "-0.127094578",              1.0e6  },
    { "eta_spiral",   "eta", "-0.7436438870371587",       "-0.13182590420531197",      1.0e9  },
    { "eta_valley",   "eta", "0.2929859127507",           "-0.6117453120075",          1.0e11 },

    // --- Perturbation (zoom >= 1e13) ---
    { "ptb_deep1",    "ptb", "-1.7492046334760003",       "0.00002864192824913105",    1.0e14 },
    { "ptb_deep2",    "ptb", "-0.743643887037158704752",  "0.131825904205311970493",   1.0e18 },
    { "ptb_deep3",    "ptb", "0.360240443437614363236",   "-0.641313061064803174860",  1.0e24 },
};

const Point* findPoint(const std::string& name) {
    for (const Point& p : POINTS)
        if (name == p.name) return &p;
    return nullptr;
}

void listPoints() {
    std::printf("name,engine,center_x,center_y,zoom\n");
    for (const Point& p : POINTS)
        std::printf("%s,%s,%s,%s,%.6e\n", p.name, p.engine, p.cx, p.cy, p.zoom);
}

// FNV-1a over the frame bytes — a thread-count-invariant fingerprint of the workload,
// used by --checksum to prove 1-thread and N-thread runs render the identical frame.
uint64_t frameChecksum(const engine::dto::FrameView& f) {
    uint64_t h = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(f.pixels);
    const size_t n = f.width * f.height * sizeof(core::Pixel);
    for (size_t i = 0; i < n; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
    return h;
}

[[noreturn]] void usage(int code) {
    std::fprintf(stderr,
        "usage: bench_frame --engine {eta|ptb} --point NAME --threads N --iters M\n"
        "                   [--width W] [--height H] [--checksum]\n"
        "       bench_frame --list-points\n");
    std::exit(code);
}

const char* argValue(int argc, char** argv, int& i) {
    if (i + 1 >= argc) usage(2);
    return argv[++i];
}

} // namespace

int main(int argc, char** argv) {
    std::string engineArg, pointArg;
    unsigned threads = 0, iters = 0;
    unsigned width = 3840, height = 2160;   // app MAX_WIDTH/MAX_HEIGHT = "max allowed by raylib"
    bool checksum = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if      (!std::strcmp(a, "--list-points")) { listPoints(); return 0; }
        else if (!std::strcmp(a, "--engine"))   engineArg = argValue(argc, argv, i);
        else if (!std::strcmp(a, "--point"))    pointArg  = argValue(argc, argv, i);
        else if (!std::strcmp(a, "--threads"))  threads = std::strtoul(argValue(argc, argv, i), nullptr, 10);
        else if (!std::strcmp(a, "--iters"))    iters   = std::strtoul(argValue(argc, argv, i), nullptr, 10);
        else if (!std::strcmp(a, "--width"))    width   = std::strtoul(argValue(argc, argv, i), nullptr, 10);
        else if (!std::strcmp(a, "--height"))   height  = std::strtoul(argValue(argc, argv, i), nullptr, 10);
        else if (!std::strcmp(a, "--checksum")) checksum = true;
        else if (!std::strcmp(a, "--help"))     usage(0);
        else { std::fprintf(stderr, "unknown argument: %s\n", a); usage(2); }
    }

    if (pointArg.empty() || threads == 0 || iters == 0) {
        std::fprintf(stderr, "error: --point, --threads and --iters are required\n");
        usage(2);
    }

    const Point* pt = findPoint(pointArg);
    if (!pt) { std::fprintf(stderr, "error: unknown point '%s'\n", pointArg.c_str()); return 2; }
    if (!engineArg.empty() && engineArg != pt->engine) {
        std::fprintf(stderr, "error: point '%s' is a '%s' point, not '%s'\n",
                     pt->name, pt->engine, engineArg.c_str());
        return 2;
    }

    // Build the camera at the target point/magnification. warp() sets current == target, so
    // the engine reads exactly these coordinates when it dispatches.
    engine::Camera cam;
    cam.warp(engine::Camera::Snapshot{ BigFloat(pt->cx), BigFloat(pt->cy), pt->zoom });

    // Fresh engine with the requested pool size. The constructor spawns the workers (they
    // park until the first dispatch), so thread-spawn cost is outside the timed region.
    engine::MandelbrotEngine mengine(width, height, threads);

    const engine::dto::FrameRequest req{ cam, width, height, iters, /*fullReference=*/true };

    // ---- measured region: request placed -> frame drained ----
    const auto t0 = std::chrono::steady_clock::now();
    while (!mengine.requestFrame(req)) std::this_thread::yield();   // ring full is momentary
    mengine.waitFrameDone();
    const auto t1 = std::chrono::steady_clock::now();

    // Guard: a genuinely completed (non-aborted) frame must be sitting in front. Nothing
    // preempts us here, so this should always hold; if it doesn't, fail so the aggregator
    // discards the sample rather than recording a bogus time.
    const engine::dto::FrameView frame = mengine.harvestFrame();
    if (!frame.uptodate || !frame.pixels) {
        std::fprintf(stderr, "error: frame did not complete\n");
        return 1;
    }

    const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::printf("%lld\n", ns);
    if (checksum) std::fprintf(stderr, "checksum %016llx\n",
                               static_cast<unsigned long long>(frameChecksum(frame)));
    return 0;
}
