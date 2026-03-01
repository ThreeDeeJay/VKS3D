@echo off
:: ============================================================================
:: VKS3D — Vulkan Stereoscopic ICD Uninstaller
:: ============================================================================
:: Removes VKS3D from the Vulkan loader registry.
:: Must be run as Administrator.
:: Run from the directory containing the JSON manifests.
:: ============================================================================

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    pause
    exit /b 1
)

set "INSTALL_DIR=%~dp0"
if "%INSTALL_DIR:~-1%"=="\" set "INSTALL_DIR=%INSTALL_DIR:~0,-1%"

echo.
echo ============================================================
echo   VKS3D Vulkan Stereoscopic ICD Uninstaller
echo ============================================================

set "JSON64=%INSTALL_DIR%\VKS3D_x64.json"
set "JSON32=%INSTALL_DIR%\VKS3D_x86.json"

echo Removing 64-bit ICD registration...
reg delete "HKLM\SOFTWARE\Khronos\Vulkan\Drivers" ^
    /v "%JSON64%" /f >nul 2>&1
if %errorLevel% equ 0 (
    echo   [OK] Removed: "%JSON64%"
) else (
    echo   [INFO] Not registered or already removed.
)

echo Removing 32-bit ICD registration...
reg delete "HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers" ^
    /v "%JSON32%" /f >nul 2>&1
if %errorLevel% equ 0 (
    echo   [OK] Removed: "%JSON32%"
) else (
    echo   [INFO] Not registered or already removed.
)

echo.
echo ============================================================
echo   VKS3D uninstalled.
echo ============================================================
echo.
pause
