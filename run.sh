#!/usr/bin/env bash

# Exit immediately if any command fails
set -e

# Configurable variables
APP_NAME="MandelbrotApp"
CONDA_DIR="$HOME/miniconda3"
ENV_NAME="mandelbrot_env"
BUILD_DIR="build"

# Force auto-accept for Anaconda's ToS plugin across non-interactive scripts
export CONDA_ACCEPT_LICENSE="yes"
export CONDA_ALWAYS_YES="true"
export CONDA_PLUGINS_AUTO_ACCEPT_TOS="true"
export CI="true"

echo "=================================================="
echo "          Mandelbrot Launcher & Build Script"
echo "=================================================="

# 1. Ensure Conda is installed locally (User-space, no sudo needed)
if [ ! -d "$CONDA_DIR" ]; then
    echo "[+] Miniconda not found. Installing to $CONDA_DIR..."
    wget -q https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O /tmp/miniconda.sh
    bash /tmp/miniconda.sh -b -p "$CONDA_DIR"
    rm /tmp/miniconda.sh
fi

# 2. Source Conda environment into script context
source "$CONDA_DIR/etc/profile.d/conda.sh"

# Configure Conda to accept TOS and skip prompts globally
conda config --set always_yes true >/dev/null 2>&1 || true
conda config --set plugins.auto_accept_tos true >/dev/null 2>&1 || true

# Explicitly issue the TOS acceptance command if the plugin exists
if conda tos --help >/dev/null 2>&1; then
    conda tos accept >/dev/null 2>&1 || true
fi

# 3. Check if the Conda environment exists; if not, create it
if ! conda env list | grep -q "^$ENV_NAME "; then
    echo "[+] Environment '$ENV_NAME' not found. Creating environment..."
    
    # Use --override-channels with -c conda-forge to bypass Anaconda 'defaults' channel TOS entirely
    conda create -n "$ENV_NAME" --override-channels -c conda-forge --yes \
        cmake \
        gxx_linux-64 \
        make \
        boost-cpp \
        libgl-devel \
        mesalib \
        xorg-libxrandr \
        xorg-libxinerama \
        xorg-libxcursor \
        xorg-libxi \
        xorg-libxext \
        xorg-libx11 \
        libglu
fi

# 4. Build the C++ project using 'conda run'
echo "[+] Building project..."
mkdir -p "$BUILD_DIR"

conda run -n "$ENV_NAME" cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
conda run -n "$ENV_NAME" cmake --build "$BUILD_DIR" -j$(nproc)

# 5. Execute the binary
if [ -f "$BUILD_DIR/$APP_NAME" ]; then
    echo "=================================================="
    echo " Build successful! Executing $APP_NAME..."
    echo "=================================================="
    "./$BUILD_DIR/$APP_NAME"
else
    echo "Error: Build completed, but '$BUILD_DIR/$APP_NAME' was not found."
    echo "Check executable target name inside CMakeLists.txt."
    exit 1
fi