<# :
@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: Must be run as Administrator. Right-click ^> "Run as administrator".
    pause & exit /b 1
)
set VKS3D_DIR=%~dp0
if "%VKS3D_DIR:~-1%"=="\" set VKS3D_DIR=%VKS3D_DIR:~0,-1%
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$d=$env:VKS3D_DIR; $lines=Get-Content '%~f0'; $ps=$lines[($lines.IndexOf('#>')+1)..($lines.Length-1)] -join \"`n\"; Invoke-Expression $ps"
pause & exit /b
#>
# ============================================================================
# VKS3D — Vulkan Stereoscopic ICD Uninstaller  (self-contained in uninstall.bat)
# ============================================================================
$ErrorActionPreference = "Stop"
$InstallDir = $env:VKS3D_DIR

$VkDriverKey64 = "HKLM:\SOFTWARE\Khronos\Vulkan\Drivers"
$VkDriverKey32 = "HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
$SaveKey64     = "HKLM:\SOFTWARE\VKS3D\DisplacedICDs64"
$SaveKey32     = "HKLM:\SOFTWARE\VKS3D\DisplacedICDs32"

$Entries = @(
    @{ Bits=64; JSON="VKS3D_x64.json"; DriverKey=$VkDriverKey64; SaveKey=$SaveKey64 },
    @{ Bits=32; JSON="VKS3D_x86.json"; DriverKey=$VkDriverKey32; SaveKey=$SaveKey32 }
)

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  VKS3D Vulkan Stereoscopic ICD Uninstaller" -ForegroundColor Cyan
Write-Host "  Directory: $InstallDir" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

foreach ($entry in $Entries) {
    $bits   = $entry.Bits
    $json   = Join-Path $InstallDir $entry.JSON
    $drvKey = $entry.DriverKey
    $savKey = $entry.SaveKey

    Write-Host "[$bits-bit] " -NoNewline -ForegroundColor White

    # Remove VKS3D
    $existing = Get-ItemProperty -Path $drvKey -Name $json -ErrorAction SilentlyContinue
    if ($existing) {
        Remove-ItemProperty -Path $drvKey -Name $json -Force
        Write-Host "Removed VKS3D registration." -ForegroundColor Green
    } else {
        Write-Host "VKS3D not registered (nothing to remove)." -ForegroundColor Gray
    }

    # Restore displaced ICDs
    $saved = Get-Item -Path $savKey -ErrorAction SilentlyContinue
    if ($saved) {
        foreach ($name in $saved.GetValueNames()) {
            Write-Host "  Restoring:  $name" -ForegroundColor Cyan
            Set-ItemProperty -Path $drvKey -Name $name -Value 0 -Type DWord -Force
        }
        Remove-Item -Path $savKey -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Clean up VKS3D registry parent key if now empty
Remove-Item -Path "HKLM:\SOFTWARE\VKS3D" -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Uninstallation complete. Original ICDs restored." -ForegroundColor Green
Write-Host ""
