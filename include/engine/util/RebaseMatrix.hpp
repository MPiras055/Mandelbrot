#pragma once
#include "core/Numeric.hpp"
#include <array>
#include <complex>
#include <limits>

template<size_t SQUARE = 9>
struct RebaseMatrix__ {
    static constexpr int DIMENSION = SQUARE;
    static constexpr int CENTER_OFFSET = 4; // Index 4 is the center of a 9x9 row/col
    static constexpr int REBASE_MTX_CHUNKS = SQUARE * SQUARE; // 81

    std::array<unsigned int, REBASE_MTX_CHUNKS> rebaseMatrix;
    
    std::complex<core::BigFloat> center;
    std::complex<double> stepSize;
    unsigned int reductionCounter{0};

    // Initializes the struct for a new search
    void init(const std::complex<core::BigFloat>& start_center, 
              const std::complex<double>& start_step) {
        center = start_center;
        stepSize = start_step;
        reductionCounter = 0;
        // Matrix is intentionally not zeroed out here or in shrinkAndCenter
    }

    // Completely resets the struct
    void reset() {
        reductionCounter = 0;
        rebaseMatrix.fill(0);
        center = std::complex<core::BigFloat>();
        stepSize = {0.0, 0.0};
    }

    // Calculates the absolute BigFloat coordinate using a single 1D index (0 to 80)
    std::complex<core::BigFloat> getPointAt(int idx) const {
        // Convert 1D index back to 2D grid coordinates
        int row = idx / SQUARE;
        int col = idx % SQUARE;
        
        // Calculate physical offset relative to the center (4, 4)
        double offset_x = (col - CENTER_OFFSET) * stepSize.real();
        double offset_y = (row - CENTER_OFFSET) * stepSize.imag();

        return {
            center.real() + core::BigFloat(offset_x),
            center.imag() + core::BigFloat(offset_y)
        };
    }

    // Sets the iteration depth at the specified 1D index (0 to 80)
    void setValueAt(int idx, unsigned int value) {
        rebaseMatrix[idx] = value;
    }

    // Recenters on the DEEPEST cell (longest orbit = best perturbation reference) and
    // shrinks the step. Max-depth beats the old highest-3x3-minimum heuristic, which
    // avoided isolated deep points (minibrot centers / filaments) — exactly the points
    // that make the best reference.
    // @return the depth of the winning cell (so the caller can early-out when a cell
    //         reaches the iteration budget).
    unsigned int shrinkAndCenter(double shrink_factor = 0.25) {
        // Default to the center cell so a fully-shallow grid stays put.
        unsigned int best_depth = 0;
        int best_idx = CENTER_OFFSET * SQUARE + CENTER_OFFSET;

        for (int idx = 0; idx < REBASE_MTX_CHUNKS; ++idx) {
            const unsigned int d = rebaseMatrix[idx];
            if (d > best_depth) {
                best_depth = d;
                best_idx = idx;
            } else if (d == best_depth && d > 0) {
                // Tie-break: prefer the cell closest to the current center.
                const int r  = idx / SQUARE,      c  = idx % SQUARE;
                const int br = best_idx / SQUARE,  bc = best_idx % SQUARE;
                const int dist  = (r  - CENTER_OFFSET)*(r  - CENTER_OFFSET) + (c  - CENTER_OFFSET)*(c  - CENTER_OFFSET);
                const int bdist = (br - CENTER_OFFSET)*(br - CENTER_OFFSET) + (bc - CENTER_OFFSET)*(bc - CENTER_OFFSET);
                if (dist < bdist) best_idx = idx;
            }
        }

        const int best_row = best_idx / SQUARE;
        const int best_col = best_idx % SQUARE;

        const double offset_x = (best_col - CENTER_OFFSET) * stepSize.real();
        const double offset_y = (best_row - CENTER_OFFSET) * stepSize.imag();

        center.real(center.real() + core::BigFloat(offset_x));
        center.imag(center.imag() + core::BigFloat(offset_y));

        stepSize = {stepSize.real() * shrink_factor, stepSize.imag() * shrink_factor};
        reductionCounter++;
        return best_depth;
    }
};