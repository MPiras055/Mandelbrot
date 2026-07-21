#!/usr/bin/env bash

# Exit immediately if any command fails
set -e

# Configurable variables
APP_NAME="MandelbrotApp"
CONDA_DIR="$HOME/miniconda3"
ENV_NAME="mandelbrot_env"
BUILD_DIR="build"

echo "=================================================="
echo "          Mandelbrot Launcher & Build Script"
echo "=================================================="

# 1. Ensure Conda is installed locally
if [ ! -d "$CONDA_DIR" ]; then
    echo "[+] Miniconda not found. Installing to $CONDA_DIR..."
    wget -q https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O /tmp/miniconda.sh
    bash /tmp/miniconda.sh -b -p "$CONDA_DIR"
    rm /tmp/miniconda.sh
fi

# 2. Source Conda environment
source "$CONDA_DIR/etc/profile.d/conda.sh"

# 3. Check if the Conda environment exists; if not, create it
if ! conda env list | grep -q "^$ENV_NAME "; then
    echo "[+] Environment '$ENV_NAME' not found. Creating and installing packages..."
    conda create -n "$ENV_NAME" -c conda-forge -y \
        cmake \
        gxx_linux-64 \
        libquadmath \
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

# 4. Build the C++ project using 'conda run' (Bypasses activation issues)
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
    exit 1
fi