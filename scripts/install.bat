@echo off
:: ============================================================================
:: VKS3D — Vulkan Stereoscopic ICD Installer
:: ============================================================================
:: Registers VKS3D and DISPLACES any competing Vulkan ICD (e.g. NVIDIA's own
:: JSON entry).  If the real GPU ICD remains active alongside VKS3D, the Vulkan
:: loader will load both and the app may bypass VKS3D, rendering in 2D.
::
:: Displaced ICD paths are saved under HKLM\SOFTWARE\VKS3D\DisplacedICDs*
:: and restored automatically by uninstall.bat.
::
:: Must be run as Administrator.
:: Run from the directory containing the DLLs and JSON manifests.
:: ============================================================================

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and choose "Run as administrator".
    pause
    exit /b 1
)

set "INSTALL_DIR=%~dp0"
if "%INSTALL_DIR:~-1%"=="\" set "INSTALL_DIR=%INSTALL_DIR:~0,-1%"

echo.
echo ============================================================
echo   VKS3D Vulkan Stereoscopic ICD Installer
echo ============================================================
echo   Install directory: %INSTALL_DIR%
echo.

:: ── 64-bit ────────────────────────────────────────────────────────────────
set "JSON64=%INSTALL_DIR%\VKS3D_x64.json"
set "DLL64=%INSTALL_DIR%\VKS3D_x64.dll"
set "DRVKEY64=HKLM\SOFTWARE\Khronos\Vulkan\Drivers"
set "SAVEKEY64=HKLM\SOFTWARE\VKS3D\DisplacedICDs64"

if not exist "%DLL64%"  goto :SKIP64
if not exist "%JSON64%" goto :SKIP64

echo [64-bit] Displacing competing ICDs...
call :DisplaceICDs "%DRVKEY64%" "%SAVEKEY64%" "VKS3D_x64"

echo [64-bit] Registering VKS3D...
reg add "%DRVKEY64%" /v "%JSON64%" /t REG_DWORD /d 0 /f >nul 2>&1
if %errorLevel% equ 0 (
    echo   [OK] %JSON64%
) else (
    echo   [FAIL] Could not write 64-bit entry.
)
:SKIP64

:: ── 32-bit ────────────────────────────────────────────────────────────────
set "JSON32=%INSTALL_DIR%\VKS3D_x86.json"
set "DLL32=%INSTALL_DIR%\VKS3D_x86.dll"
set "DRVKEY32=HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
set "SAVEKEY32=HKLM\SOFTWARE\VKS3D\DisplacedICDs32"

if not exist "%DLL32%"  goto :SKIP32
if not exist "%JSON32%" goto :SKIP32

echo [32-bit] Displacing competing ICDs...
call :DisplaceICDs "%DRVKEY32%" "%SAVEKEY32%" "VKS3D_x86"

echo [32-bit] Registering VKS3D...
reg add "%DRVKEY32%" /v "%JSON32%" /t REG_DWORD /d 0 /f >nul 2>&1
if %errorLevel% equ 0 (
    echo   [OK] %JSON32%
) else (
    echo   [FAIL] Could not write 32-bit entry.
)
:SKIP32

echo.
echo ============================================================
echo   Installation complete.
echo   Competing ICDs have been disabled.  Run uninstall.bat
echo   to restore them if you remove VKS3D.
echo ============================================================
echo.
pause
exit /b 0

:: ────────────────────────────────────────────────────────────────────────────
:DisplaceICDs  drvKey saveKey selfTag
::  Enumerate all DWORD=0 entries in drvKey.
::  Any that don't contain selfTag are saved to saveKey and set to 1.
:: ────────────────────────────────────────────────────────────────────────────
setlocal enabledelayedexpansion
set "_DK=%~1"
set "_SK=%~2"
set "_TAG=%~3"

:: Use PowerShell for enumeration (reg query output parsing is fragile)
for /f "tokens=* usebackq" %%L in (`powershell -NoProfile -Command ^
    "try { $k=Get-Item -Path 'HKLM:\%_DK:HKLM\=%' -EA Stop; ^
     $k.GetValueNames() | foreach { $v=$k.GetValue($_); ^
     if($v -eq 0 -and $_ -notmatch '%_TAG%') { $_ } } } catch {}"`) do (
    set "_PATH=%%L"
    if not "!_PATH!"=="" (
        echo   Displacing: !_PATH!
        reg add "%_SK%" /v "!_PATH!" /t REG_DWORD /d 0 /f >nul 2>&1
        reg add "%_DK%" /v "!_PATH!" /t REG_DWORD /d 1 /f >nul 2>&1
    )
)
endlocal
goto :eof
