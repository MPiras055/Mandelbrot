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
    SYS_ARCH="x86_64"
    CONDA_ARCH="64"
    FFMPEG_ARCH="amd64"
else
    SYS_ARCH="aarch64"
    CONDA_ARCH="aarch64"
    FFMPEG_ARCH="arm64"
fi

# =====================================================================
# 1. SETUP COMPILER & SYSTEM HEADERS (Linux Only)
# =====================================================================
if [ "$OS" = "Linux" ]; then
    MAMBA_BIN="$DEV_TOOLS_DIR/bin/micromamba"
    DEV_ENV_DIR="$DEV_TOOLS_DIR/linux_dev_env"
    export MAMBA_ROOT_PREFIX="$DEV_TOOLS_DIR/mamba_root"

    if [ ! -f "$MAMBA_BIN" ]; then
        echo "Downloading portable package manager for Linux headers & compiler..."
        mkdir -p "$DEV_TOOLS_DIR/bin"
        curl -Ls https://micro.mamba.pm/api/micromamba/linux-64/latest | tar -xvj -C "$DEV_TOOLS_DIR" bin/micromamba
    fi

    echo "Ensuring cutting-edge GCC 14+ (C++26) is installed..."
    if [ ! -d "$DEV_ENV_DIR" ]; then
        "$MAMBA_BIN" create -p "$DEV_ENV_DIR" -y -c conda-forge \
            "gcc_linux-${CONDA_ARCH}>=14" "gxx_linux-${CONDA_ARCH}>=14" \
            pkg-config xorg-libx11 xorg-libxcursor xorg-libxrandr \
            xorg-libxinerama xorg-libxi xorg-libxext libgl-devel alsa-lib xorg-xorgproto
    else
        # Force install/update if the environment already exists so we don't get stuck on GCC 12
        "$MAMBA_BIN" install -p "$DEV_ENV_DIR" -y -c conda-forge \
            "gcc_linux-${CONDA_ARCH}>=14" "gxx_linux-${CONDA_ARCH}>=14"
    fi

    echo "Activating Linux development environment..."
    eval "$("$MAMBA_BIN" shell hook -s bash)"
    micromamba activate "$DEV_ENV_DIR"

    # Print the compiler version to guarantee it is GCC 14+
    echo "Using C++ Compiler: $CXX"
    $CXX --version | head -n 1
    
    export PKG_CONFIG_PATH="$DEV_ENV_DIR/lib/pkgconfig:$PKG_CONFIG_PATH"
    export CMAKE_PREFIX_PATH="$DEV_ENV_DIR:$CMAKE_PREFIX_PATH"
    export C_INCLUDE_PATH="$DEV_ENV_DIR/include:$C_INCLUDE_PATH"
    export CPLUS_INCLUDE_PATH="$DEV_ENV_DIR/include:$CPLUS_INCLUDE_PATH"
    export LIBRARY_PATH="$DEV_ENV_DIR/lib:$LIBRARY_PATH"
    export LD_LIBRARY_PATH="$DEV_ENV_DIR/lib:$LD_LIBRARY_PATH"

    # FIX: Nullify the broken _X_NONSTRING macro in older X11 headers when using modern GCC
    export CFLAGS="-D_X_NONSTRING= $CFLAGS"
    export CXXFLAGS="-D_X_NONSTRING= $CXXFLAGS"
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
            curl -L "https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-linux-${SYS_ARCH}.tar.gz" -o "$DEV_TOOLS_DIR/cmake.tar.gz"
            tar -xzf "$DEV_TOOLS_DIR/cmake.tar.gz" -C "$DEV_TOOLS_DIR"
            mv "$DEV_TOOLS_DIR"/cmake-*-linux-${SYS_ARCH} "$DEV_TOOLS_DIR/cmake"
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
    echo "Setting up build..."
    
    # Nuke the old build folder to prevent cache corruption
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    
    # Configure with explicit compiler paths passed to CMake
    # We use ${CC} and ${CXX} which Micromamba natively populates with the correct wrapper binaries
    cmake -B "$BUILD_DIR" \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCMAKE_C_COMPILER="${CC:-gcc}" \
          -DCMAKE_CXX_COMPILER="${CXX:-g++}"
    
    # Compile the project
    cmake --build "$BUILD_DIR" --config Release -j
else
    echo "Project is already built. Skipping compilation."
fi

echo "Launching application..."
"./$EXECUTABLE"