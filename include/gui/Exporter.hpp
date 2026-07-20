#pragma once
#include <raylib.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <filesystem>
#include <system_error>
#include "../engine/MandelbrotEngine.hpp"
#include "../engine/Camera.hpp"

/**
 * @file Exporter.hpp
 * @brief PNG frame export and zoom-out path animation.
 *
 * @details Both flows render synchronously through the normal engine path
 * (`requestFrame` -> `waitFrameDone` -> `harvestFrame`) and write with raylib's
 * `ExportImage`. `core::Pixel` is byte-identical to raylib `Color`, so frames are wrapped
 * without conversion — and because the buffer is engine-owned, the wrapping `Image` must
 * never be unloaded.
 *
 * External tools are invoked rather than vendored: `zenity` for the save dialog and
 * `ffmpeg` for encoding. Both are optional at runtime — a missing tool degrades (no
 * dialog / PNG sequence kept) rather than failing.
 */
namespace gui {

class Exporter {
public:
    /// Outcome of an export, for reporting back to the user in the UI.
    struct Result {
        bool ok{false};
        std::string message;
    };

    // ---------------------------------------------------------------------
    // Save dialog
    // ---------------------------------------------------------------------

    /// Native save dialog via `zenity`. Returns an empty string if the user cancelled
    /// or zenity is unavailable.
    static std::string SaveDialog(const char* title, const char* defaultName) {
        std::string cmd = "zenity --file-selection --save --confirm-overwrite";
        cmd += " --title='"; cmd += title;       cmd += "'";
        cmd += " --filename='"; cmd += defaultName; cmd += "' 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return {};

        char buf[4096] = {};
        std::string path;
        if (std::fgets(buf, sizeof(buf), pipe)) path = buf;
        const int rc = pclose(pipe);
        if (rc != 0) return {};   // cancelled, or zenity missing

        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        return path;
    }

    // ---------------------------------------------------------------------
    // Single frame
    // ---------------------------------------------------------------------

    /**
     * @brief Render the current view at @p w x @p h and write it as a PNG.
     * @note Renders at the CURRENT window resolution. Going higher would require
     * `resizeCanvas` + restore, which invalidates what is on screen and forces a
     * re-dispatch — not worth the fragility.
     */
    static Result ExportFrame(engine::MandelbrotEngine& engine, const engine::Camera& cam,
                              unsigned w, unsigned h, unsigned iterations,
                              const std::string& path) {
        if (path.empty()) return { false, "Export cancelled" };
        if (!RenderTo(engine, cam, w, h, iterations)) return { false, "Render was aborted" };

        auto frame = engine.harvestFrame();
        // An aborted job leaves `uptodate == false` and returns the STALE front buffer —
        // writing that would silently save the wrong image.
        if (!frame.uptodate || !frame.pixels) return { false, "Frame did not complete" };

        if (!WritePng(frame, path)) return { false, "Could not write " + path };
        return { true, "Saved " + path };
    }

    // ---------------------------------------------------------------------
    // Path animation (zoom-in)
    // ---------------------------------------------------------------------

    /// Per-frame progress callback. Return false to cancel the export.
    using ProgressFn = bool (*)(unsigned done, unsigned total, void* ctx);

    static constexpr int      VIDEO_FPS         = 30;
    static constexpr unsigned FRAMES_PER_OCTAVE = 20;      // ~0.67 s per zoom doubling
    static constexpr unsigned MAX_FRAMES        = 3000;    // guard against absurd exports

    /// Frames needed to travel from zoom 1.0 to @p targetZoom at a constant perceived
    /// zoom rate. Derived from the octave count so deep views take proportionally longer.
    static unsigned FrameCountFor(double targetZoom) {
        if (!(targetZoom > 1.0)) return 1;
        const double octaves = std::log2(targetZoom);
        const double n = std::round(octaves * FRAMES_PER_OCTAVE);
        if (n < 1.0) return 1;
        if (n > static_cast<double>(MAX_FRAMES)) return MAX_FRAMES;
        return static_cast<unsigned>(n);
    }

