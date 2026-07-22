#!/bin/bash
set -e

APP_NAME="MandelbrotApp"
BUILD_DIR="build"
EXECUTABLE="$BUILD_DIR/$APP_NAME"

DEV_TOOLS_DIR="$HOME/DevTools"
mkdir -p "$DEV_TOOLS_DIR"

# Detect OS and Architecture
OS="$(uname -s)"
ARCH="$(uname -m)"

if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "amd64" ]; then
    LINUX_ARCH="x86_64"
    FFMPEG_ARCH="amd64"
else
    LINUX_ARCH="aarch64"
    FFMPEG_ARCH="arm64"
fi

# =====================================================================
# 1. SETUP LINUX HEADERS (Linux Only)
# =====================================================================
if [ "$OS" = "Linux" ]; then
    MAMBA_BIN="$DEV_TOOLS_DIR/bin/micromamba"
    DEV_ENV_DIR="$DEV_TOOLS_DIR/linux_dev_env"
    export MAMBA_ROOT_PREFIX="$DEV_TOOLS_DIR/mamba_root"

    if [ ! -f "$MAMBA_BIN" ]; then
        echo "Downloading portable package manager for Linux system headers..."
        mkdir -p "$DEV_TOOLS_DIR/bin"
        curl -Ls https://micro.mamba.pm/api/micromamba/linux-64/latest | tar -xvj -C "$DEV_TOOLS_DIR" bin/micromamba
    fi

    if [ ! -d "$DEV_ENV_DIR" ]; then
        echo "Creating isolated local environment with X11/OpenGL headers..."
        # Added xorg-xproto here (contains the core X11/X.h header)
        "$MAMBA_BIN" create -p "$DEV_ENV_DIR" -y -c conda-forge \
            pkg-config xorg-libx11 xorg-libxcursor xorg-libxrandr \
            xorg-libxinerama xorg-libxi xorg-libxext libgl-devel alsa-lib xorg-xproto
    fi

    echo "Activating Linux development headers environment..."
    eval "$("$MAMBA_BIN" shell hook -s bash)"
    micromamba activate "$DEV_ENV_DIR"

    # CRITICAL FIX: Force CMake and the compiler to look inside the Micromamba folder
    export PKG_CONFIG_PATH="$DEV_ENV_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"
    export CMAKE_PREFIX_PATH="$DEV_ENV_DIR:$CMAKE_PREFIX_PATH"
    export C_INCLUDE_PATH="$DEV_ENV_DIR/include:$C_INCLUDE_PATH"
    export CPLUS_INCLUDE_PATH="$DEV_ENV_DIR/include:$CPLUS_INCLUDE_PATH"
    export LIBRARY_PATH="$DEV_ENV_DIR/lib:$LIBRARY_PATH"
    export LD_LIBRARY_PATH="$DEV_ENV_DIR/lib:$LD_LIBRARY_PATH"
fi

# =====================================================================
# 2. CHECK & INSTALL CMAKE
# =====================================================================
if command -v cmake >/dev/null 2>&1; then
    echo "System CMake detected: $(command -v cmake)"
else
    if [ "$OS" = "Darwin" ]; then
        CMAKE_BIN="$DEV_TOOLS_DIR/cmake/CMake.app/Contents/bin"
    else
        CMAKE_BIN="$DEV_TOOLS_DIR/cmake/bin"
    fi

    if [ ! -f "$CMAKE_BIN/cmake" ]; then
        echo "CMake not found globally. Downloading portable $OS version..."
        if [ "$OS" = "Darwin" ]; then
            curl -L "https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-macos-universal.tar.gz" -o "$DEV_TOOLS_DIR/cmake.tar.gz"
            tar -xzf "$DEV_TOOLS_DIR/cmake.tar.gz" -C "$DEV_TOOLS_DIR"
            mv "$DEV_TOOLS_DIR"/cmake-*-macos-universal "$DEV_TOOLS_DIR/cmake"
        else
            curl -L "https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-linux-${LINUX_ARCH}.tar.gz" -o "$DEV_TOOLS_DIR/cmake.tar.gz"
            tar -xzf "$DEV_TOOLS_DIR/cmake.tar.gz" -C "$DEV_TOOLS_DIR"
            mv "$DEV_TOOLS_DIR"/cmake-*-linux-${LINUX_ARCH} "$DEV_TOOLS_DIR/cmake"
        fi
        rm "$DEV_TOOLS_DIR/cmake.tar.gz"
    fi
    export PATH="$CMAKE_BIN:$PATH"
fi

# =====================================================================
# 3. CHECK & INSTALL FFMPEG
# =====================================================================
if command -v ffmpeg >/dev/null 2>&1; then
    echo "System FFmpeg detected: $(command -v ffmpeg)"
else
    FFMPEG_BIN="$DEV_TOOLS_DIR/ffmpeg"
    if [ ! -f "$FFMPEG_BIN/ffmpeg" ]; then
        echo "FFmpeg not found globally. Downloading portable $OS build..."
        if [ "$OS" = "Darwin" ]; then
            mkdir -p "$FFMPEG_BIN"
            curl -L "https://evermeet.cx/ffmpeg/getrelease/zip" -o "$DEV_TOOLS_DIR/ffmpeg.zip"
            unzip -q "$DEV_TOOLS_DIR/ffmpeg.zip" -d "$FFMPEG_BIN"
            rm "$DEV_TOOLS_DIR/ffmpeg.zip"
        else
            curl -L "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-${FFMPEG_ARCH}-static.tar.xz" -o "$DEV_TOOLS_DIR/ffmpeg.tar.xz"
            tar -xf "$DEV_TOOLS_DIR/ffmpeg.tar.xz" -C "$DEV_TOOLS_DIR"
            rm "$DEV_TOOLS_DIR/ffmpeg.tar.xz"
            mv "$DEV_TOOLS_DIR"/ffmpeg-*-static "$DEV_TOOLS_DIR/ffmpeg"
        fi
    fi
    export PATH="$FFMPEG_BIN:$PATH"
fi

# =====================================================================
# 4. BUILD & EXECUTE
# =====================================================================
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