@echo off
:: ============================================================================
:: VKS3D — Vulkan Stereoscopic ICD Uninstaller
:: Thin wrapper — all logic lives in Install-VKS3D.ps1
:: Must be run as Administrator.
:: ============================================================================

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and choose "Run as administrator".
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-VKS3D.ps1" -InstallDir "%~dp0" -Uninstall
pause