    /**
     * @brief Render a geometric zoom-IN from zoom 1.0 to the current view, streaming the
     * frames straight into ffmpeg.
     *
     * @details The zoom schedule is LOGARITHMIC — `z_t = exp(log(target) * t)` — driving
     * `Camera::warp` directly. `Camera::updateCamera` interpolates zoom *linearly*, so
     * leaning on the built-in damping would spend nearly every frame in the last decade and
     * snap through the first twelve. Frames are produced in playback order (frame 0 = the
     * wide view), so no reversal is needed anywhere.
     *
     * The centre is held at the target point for the whole flight, which keeps it
     * stationary on screen; the opening frame is therefore the set framed around the
     * target's coordinates rather than the canonical -0.5+0i home framing.
     *
     * Pixels go straight to ffmpeg's stdin as raw RGBA — no PNG encoding in this process,
     * so the encoder runs concurrently in its own process instead of stalling the render
     * workers between frames. The camera is always restored.
     */
    static Result ExportPath(engine::MandelbrotEngine& engine, engine::Camera& cam,
                             unsigned w, unsigned h, unsigned iterations,
                             unsigned frames, const std::string& outPath,
                             ProgressFn onProgress, void* ctx) {
        if (outPath.empty()) return { false, "Export cancelled" };
        if (frames == 0)     return { false, "Frame count must be at least 1" };

        if (!HasFfmpeg())
            return ExportPathAsPngs(engine, cam, w, h, iterations, frames, outPath, onProgress, ctx);

        const engine::Camera::Snapshot start = cam.currentSnapshot();
        const double logTarget = std::log(start.z > 1.0 ? start.z : 1.0);

        FILE* pipe = OpenEncoder(w, h, outPath);
        if (!pipe) { cam.warp(start); return { false, "Could not start ffmpeg" }; }

        const size_t frameBytes = static_cast<size_t>(w) * h * 4;
        bool cancelled = false;

        for (unsigned i = 0; i < frames; ++i) {
            // t: 0 -> 1. Zoom grows geometrically from 1.0 up to the target, so forward
            // playback zooms IN.
            const double t = (frames == 1) ? 1.0 : static_cast<double>(i) / (frames - 1);
            cam.warp({ start.x, start.y, std::exp(logTarget * t) });

            if (!RenderTo(engine, cam, w, h, iterations)) { cancelled = true; break; }
            auto frame = engine.harvestFrame();
            // An aborted job leaves `uptodate == false` and hands back the STALE front
            // buffer — writing that would silently splice a wrong frame into the video.
            if (!frame.uptodate || !frame.pixels) { cancelled = true; break; }

            if (std::fwrite(frame.pixels, 1, frameBytes, pipe) != frameBytes) { cancelled = true; break; }

            if (onProgress && !onProgress(i + 1, frames, ctx)) { cancelled = true; break; }
        }

        pclose(pipe);
        cam.warp(start);   // always restore, cancelled or not

        if (cancelled) {
            std::error_code ec;
            std::filesystem::remove(outPath, ec);   // drop the partial video
            return { false, "Export cancelled" };
        }
        return { true, "Encoded " + std::to_string(frames) + " frames to " + outPath };
    }

private:
    /// Probe once for ffmpeg. Checking `pclose`'s status instead would only tell us after
    /// every frame had already been streamed into a doomed pipe.
    static bool HasFfmpeg() {
        static const bool present = (std::system("command -v ffmpeg >/dev/null 2>&1") == 0);
        return present;
    }

    /// Long-lived raw-RGBA -> H.264 encoder pipe.
    static FILE* OpenEncoder(unsigned w, unsigned h, const std::string& outPath) {
        char cmd[1024];
        std::snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -f rawvideo -pixel_format rgba -video_size %ux%u -framerate %d -i - "
            // yuv420p requires EVEN dimensions and the window can be resized to an odd size.
            "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" "
            "-c:v libx264 -pix_fmt yuv420p -crf 18 -preset fast \"%s\" >/dev/null 2>&1",
            w, h, VIDEO_FPS, outPath.c_str());
        return popen(cmd, "w");
    }

    /// Fallback when ffmpeg is absent: write a numbered PNG sequence beside the target.
    static Result ExportPathAsPngs(engine::MandelbrotEngine& engine, engine::Camera& cam,
                                   unsigned w, unsigned h, unsigned iterations,
                                   unsigned frames, const std::string& outPath,
                                   ProgressFn onProgress, void* ctx) {
        const engine::Camera::Snapshot start = cam.currentSnapshot();
        const double logTarget = std::log(start.z > 1.0 ? start.z : 1.0);

        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(outPath).parent_path() / "frames";
        std::filesystem::create_directories(dir, ec);
        if (ec) { cam.warp(start); return { false, "Could not create " + dir.string() }; }

        bool cancelled = false;
        for (unsigned i = 0; i < frames; ++i) {
            const double t = (frames == 1) ? 1.0 : static_cast<double>(i) / (frames - 1);
            cam.warp({ start.x, start.y, std::exp(logTarget * t) });

            if (!RenderTo(engine, cam, w, h, iterations)) { cancelled = true; break; }
            auto frame = engine.harvestFrame();
            if (!frame.uptodate || !frame.pixels) { cancelled = true; break; }

            char name[64];
            std::snprintf(name, sizeof(name), "frame_%05u.png", i);
            if (!WritePng(frame, (dir / name).string())) { cancelled = true; break; }

            if (onProgress && !onProgress(i + 1, frames, ctx)) { cancelled = true; break; }
        }

        cam.warp(start);
        if (cancelled) {
            std::filesystem::remove_all(dir, ec);
            return { false, "Export cancelled — frames removed" };
        }
        return { false, "ffmpeg unavailable — PNG sequence written to " + dir.string() };
    }

    /// Dispatch one full-reference frame and block until it lands.
    static bool RenderTo(engine::MandelbrotEngine& engine, const engine::Camera& cam,
                         unsigned w, unsigned h, unsigned iterations) {
        if (!engine.requestFrame({ cam, w, h, iterations, true })) return false;
        engine.waitFrameDone();
        return true;
    }

    /// Wrap an engine frame as a raylib Image and write it. The pixel buffer is
    /// engine-owned, so the Image is never unloaded.
    static bool WritePng(const engine::dto::FrameView& frame, const std::string& path) {
        Image img{};
        img.data    = const_cast<void*>(static_cast<const void*>(frame.pixels));
        img.width   = static_cast<int>(frame.width);
        img.height  = static_cast<int>(frame.height);
        img.mipmaps = 1;
        img.format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        return ExportImage(img, path.c_str());
    }

};

} // namespace gui
