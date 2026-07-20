#include <iostream>
#include <string>
#include "gui/MandelbrotGUI.hpp"

/**
 * @brief Application Entry Point.
 * @details Parses command-line arguments for window sizing configurations 
 * and initializes the structural UI/Engine loop context.
 */
int main(int argc, char* argv[]) {
    // Default fallback window resolutions
    int window_width = 1280;
    int window_height = 720;

    // --- ARGUMENT PARSING ---
    if (argc >= 3) {
        try {
            int parsed_width = std::stoi(argv[1]);
            int parsed_height = std::stoi(argv[2]);

            // Clamp dimensions to prevent users from blowing past hardware capacity limits
            if (parsed_width >= 320 && parsed_height >= 240) {
                window_width = parsed_width;
                window_height = parsed_height;
            } else {
                std::cerr << "[Warning]: Dimensions too small! Falling back to 1280x720.\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Error Parsing Arguments]: " << e.what() 
                      << "\nUsage: ./MandelbrotEngine [width] [height]\n"
                      << "Falling back to default configuration.\n";
        }
    } else {
        std::cout << "[Info]: No resolution overrides passed.\n"
                  // Instruct users how to launch with custom sizes if they want to
                  << "Usage: ./MandelbrotEngine [width] [height]\n"
                  << "Launching at default 1280x720 window context.\n";
    }

    // --- MAIN EXECUTION LIFECYCLE ---
    try {
        // 1. Instantiate the GUI frontend wrapper.
        // This implicitly configures raylib windows, and boots up the Engine's 
        // internal lock-free worker pool thread structure.
        gui::GUI app(window_width, window_height);

        // 2. Run the main processing and rendering lifecycle loop.
        // Control stays here until the user clicks the window exit close box or hits ESC.
        app.Run();
    } 
    catch (const std::exception& ex) {
        std::cerr << "[FATAL CRASH]: Unhandled exception: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}