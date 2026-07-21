#!/usr/bin/env bash

# Exit immediately if any command fails
set -e

# Configurable variables
APP_NAME="MandelbrotApp"          # Name of your compiled executable
CONDA_DIR="$HOME/miniconda3"
ENV_NAME="mandelbrot_env"
BUILD_DIR="build"

echo "=================================================="
echo "          Mandelbrot Launcher & Build Script"
echo "=================================================="

# 1. Fast-path check: If application is already built, run it immediately!
if [ -f "$BUILD_DIR/$APP_NAME" ]; then
    echo "Executable '$APP_NAME' found! Running application..."
    echo "=================================================="
    "./$BUILD_DIR/$APP_NAME"
    exit 0
fi

# 2. Ensure Conda is installed locally
if [ ! -d "$CONDA_DIR" ]; then
    echo "[+] Miniconda not found. Installing to $CONDA_DIR..."
    wget -q https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O /tmp/miniconda.sh
    bash /tmp/miniconda.sh -b -p "$CONDA_DIR"
    rm /tmp/miniconda.sh
fi

# 3. Source Conda directly into script
source "$CONDA_DIR/etc/profile.d/conda.sh"

# Add conda initialization to bashrc if missing
if ! grep -q "miniconda3" "$HOME/.bashrc" 2>/dev/null; then
    "$CONDA_DIR/bin/conda" init bash >/dev/null 2>&1 || true
fi

# 4. Check if the Conda environment exists; if not, create it with all dependencies
if ! conda env list | grep -q "^$ENV_NAME "; then
    echo "[+] Environment '$ENV_NAME' not found. Creating and installing packages..."
    conda create -n "$ENV_NAME" -c conda-forge -y \
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

# 5. Activate the Conda environment
conda activate "$ENV_NAME"

# 6. Build the C++ project
echo "[+] Building project..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake ..
make -j$(nproc)

cd ..

# 7. Execute the binary
if [ -f "$BUILD_DIR/$APP_NAME" ]; then
    echo "=================================================="
    echo " Build successful! Executing $APP_NAME..."
    echo "=================================================="
    "./$BUILD_DIR/$APP_NAME"
else
    echo "Error: Build completed, but '$BUILD_DIR/$APP_NAME' was not found."
    echo "Check the executable name configured in CMakeLists.txt and update APP_NAME in run.sh."
    exit 1
fi
