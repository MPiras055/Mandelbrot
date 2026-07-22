#pragma once
#include <raylib.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <filesystem>
#include <system_error>
#include <cstdlib>
#include "../engine/MandelbrotEngine.hpp"
#include "../engine/Camera.hpp"

// OS-specific macros for pipes and null devices
#ifdef _WIN32
    #define POPEN _popen
    #define PCLOSE _pclose
    #define NULL_DEV "NUL"
#else
    #define POPEN popen
    #define PCLOSE pclose
    #define NULL_DEV "/dev/null"
#endif

namespace gui {

class Exporter {
public:
    struct Result {
        bool ok{false};
        std::string message;
    };

    // ---------------------------------------------------------------------
    // Save dialog
    // ---------------------------------------------------------------------

    static std::string SaveDialog(const char* title, const char* defaultName) {
        std::string cmd;

#if defined(_WIN32)
        // Windows: Use PowerShell to call the native .NET SaveFileDialog
        cmd = "powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; "
              "$dlg = New-Object System.Windows.Forms.SaveFileDialog; "
              "$dlg.Title = '"; cmd += title; cmd += "'; "
              "$dlg.FileName = '"; cmd += defaultName; cmd += "'; "
              "if ($dlg.ShowDialog() -eq 'OK') { $dlg.FileName }\"";
#elif defined(__APPLE__)
        // macOS: Use AppleScript via osascript
        cmd = "osascript -e 'tell application \"System Events\" to set myFile to choose file name "
              "with prompt \""; cmd += title; cmd += "\" default name \""; cmd += defaultName; cmd += "\"' "
              "-e 'POSIX path of myFile' 2>/dev/null";
#else
        // Linux: Zenity
        cmd = "zenity --file-selection --save --confirm-overwrite";
        cmd += " --title='"; cmd += title;       cmd += "'";
        cmd += " --filename='"; cmd += defaultName; cmd += "' 2>/dev/null";
#endif

        FILE* pipe = POPEN(cmd.c_str(), "r");
        if (!pipe) return {};

        char buf[4096] = {};
        std::string path;
        if (std::fgets(buf, sizeof(buf), pipe)) path = buf;
        const int rc = PCLOSE(pipe);
        
        // On Windows PowerShell, an empty string means cancelled. On Unix, rc != 0 means cancelled.
#ifndef _WIN32
        if (rc != 0) return {};
#endif

        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
        return path;
    }

    // ---------------------------------------------------------------------
    // Single frame
    // ---------------------------------------------------------------------

    static Result ExportFrame(engine::MandelbrotEngine& engine, const engine::Camera& cam,
                              unsigned w, unsigned h, unsigned iterations,
                              const std::string& path) {
        if (path.empty()) return { false, "Export cancelled" };
        if (!RenderTo(engine, cam, w, h, iterations)) return { false, "Render was aborted" };

        auto frame = engine.harvestFrame();
        if (!frame.uptodate || !frame.pixels) return { false, "Frame did not complete" };

        if (!WritePng(frame, path)) return { false, "Could not write " + path };
        return { true, "Saved " + path };
    }

    // ---------------------------------------------------------------------
    // Path animation (zoom-in)
    // ---------------------------------------------------------------------

    using ProgressFn = bool (*)(unsigned done, unsigned total, void* ctx);

    static constexpr int      VIDEO_FPS         = 30;
    static constexpr unsigned FRAMES_PER_OCTAVE = 20;
    static constexpr unsigned MAX_FRAMES        = 3000;

    static unsigned FrameCountFor(double targetZoom) {
        if (!(targetZoom > 1.0)) return 1;
        const double octaves = std::log2(targetZoom);
        const double n = std::round(octaves * FRAMES_PER_OCTAVE);
        if (n < 1.0) return 1;
        if (n > static_cast<double>(MAX_FRAMES)) return MAX_FRAMES;
        return static_cast<unsigned>(n);
    }

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
            const double t = (frames == 1) ? 1.0 : static_cast<double>(i) / (frames - 1);
            cam.warp({ start.x, start.y, std::exp(logTarget * t) });

            if (!RenderTo(engine, cam, w, h, iterations)) { cancelled = true; break; }
            auto frame = engine.harvestFrame();
            
            if (!frame.uptodate || !frame.pixels) { cancelled = true; break; }
            if (std::fwrite(frame.pixels, 1, frameBytes, pipe) != frameBytes) { cancelled = true; break; }
            if (onProgress && !onProgress(i + 1, frames, ctx)) { cancelled = true; break; }
        }

        PCLOSE(pipe); // OS-agnostic pclose macro
        cam.warp(start);

        if (cancelled) {
            std::error_code ec;
            std::filesystem::remove(outPath, ec);
            return { false, "Export cancelled" };
        }
        return { true, "Encoded " + std::to_string(frames) + " frames to " + outPath };
    }

private:
    static bool HasFfmpeg() {
#ifdef _WIN32
        static const bool present = (std::system("where ffmpeg > NUL 2>&1") == 0);
#else
        static const bool present = (std::system("command -v ffmpeg >/dev/null 2>&1") == 0);
#endif
        return present;
    }

    static FILE* OpenEncoder(unsigned w, unsigned h, const std::string& outPath) {
        char cmd[1024];
        std::snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -f rawvideo -pixel_format rgba -video_size %ux%u -framerate %d -i - "
            "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" "
            "-c:v libx264 -pix_fmt yuv420p -crf 18 -preset fast \"%s\" > %s 2>&1",
            w, h, VIDEO_FPS, outPath.c_str(), NULL_DEV); // Uses NUL on Win, /dev/null on POSIX
        return POPEN(cmd, "w");
    }

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

    static bool RenderTo(engine::MandelbrotEngine& engine, const engine::Camera& cam,
                         unsigned w, unsigned h, unsigned iterations) {
        if (!engine.requestFrame({ cam, w, h, iterations, true })) return false;
        engine.waitFrameDone();
        return true;
    }

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