$DevToolsDir = "$env:USERPROFILE\Desktop\DevTools"
$BuildDir = "build"

Write-Host "Starting cleanup process..." -ForegroundColor Cyan

if (Test-Path $BuildDir) {
    Write-Host "Removing project build directory ($BuildDir)..."
    Remove-Item -Path $BuildDir -Recurse -Force
} else {
    Write-Host "- Build directory not found. Skipping." -ForegroundColor DarkGray
}

if (Test-Path "$DevToolsDir\cmake") {
    Write-Host "Removing portable CMake..."
    Remove-Item -Path "$DevToolsDir\cmake" -Recurse -Force
} else {
    Write-Host "- Portable CMake installation not found. Skipping." -ForegroundColor DarkGray
}

if (Test-Path "$DevToolsDir\ffmpeg") {
    Write-Host "Removing portable FFmpeg..."
    Remove-Item -Path "$DevToolsDir\ffmpeg" -Recurse -Force
} else {
    Write-Host "- Portable FFmpeg installation not found. Skipping." -ForegroundColor DarkGray
}

if (Test-Path $DevToolsDir) {
    if (-not (Get-ChildItem -Path $DevToolsDir)) {
        Write-Host "Removing empty DevTools directory..."
        Remove-Item -Path $DevToolsDir -Force
    } else {
        Write-Host "- DevTools directory is not empty. Leaving it intact." -ForegroundColor DarkGray
    }
}

Write-Host "Uninstall/Cleanup complete!" -ForegroundColor Green