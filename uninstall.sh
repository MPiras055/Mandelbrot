#!/bin/bash
set -e

# Define the paths used in the run script
DEV_TOOLS_DIR="$HOME/DevTools"
CMAKE_DIR="$DEV_TOOLS_DIR/cmake"
FFMPEG_DIR="$DEV_TOOLS_DIR/ffmpeg"
BUILD_DIR="build"

echo "Starting cleanup process..."

if [ -d "$CMAKE_DIR" ]; then
    echo "Removing CMake from $CMAKE_DIR..."
    rm -rf "$CMAKE_DIR"
else
    echo "- CMake installation not found. Skipping."
fi

# =====================================================================
# 2. REMOVE FFMPEG
# =====================================================================
if [ -d "$FFMPEG_DIR" ]; then
    echo "Removing FFmpeg from $FFMPEG_DIR..."
    rm -rf "$FFMPEG_DIR"
else
    echo "- FFmpeg installation not found. Skipping."
fi

# =====================================================================
# 3. REMOVE BUILD DIRECTORY
# =====================================================================
if [ -d "$BUILD_DIR" ]; then
    echo "Removing project build directory ($BUILD_DIR)..."
    rm -rf "$BUILD_DIR"
else
    echo "- Build directory not found. Skipping."
fi

# =====================================================================
# 4. CLEAN UP DEV_TOOLS DIRECTORY (If Empty)
# =====================================================================
if [ -d "$DEV_TOOLS_DIR" ]; then
    # Check if the directory is empty
    if [ -z "$(ls -A "$DEV_TOOLS_DIR")" ]; then
        echo "Removing empty DevTools directory..."
        rm -rf "$DEV_TOOLS_DIR"
    else
        echo "- DevTools directory is not empty (may contain GCC or other tools). Leaving it intact."
    fi
fi

echo "Cleanup complete!"