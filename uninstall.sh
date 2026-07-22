#!/bin/bash
set -e

DEV_TOOLS_DIR="$HOME/DevTools"
BUILD_DIR="build"

echo "Starting cleanup process..."

# 1. Clean Build
if [ -d "$BUILD_DIR" ]; then
    echo "Removing project build directory ($BUILD_DIR)..."
    rm -rf "$BUILD_DIR"
fi

# 2. Clean CMake
if [ -d "$DEV_TOOLS_DIR/cmake" ]; then
    echo "Removing portable CMake..."
    rm -rf "$DEV_TOOLS_DIR/cmake"
fi

# 3. Clean FFmpeg
if [ -d "$DEV_TOOLS_DIR/ffmpeg" ]; then
    echo "Removing portable FFmpeg..."
    rm -rf "$DEV_TOOLS_DIR/ffmpeg"
fi

# 4. Clean Micromamba & Linux Environments
if [ -d "$DEV_TOOLS_DIR/bin" ] || [ -d "$DEV_TOOLS_DIR/linux_dev_env" ] || [ -d "$DEV_TOOLS_DIR/mamba_root" ]; then
    echo "Removing Linux system headers and Micromamba..."
    rm -rf "$DEV_TOOLS_DIR/bin"
    rm -rf "$DEV_TOOLS_DIR/linux_dev_env"
    rm -rf "$DEV_TOOLS_DIR/mamba_root"
fi

# 5. Clean DevTools Directory (If Empty)
if [ -d "$DEV_TOOLS_DIR" ]; then
    if [ -z "$(ls -A "$DEV_TOOLS_DIR")" ]; then
        echo "Removing empty DevTools directory..."
        rm -rf "$DEV_TOOLS_DIR"
    else
        echo "- DevTools directory is not empty. Leaving it intact."
    fi
fi

echo "Uninstall/Cleanup complete!"