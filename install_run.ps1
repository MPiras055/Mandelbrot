$ErrorActionPreference = "Stop"

# =====================================================================
# 1. CONFIGURATION
# =====================================================================
$AppName = "MandelbrotApp"
# Determine the directory where this script is located
$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

$BuildDir = "$ScriptDir\build"
$DevToolsDir = "$ScriptDir\DevTools"
$CMakeBin = "$DevToolsDir\cmake\bin"
$FFmpegBin = "$DevToolsDir\ffmpeg\bin"
$GccBin = "$DevToolsDir\mingw64\bin"

if (-not (Test-Path $DevToolsDir)) { New-Item -ItemType Directory -Force -Path $DevToolsDir | Out-Null }

# =====================================================================
# 2. CHECK & INSTALL CMAKE
# =====================================================================
$SystemCMake = Get-Command "cmake" -ErrorAction SilentlyContinue
if ($SystemCMake) {
    Write-Host "System CMake detected..." -ForegroundColor Green
} else {
    if (-not (Test-Path "$CMakeBin\cmake.exe")) {
        Write-Host "Downloading portable CMake to project folder..." -ForegroundColor Cyan
        $CMakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-windows-x86_64.zip"
        $ZipPath = "$DevToolsDir\cmake.zip"
        
        Invoke-WebRequest -Uri $CMakeUrl -OutFile $ZipPath -UseBasicParsing
        Write-Host "Extracting CMake (using tar)..." -ForegroundColor Yellow
        tar -xf $ZipPath -C $DevToolsDir
        Remove-Item $ZipPath
        
        $Extracted = Get-ChildItem -Path $DevToolsDir -Filter "cmake-*-windows-x86_64" | Select-Object -First 1
        Rename-Item -Path $Extracted.FullName -NewName "cmake"
    }
    $env:PATH = "$CMakeBin;" + $env:PATH
}

# =====================================================================
# 3. CHECK & INSTALL FFMPEG
# =====================================================================
$SystemFFmpeg = Get-Command "ffmpeg" -ErrorAction SilentlyContinue
if ($SystemFFmpeg) {
    Write-Host "System FFmpeg detected..." -ForegroundColor Green
} else {
    if (-not (Test-Path "$FFmpegBin\ffmpeg.exe")) {
        Write-Host "Downloading portable FFmpeg to project folder..." -ForegroundColor Cyan
        $FFmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
        $ZipPath = "$DevToolsDir\ffmpeg.zip"
        
        Invoke-WebRequest -Uri $FFmpegUrl -OutFile $ZipPath -UseBasicParsing
        Write-Host "Extracting FFmpeg (using tar)..." -ForegroundColor Yellow
        tar -xf $ZipPath -C $DevToolsDir
        Remove-Item $ZipPath
        
        $Extracted = Get-ChildItem -Path $DevToolsDir -Filter "ffmpeg-master-latest-win64-gpl" | Select-Object -First 1
        Rename-Item -Path $Extracted.FullName -NewName "ffmpeg"
    }
    $env:PATH = "$FFmpegBin;" + $env:PATH
}

# =====================================================================
# 4. DOWNLOAD PORTABLE C++ COMPILER (WinLibs MinGW)
# =====================================================================
$SystemGCC = Get-Command "gcc" -ErrorAction SilentlyContinue
if ($SystemGCC) {
    Write-Host "System GCC detected..." -ForegroundColor Green
} else {
    if (-not (Test-Path "$GccBin\gcc.exe")) {
        Write-Host "Downloading portable GCC (WinLibs) to project folder..." -ForegroundColor Cyan
        $GccUrl = "https://github.com/brechtsanders/winlibs_mingw/releases/download/13.1.0-15.0.7-11.0.0-msvcrt-r5/winlibs-x86_64-posix-seh-gcc-13.1.0-mingw-w64msvcrt-11.0.0-r5.zip"
        $ZipPath = "$DevToolsDir\winlibs.zip"
        
        Invoke-WebRequest -Uri $GccUrl -OutFile $ZipPath -UseBasicParsing
        Write-Host "Extracting GCC toolchain (using tar)..." -ForegroundColor Yellow
        tar -xf $ZipPath -C $DevToolsDir
        Remove-Item $ZipPath
    }
    $env:PATH = "$GccBin;" + $env:PATH
}

# =====================================================================
# 5. BUILD IF NEEDED
# =====================================================================
$ExeFile = "$BuildDir\$AppName"
$ExeFileWindows = "$BuildDir\$AppName.exe"

if ((-not (Test-Path $BuildDir)) -or ((-not (Test-Path $ExeFile)) -and (-not (Test-Path $ExeFileWindows)))) {
    Write-Host "Build not found. Setting up build..." -ForegroundColor Cyan
    
    # Nuke the old build folder to prevent cache corruption
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    
    # Force CMake to use our portable GCC
    cmake -B $BuildDir -G "MinGW Makefiles"
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    # Compile the project
    cmake --build $BuildDir --config Release -j
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }
} else {
    Write-Host "Project is already built. Skipping compilation." -ForegroundColor Green
}

# =====================================================================
# 6. EXECUTE
# =====================================================================
Write-Host "Launching application..." -ForegroundColor Magenta
if (Test-Path $ExeFileWindows) {
    & ".\$ExeFileWindows"
} elseif (Test-Path $ExeFile) {
    & ".\$ExeFile"
}