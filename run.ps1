# Stop script on any failing command
$ErrorActionPreference = "Stop"

# Configurable variables
$AppName = "MandelbrotApp.exe"
$CondaDir = "$HOME\Miniconda3"
$EnvName = "mandelbrot_env"
$BuildDir = "build"

# Force auto-accept for Anaconda's ToS and Conda licenses
$env:CONDA_ACCEPT_LICENSE = "yes"
$env:CONDA_ALWAYS_YES = "true"
$env:CONDA_PLUGINS_AUTO_ACCEPT_TOS = "true"
$env:CI = "true"

Write-Host "=================================================="
Write-Host "     Mandelbrot Launcher & Build Script (Windows)"
Write-Host "=================================================="

# 1. Ensure Miniconda is installed locally (User-space, no admin required)
if (-not (Test-Path $CondaDir)) {
    Write-Host "[+] Miniconda not found. Downloading and installing to $CondaDir..."
    $InstallerPath = "$env:TEMP\miniconda.exe"
    
    Invoke-WebRequest -Uri "https://repo.anaconda.com/miniconda/Miniconda3-latest-Windows-x86_64.exe" -OutFile $InstallerPath
    
    # Run silent installation
    Start-Process -FilePath $InstallerPath -ArgumentList "/S", "/InstallationType=JustMe", "/RegisterPython=0", "/D=$CondaDir" -Wait
    Remove-Item $InstallerPath -Force
}

# 2. Hook Conda directly into the active PowerShell session
$CondaHook = Join-Path $CondaDir "shell\condabin\conda-hook.ps1"
if (Test-Path $CondaHook) {
    . $CondaHook
} else {
    Write-Error "Failed to locate Conda hook script at $CondaHook"
}

# Configure Conda to accept TOS and skip prompts globally
conda config --set always_yes true | Out-Null
conda config --set plugins.auto_accept_tos true | Out-Null

# 3. Check if the Conda environment exists; if not, create it
$EnvList = conda env list
if ($EnvList -notmatch "^$EnvName\s") {
    Write-Host "[+] Environment '$EnvName' not found. Creating environment and installing packages..."
    
    # We install CMake, Ninja, Clang Toolchain, Boost, AND FFmpeg from conda-forge
    conda create -n $EnvName --override-channels -c conda-forge --yes `
        cmake `
        ninja `
        clangxx_win-64 `
        boost-cpp `
        ffmpeg
}

# 4. Build the C++ project using 'conda run' with Ninja
Write-Host "[+] Building project..."
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Run CMake configuration and building directly in the Conda environment
conda run -n $EnvName cmake -G "Ninja" -B $BuildDir -DCMAKE_BUILD_TYPE=Release
conda run -n $EnvName cmake --build $BuildDir

# 5. Execute the binary
$ExecutablePath = Join-Path $BuildDir $AppName
if (Test-Path $ExecutablePath) {
    Write-Host "=================================================="
    Write-Host " Build successful! Executing $AppName..."
    Write-Host "=================================================="
    & ".\$ExecutablePath"
} else {
    Write-Host "Error: Build completed, but '$ExecutablePath' was not found."
    Write-Host "Check executable target name inside CMakeLists.txt."
    exit 1
}