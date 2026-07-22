$ErrorActionPreference = "Stop"

# =====================================================================
# 1. CONFIGURATION
# =====================================================================
$AppName = "MandelbrotApp"
$BuildDir = "build"
$DevToolsDir = "$env:USERPROFILE\Desktop\DevTools"
$CMakeBin = "$DevToolsDir\cmake\bin"
$FFmpegBin = "$DevToolsDir\ffmpeg\bin"

if (-not (Test-Path $DevToolsDir)) { New-Item -ItemType Directory -Force -Path $DevToolsDir | Out-Null }

# =====================================================================
# 2. CHECK & INSTALL CMAKE
# =====================================================================
$SystemCMake = Get-Command "cmake" -ErrorAction SilentlyContinue
if ($SystemCMake) {
    Write-Host "System CMake detected: $($SystemCMake.Source)" -ForegroundColor Green
} else {
    if (-not (Test-Path "$CMakeBin\cmake.exe")) {
        Write-Host "CMake not found globally. Downloading portable Windows version..." -ForegroundColor Cyan
        $CMakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.30.2/cmake-3.30.2-windows-x86_64.zip"
        $ZipPath = "$DevToolsDir\cmake.zip"
        
        Invoke-WebRequest -Uri $CMakeUrl -OutFile $ZipPath -UseBasicParsing
        Write-Host "Extracting CMake..." -ForegroundColor Yellow
        Expand-Archive -Path $ZipPath -DestinationPath $DevToolsDir -Force
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
    Write-Host "System FFmpeg detected: $($SystemFFmpeg.Source)" -ForegroundColor Green
} else {
    if (-not (Test-Path "$FFmpegBin\ffmpeg.exe")) {
        Write-Host "FFmpeg not found globally. Downloading portable Windows build..." -ForegroundColor Cyan
        $FFmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
        $ZipPath = "$DevToolsDir\ffmpeg.zip"
        
        Invoke-WebRequest -Uri $FFmpegUrl -OutFile $ZipPath -UseBasicParsing
        Write-Host "Extracting FFmpeg..." -ForegroundColor Yellow
        Expand-Archive -Path $ZipPath -DestinationPath $DevToolsDir -Force
        Remove-Item $ZipPath
        
        $Extracted = Get-ChildItem -Path $DevToolsDir -Filter "ffmpeg-master-latest-win64-gpl" | Select-Object -First 1
        Rename-Item -Path $Extracted.FullName -NewName "ffmpeg"
    }
    $env:PATH = "$FFmpegBin;" + $env:PATH
}

# =====================================================================
# 4. BUILD IF NEEDED
# =====================================================================
$ExeFile = "$BuildDir\$AppName"
$ExeFileWindows = "$BuildDir\$AppName.exe"

# Check if build directory is missing, or if both variations of the executable are missing
if ((-not (Test-Path $BuildDir)) -or ((-not (Test-Path $ExeFile)) -and (-not (Test-Path $ExeFileWindows)))) {
    Write-Host "Build not found. Setting up build..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    
    # Configure CMake. (Add -G "MinGW Makefiles" here if you are using WinLibs/GCC instead of MSVC)
    cmake -B $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    # Compile the project
    cmake --build $BuildDir --config Release -j
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }
} else {
    Write-Host "Project is already built. Skipping compilation." -ForegroundColor Green
}

# =====================================================================
# 5. EXECUTE
# =====================================================================
Write-Host "Launching application..." -ForegroundColor Magenta
if (Test-Path $ExeFileWindows) {
    & ".\$ExeFileWindows"
} elseif (Test-Path $ExeFile) {
    & ".\$ExeFile"
}