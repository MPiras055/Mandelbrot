@echo off
setlocal EnableDelayedExpansion

:: Configurable variables
set APP_NAME=MandelbrotApp.exe
set CONDA_DIR=%USERPROFILE%\miniconda3
set ENV_NAME=mandelbrot_env
set BUILD_DIR=build
set EXECUTABLE_PATH=%BUILD_DIR%\%APP_NAME%

echo ==================================================
echo     Mandelbrot Launcher ^& Build Script (Windows)
echo ==================================================

:: 1. Ensure Conda is installed locally (User-space, no admin needed)
if not exist "%CONDA_DIR%\Scripts\conda.exe" (
    echo [+] Miniconda not found. Downloading to %CONDA_DIR%...
    curl -fL "https://repo.anaconda.com/miniconda/Miniconda3-latest-Windows-x86_64.exe" -o "%TEMP%\miniconda.exe"
    :: Install silently
    start /wait "" "%TEMP%\miniconda.exe" /InstallationType=JustMe /RegisterPython=0 /S /D=%CONDA_DIR%
    del "%TEMP%\miniconda.exe"
)

:: 2. Activate base Conda
call "%CONDA_DIR%\Scripts\activate.bat" base

:: Configure Conda to skip prompts
call conda config --set always_yes true >nul 2>&1
call conda config --set plugins.auto_accept_tos true >nul 2>&1

:: 3. Check if the Conda environment exists; if not, create it
call conda env list | findstr /B /C:"%ENV_NAME% " >nul
if errorlevel 1 (
    echo [+] Environment '%ENV_NAME%' not found. Creating environment...
    
    :: Includes ffmpeg, pkg-config, and ninja (for flat builds)
    call conda create -n "%ENV_NAME%" --override-channels -c conda-forge --yes ^
        cmake ^
        ninja ^
        boost-cpp ^
        ffmpeg ^
        pkg-config ^
        cxx-compiler
)

:: Activate the target environment
call conda activate "%ENV_NAME%"

:: 4. Build the C++ project (Only if executable does not exist)
if not exist "%EXECUTABLE_PATH%" (
    echo [+] Executable not found. Building project...
    if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

    :: We pass C++26 strictly.
    :: We pass _SILENCE_ALL flags because MSVC in C++26 mode will violently reject 
    :: Boost headers that rely on deprecated/removed standard library features.
    call cmake -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_CXX_STANDARD=26 ^
        -DCMAKE_CXX_FLAGS="/D_SILENCE_ALL_CXX23_DEPRECATION_WARNINGS /D_SILENCE_ALL_CXX26_DEPRECATION_WARNINGS /DBOOST_ALLOW_DEPRECATED_HEADERS"
    
    call cmake --build "%BUILD_DIR%" --parallel
) else (
    echo [+] Executable '%EXECUTABLE_PATH%' already exists. Skipping build phase.
)

:: 5. Execute the binary
if exist "%EXECUTABLE_PATH%" (
    echo ==================================================
    echo  Executing %APP_NAME%...
    echo ==================================================
    "%EXECUTABLE_PATH%"
) else (
    echo Error: Build phase was run, but '%EXECUTABLE_PATH%' was not found.
    echo Check executable target name inside CMakeLists.txt.
    exit /b 1
)