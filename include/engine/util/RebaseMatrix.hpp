#pragma once
#include "core/Numeric.hpp"
#include "core/Kernel.hpp"
#include <array>
#include <complex>
#include <limits>

template<size_t SQUARE = 9>
struct RebaseMatrix__ {
    static constexpr int DIMENSION = SQUARE;
    static constexpr int CENTER_OFFSET = SQUARE / 2;
    static constexpr int REBASE_MTX_CHUNKS = SQUARE * SQUARE; // 81

    struct OrbitData {
        unsigned int depth{0};
        double min_z_sq{std::numeric_limits<double>::max()};
    };

    std::array<OrbitData, REBASE_MTX_CHUNKS> rebaseMatrix;
    
    std::complex<core::BigFloat> center;
    std::complex<double> stepSize;

    void init(const std::complex<core::BigFloat>& start_center,
              const std::complex<double>& start_step) {
        center = start_center;
        stepSize = start_step;
    }

    // Read-only: Safe to call from multiple threads
    std::complex<core::BigFloat> getPointAt(int idx) const {
        int row = idx / SQUARE;
        int col = idx % SQUARE;
        
        double offset_x = (col - CENTER_OFFSET) * stepSize.real();
        double offset_y = (row - CENTER_OFFSET) * stepSize.imag();

        return {
            center.real() + core::BigFloat(offset_x),
            center.imag() + core::BigFloat(offset_y)
        };
    }

    // MT-SAFE COMPUTE AND WRITE
    // Safe as long as your thread pool assigns a unique 'idx' to each thread.
    // The C++ memory model guarantees distinct array elements can be mutated concurrently.
    //
    // @param aborted  polled every `core::ABORT_POLL_MASK + 1` iterations. The high-res
    //   search runs this with the FULL iteration budget, so without a cancellation check a
    //   single probe cell could grind through `max_iterations` BigFloat steps with no way
    //   to stop — while the surrounding code assumes abort lands promptly. Bailing early
    //   still records the partial depth; the cell simply ranks lower.
    template <typename AbortFn>
    void computeAndStoreAt(int idx, unsigned int max_iterations, AbortFn&& aborted) {
        const std::complex<core::BigFloat> c = getPointAt(idx);
        core::BigFloat z_r = 0.0, z_i = 0.0;
        const core::BigFloat c_r = c.real(), c_i = c.imag();

        unsigned int depth = 0;
        double min_z_sq = std::numeric_limits<double>::max();

        // Skip the first few iterations so the initial z=0 and z=c
        // don't falsely trigger the minimum distance tracker.
        const unsigned int MIN_ITER_IGNORE = 5;

        while (depth < max_iterations) {
            if ((depth & core::ABORT_POLL_MASK) == 0 && aborted()) break;

            // Fused 3-multiply step (the std::complex spelling cost 4 plus temporaries).
            core::mandelStep(z_r, z_i, c_r, c_i);

            // Convert to double for fast distance approximation.
            const double z_real = static_cast<double>(z_r);
            const double z_imag = static_cast<double>(z_i);
            const double norm_sq = z_real * z_real + z_imag * z_imag;

            if (norm_sq > 4.0) {
                break;
            }

            if (depth > MIN_ITER_IGNORE && norm_sq < min_z_sq) {
                min_z_sq = norm_sq;
            }

            depth++;
        }

        // Write directly to the array (MT-Safe for unique idx)
        rebaseMatrix[idx] = {depth, min_z_sq};
    }

    // ANTI-GLITCH HEURISTIC
    unsigned int shrinkAndCenter(double shrink_factor = 0.25, double glitch_threshold_sq = 1e-6) {
        unsigned int best_depth = 0;
        int best_idx = CENTER_OFFSET * SQUARE + CENTER_OFFSET;

        for (int idx = 0; idx < REBASE_MTX_CHUNKS; ++idx) {
            const auto& cell = rebaseMatrix[idx];
            
            bool is_glitchy = (cell.min_z_sq < glitch_threshold_sq);
            bool best_is_glitchy = (rebaseMatrix[best_idx].min_z_sq < glitch_threshold_sq);

            if (cell.depth > best_depth) {
                // Accept deeper point if it's safe, OR if we are currently stuck with a glitchy point anyway
                if (!is_glitchy || best_is_glitchy) {
                    best_depth = cell.depth;
                    best_idx = idx;
                }
            } else if (cell.depth == best_depth && cell.depth > 0) {
                // TIE-BREAKER 1: Prefer the safe orbit over the glitchy one
                if (!is_glitchy && best_is_glitchy) {
                    best_idx = idx;
                } 
                // TIE-BREAKER 2: If safety is equal, prefer the one closest to current center
                else if (is_glitchy == best_is_glitchy) {
                    const int r  = idx / SQUARE,      c  = idx % SQUARE;
                    const int br = best_idx / SQUARE, bc = best_idx % SQUARE;
                    const int dist  = (r  - CENTER_OFFSET)*(r  - CENTER_OFFSET) + (c  - CENTER_OFFSET)*(c  - CENTER_OFFSET);
                    const int bdist = (br - CENTER_OFFSET)*(br - CENTER_OFFSET) + (bc - CENTER_OFFSET)*(bc - CENTER_OFFSET);
                    if (dist < bdist) best_idx = idx;
                }
            }
        }

        // Shift center
        const int best_row = best_idx / SQUARE;
        const int best_col = best_idx % SQUARE;

        const double offset_x = (best_col - CENTER_OFFSET) * stepSize.real();
        const double offset_y = (best_row - CENTER_OFFSET) * stepSize.imag();

        center.real(center.real() + core::BigFloat(offset_x));
        center.imag(center.imag() + core::BigFloat(offset_y));

        stepSize = {stepSize.real() * shrink_factor, stepSize.imag() * shrink_factor};

        return best_depth;
    }
};