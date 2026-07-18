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

    // Evaluates 3x3 subchunks and shrinks the matrix toward the most stable deep region
    void shrinkAndCenter(double shrink_factor = 0.25) {
        unsigned int max_subchunk_min = 0;
        
        // Default to the center subchunk (index 1, 1 in a 3x3 grid of subchunks)
        int best_SC_r = 1; 
        int best_SC_c = 1;

        // 1. Evaluate all 9 subchunks (each is 3x3)
        for (int SC_r = 0; SC_r < 3; ++SC_r) {
            for (int SC_c = 0; SC_c < 3; ++SC_c) {
                
                unsigned int current_subchunk_min = std::numeric_limits<unsigned int>::max();
                
                // Find the LOWEST iteration count within this specific 3x3 subchunk
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        int r = SC_r * 3 + i;
                        int c = SC_c * 3 + j;
                        int idx = r * SQUARE + c;
                        
                        if (rebaseMatrix[idx] < current_subchunk_min) {
                            current_subchunk_min = rebaseMatrix[idx];
                        }
                    }
                }

                // 2. Select the subchunk with the HIGHEST minimum iteration count
                if (current_subchunk_min > max_subchunk_min) {
                    max_subchunk_min = current_subchunk_min;
                    best_SC_r = SC_r;
                    best_SC_c = SC_c;
                } 
                // Tie-breaker: if subchunks tie, prefer the one closest to the center (1, 1)
                else if (current_subchunk_min == max_subchunk_min && current_subchunk_min > 0) {
                    int current_dist = (SC_r - 1)*(SC_r - 1) + (SC_c - 1)*(SC_c - 1);
                    int best_dist = (best_SC_r - 1)*(best_SC_r - 1) + (best_SC_c - 1)*(best_SC_c - 1);
                    
                    if (current_dist < best_dist) {
                        best_SC_r = SC_r;
                        best_SC_c = SC_c;
                    }
                }
            }
        }

        // 3. The new center is the physical center of the winning 3x3 subchunk.
        // For subchunk (SC_r, SC_c), its center cell is at offset 1 within the subchunk.
        int best_row = best_SC_r * 3 + 1;
        int best_col = best_SC_c * 3 + 1;

        double offset_x = (best_col - CENTER_OFFSET) * stepSize.real();
        double offset_y = (best_row - CENTER_OFFSET) * stepSize.imag();
        
        center.real(center.real() + core::BigFloat(offset_x));
        center.imag(center.imag() + core::BigFloat(offset_y));

        // 4. Shrink the step size for the next iteration
        stepSize = {stepSize.real() * shrink_factor, stepSize.imag() * shrink_factor};

        // 5. Update reduction state
        reductionCounter++;
    }
};