#!/bin/bash
set -e

APP_NAME="MandelbrotApp"
BUILD_DIR="build"
EXECUTABLE="$BUILD_DIR/$APP_NAME"

DEV_TOOLS_DIR="$HOME/DevTools"
mkdir -p "$DEV_TOOLS_DIR"

if command -v cmake >/dev/null 2>&1; then
    echo "System CMake detected: $(command -v cmake)"
else
    CMAKE_BIN="$DEV_TOOLS_DIR/cmake/bin"
    if [ ! -f "$CMAKE_BIN/cmake" ]; then
        echo "CMake not found globally. Downloading portable Linux version..."
        curl -L "https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-linux-x86_64.tar.gz" -o "$DEV_TOOLS_DIR/cmake.tar.gz"
        
        echo "Extracting CMake..."
        tar -xzf "$DEV_TOOLS_DIR/cmake.tar.gz" -C "$DEV_TOOLS_DIR"
        rm "$DEV_TOOLS_DIR/cmake.tar.gz"
        mv "$DEV_TOOLS_DIR"/cmake-*-linux-x86_64 "$DEV_TOOLS_DIR/cmake"
    fi
    export PATH="$CMAKE_BIN:$PATH"
fi

if command -v ffmpeg >/dev/null 2>&1; then
    echo "System FFmpeg detected: $(command -v ffmpeg)"
else
    FFMPEG_BIN="$DEV_TOOLS_DIR/ffmpeg"
    if [ ! -f "$FFMPEG_BIN/ffmpeg" ]; then
        echo "FFmpeg not found globally. Downloading portable Linux build..."
        curl -L "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz" -o "$DEV_TOOLS_DIR/ffmpeg.tar.xz"
        
        echo "Extracting FFmpeg..."
        tar -xf "$DEV_TOOLS_DIR/ffmpeg.tar.xz" -C "$DEV_TOOLS_DIR"
        rm "$DEV_TOOLS_DIR/ffmpeg.tar.xz"
        mv "$DEV_TOOLS_DIR"/ffmpeg-*-static "$DEV_TOOLS_DIR/ffmpeg"
    fi
    export PATH="$FFMPEG_BIN:$PATH"
fi

if [ ! -d "$BUILD_DIR" ] || [ ! -f "$EXECUTABLE" ]; then
    echo "Build not found (or missing executable). Setting up build..."
    
    mkdir -p "$BUILD_DIR"
    cmake -B "$BUILD_DIR"
    cmake --build "$BUILD_DIR" --config Release -j
else
    echo "Project is already built. Skipping compilation."
fi

echo "Launching application..."
"./$EXECUTABLE"