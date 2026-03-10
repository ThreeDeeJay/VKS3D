@echo off
:: ============================================================================
:: VKS3D — Vulkan Stereoscopic ICD Uninstaller
:: ============================================================================
:: Removes VKS3D from the Vulkan loader registry and restores any ICDs that
:: were displaced during installation.
:: Must be run as Administrator.
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
set "DRVKEY64=HKLM\SOFTWARE\Khronos\Vulkan\Drivers"
set "DRVKEY32=HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
set "SAVEKEY64=HKLM\SOFTWARE\VKS3D\DisplacedICDs64"
set "SAVEKEY32=HKLM\SOFTWARE\VKS3D\DisplacedICDs32"

echo [64-bit] Removing VKS3D registration...
reg delete "%DRVKEY64%" /v "%JSON64%" /f >nul 2>&1
if %errorLevel% equ 0 (echo   [OK] Removed.) else (echo   [INFO] Not found.)

echo [64-bit] Restoring displaced ICDs...
call :RestoreICDs "%DRVKEY64%" "%SAVEKEY64%"

echo [32-bit] Removing VKS3D registration...
reg delete "%DRVKEY32%" /v "%JSON32%" /f >nul 2>&1
if %errorLevel% equ 0 (echo   [OK] Removed.) else (echo   [INFO] Not found.)

echo [32-bit] Restoring displaced ICDs...
call :RestoreICDs "%DRVKEY32%" "%SAVEKEY32%"

:: Clean up VKS3D save key
reg delete "HKLM\SOFTWARE\VKS3D" /f >nul 2>&1

echo.
echo ============================================================
echo   VKS3D uninstalled.  Original ICDs restored.
echo ============================================================
echo.
pause
exit /b 0

:RestoreICDs  drvKey saveKey
setlocal enabledelayedexpansion
set "_DK=%~1"
set "_SK=%~2"
for /f "tokens=* usebackq" %%L in (`powershell -NoProfile -Command ^
    "try { $k=Get-Item -Path 'HKLM:\%_SK:HKLM\=%' -EA Stop; ^
     $k.GetValueNames() } catch {}"`) do (
    set "_PATH=%%L"
    if not "!_PATH!"=="" (
        echo   Restoring: !_PATH!
        reg add "%_DK%" /v "!_PATH!" /t REG_DWORD /d 0 /f >nul 2>&1
    )
)
endlocal
goto :eof
