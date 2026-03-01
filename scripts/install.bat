@echo off
:: ============================================================================
:: VKS3D — Vulkan Stereoscopic ICD Installer
:: ============================================================================
:: Registers VKS3D_x64.dll (64-bit) and VKS3D_x86.dll (32-bit) with the
:: Vulkan loader by adding manifest paths to the Windows registry.
::
:: Must be run as Administrator.
:: Run from the directory containing the DLLs and JSON manifests.
::
:: Vulkan loader registry locations:
::   64-bit ICDs: HKLM\SOFTWARE\Khronos\Vulkan\Drivers
::   32-bit ICDs: HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers
:: ============================================================================

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and choose "Run as administrator".
    pause
    exit /b 1
)

set "INSTALL_DIR=%~dp0"
:: Remove trailing backslash
if "%INSTALL_DIR:~-1%"=="\" set "INSTALL_DIR=%INSTALL_DIR:~0,-1%"

echo.
echo ============================================================
echo   VKS3D Vulkan Stereoscopic ICD Installer
echo ============================================================
echo   Install directory: %INSTALL_DIR%
echo.

:: ── 64-bit ICD ────────────────────────────────────────────────────────────
set "JSON64=%INSTALL_DIR%\VKS3D_x64.json"
set "DLL64=%INSTALL_DIR%\VKS3D_x64.dll"

if not exist "%DLL64%" (
    echo WARNING: VKS3D_x64.dll not found, skipping 64-bit registration.
    goto :SKIP64
)
if not exist "%JSON64%" (
    echo WARNING: VKS3D_x64.json not found, skipping 64-bit registration.
    goto :SKIP64
)

:: Update JSON to use absolute path for the DLL
:: (The loader needs an absolute path or a path relative to the JSON file)
echo Registering 64-bit ICD...
reg add "HKLM\SOFTWARE\Khronos\Vulkan\Drivers" ^
    /v "%JSON64%" /t REG_DWORD /d 0 /f >nul 2>&1
if %errorLevel% equ 0 (
    echo   [OK] HKLM\SOFTWARE\Khronos\Vulkan\Drivers
    echo        "%JSON64%"
) else (
    echo   [FAIL] Could not write to 64-bit Vulkan driver registry key.
)

:SKIP64

:: ── 32-bit ICD ────────────────────────────────────────────────────────────
set "JSON32=%INSTALL_DIR%\VKS3D_x86.json"
set "DLL32=%INSTALL_DIR%\VKS3D_x86.dll"

if not exist "%DLL32%" (
    echo WARNING: VKS3D_x86.dll not found, skipping 32-bit registration.
    goto :SKIP32
)
if not exist "%JSON32%" (
    echo WARNING: VKS3D_x86.json not found, skipping 32-bit registration.
    goto :SKIP32
)

echo Registering 32-bit ICD...
reg add "HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers" ^
    /v "%JSON32%" /t REG_DWORD /d 0 /f >nul 2>&1
if %errorLevel% equ 0 (
    echo   [OK] HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers
    echo        "%JSON32%"
) else (
    echo   [FAIL] Could not write to 32-bit Vulkan driver registry key.
)

:SKIP32

echo.
echo ============================================================
echo   Installation complete.
echo.
echo   Configuration (set before running your Vulkan app):
echo     set STEREO_SEPARATION=0.065
echo     set STEREO_CONVERGENCE=0.030
echo     set STEREO_ENABLED=1
echo.
echo   To uninstall: run uninstall.bat as Administrator
echo ============================================================
echo.
pause
