#pragma once
#include <raylib.h>
#include <cmath>
#include <vector>
#include <algorithm> // Required for std::sort

namespace engine::util {
/**
 * @brief Represents a single color marker on a continuous spectrum.
 */
struct GradientStop {
    float position; // Anchor point from 0.0f (start) to 1.0f (end)
    Color color;    // The Raylib color at this anchor point
};

/**
 * @struct Gradient
 * @brief Encapsulates a complete coloring scheme profile.
 */
struct Gradient {
    std::vector<GradientStop> stops;
    bool smooth_shading{true};
    bool root_scaling{false};

    /**
     * @brief Normalizes and prepares the internal gradient anchors.
     */
    void prepare() {
        std::sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) {
            return a.position < b.position;
        });
    }
};

/**
 * @brief A purely stateless, thread-safe mathematical color utility class.
 * @details Because it contains no mutable state, threads do not need locks to evaluate colors.
 * The application layer is responsible for storing the settings and passing them into these functions.
 */
class ColorUtil {
public:
    // Internal constant for fallback hard-limits, if required by external bounds logic.
    static constexpr size_t MAX_ITER = 2048;

    // --- UTILITIES & HELPERS ---

    /**
     * @brief Sorts a gradient vector in-place.
     * @note The UI or Application layer MUST call this once whenever a new gradient is built, 
     * BEFORE passing it to the compute functions.
     */
    static void SortGradient(std::vector<GradientStop>& stops) {
        std::sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) {
            return a.position < b.position;
        });
    }

    /**
     * @brief Linearly interpolates (LERPs) between two distinct RGBA colors.
     */
    static inline Color LerpColor(Color c1, Color c2, float t) noexcept {
        return Color{
            static_cast<unsigned char>(c1.r + t * (c2.r - c1.r)),
            static_cast<unsigned char>(c1.g + t * (c2.g - c1.g)),
            static_cast<unsigned char>(c1.b + t * (c2.b - c1.b)),
            255 // Force full alpha opacity
        };
    }

    /**
     * @brief Scans a pre-sorted gradient vector to evaluate the exact RGB value at ratio 't'.
     */
    static inline Color SampleGradient(const std::vector<GradientStop>& stops, float t) noexcept {
        if (stops.empty()) return BLACK;
        
        // Clamp boundaries to prevent out-of-bounds evaluation artifacts
        if (t <= stops.front().position) return stops.front().color;
        if (t >= stops.back().position) return stops.back().color;

        for (size_t i = 0; i < stops.size() - 1; ++i) {
            if (t >= stops[i].position && t <= stops[i+1].position) {
                // Map global 't' value to a local [0.0, 1.0] fractional range
                float segment_t = (t - stops[i].position) / (stops[i+1].position - stops[i].position);
                return LerpColor(stops[i].color, stops[i+1].color, segment_t);
            }
        }
        return BLACK;
    }

    // --- CORE KERNEL EVALUATORS ---

    /**
     * @brief Computes the exact pixel color on the fly, dynamically applying log-log smooth shading.
     * @param iter            The raw integer iteration score calculated by the escape test loop.
     * @param active_max_iter The current dynamic maximum loop cutoff restriction.
     * @param r2              The magnitude squared of complex number Z (x^2 + y^2) at escape moment.
     * @param stops           The gradient definition (MUST be pre-sorted).
     * @param smooth_shading  Toggle for continuous log-log mathematical blending.
     * @param root_scaling    Toggle for non-linear square-root distribution scaling.
     */
    static inline Color Compute(unsigned int iter, unsigned int active_max_iter, float r2, 
                                const std::vector<GradientStop>& stops, 
                                bool smooth_shading, bool root_scaling) noexcept 
    {
        if (iter >= active_max_iter) return BLACK;

        float final_t = 0.0f;

        if (smooth_shading && iter > 2) {
            // Log-log smooth interpolation
            float log_zn = std::log(r2) * 0.5f;
            float nu = std::log(log_zn / 0.69314718056f) / 0.69314718056f;
            float smooth_iter = static_cast<float>(iter) + 1.0f - nu;
            
            if (smooth_iter < 0.0f) smooth_iter = 0.0f;
            final_t = smooth_iter / static_cast<float>(active_max_iter);
        } else {
            // Standard uniform banded fallback
            final_t = static_cast<float>(iter) / static_cast<float>(active_max_iter);
        }

        if (root_scaling) {
            final_t = std::sqrt(final_t);
        }

        return SampleGradient(stops, final_t);
    }

    /**
    * @brief Computes the exact pixel color on the fly using a unified Gradient profile configuration.
    * @param iter            The raw integer iteration score calculated by the escape test loop.
    * @param active_max_iter The current dynamic maximum loop cutoff restriction.
    * @param r2              The magnitude squared of complex number Z (x^2 + y^2) at escape moment.
    * @param gradient        The unified coloring configuration profile containing pre-sorted stops and toggles.
    */
    static inline Color Compute(unsigned int iter, unsigned int active_max_iter, float r2, 
                                const Gradient& gradient) noexcept 
    {
        return Compute(
            iter, 
            active_max_iter, 
            r2, 
            gradient.stops, 
            gradient.smooth_shading, 
            gradient.root_scaling
        );
    }

    /**
     * @brief Fast-pass discrete color accessor, ignoring the smooth shading logarithmic overhead.
     */
    static inline Color EvaluateDiscrete(unsigned int iter, unsigned int active_max_iter, 
                                         const std::vector<GradientStop>& stops, 
                                         bool root_scaling) noexcept 
    {
        if (iter >= active_max_iter) return BLACK;
        
        float t = static_cast<float>(iter) / static_cast<float>(active_max_iter);
        if (root_scaling) t = std::sqrt(t);
        
        return SampleGradient(stops, t);
    }

    /**
     * @brief Allows float iterations for manual custom external smooth handlers.
     */
    static inline Color EvaluateContinuous(float iter, unsigned int active_max_iter, 
                                           const std::vector<GradientStop>& stops, 
                                           bool root_scaling) noexcept 
    {
        if (iter >= static_cast<float>(active_max_iter)) return BLACK;

        float t = iter / static_cast<float>(active_max_iter);
        if (root_scaling) t = std::sqrt(t);
        
        return SampleGradient(stops, t);
    }
};

} // namespace mandelbrot_engine