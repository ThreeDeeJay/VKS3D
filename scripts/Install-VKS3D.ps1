<#
.SYNOPSIS
    VKS3D Vulkan Stereoscopic ICD Installer / Uninstaller

.DESCRIPTION
    Registers or unregisters VKS3D_x64.dll and VKS3D_x86.dll with the
    Windows Vulkan loader via the registry.

.PARAMETER Uninstall
    Remove VKS3D from the registry instead of installing.

.PARAMETER InstallDir
    Path to the directory containing the DLLs and JSON manifests.
    Defaults to the directory containing this script.

.EXAMPLE
    .\Install-VKS3D.ps1
    .\Install-VKS3D.ps1 -InstallDir "C:\VKS3D"
    .\Install-VKS3D.ps1 -Uninstall

.NOTES
    Must be run as Administrator.
#>

[CmdletBinding()]
param(
    [switch]$Uninstall,
    [string]$InstallDir = $PSScriptRoot
)

#Requires -RunAsAdministrator

$ErrorActionPreference = "Stop"

$InstallDir = (Resolve-Path $InstallDir).Path

$Entries = @(
    @{
        Bits    = 64
        DLL     = Join-Path $InstallDir "VKS3D_x64.dll"
        JSON    = Join-Path $InstallDir "VKS3D_x64.json"
        RegKey  = "HKLM:\SOFTWARE\Khronos\Vulkan\Drivers"
    },
    @{
        Bits    = 32
        DLL     = Join-Path $InstallDir "VKS3D_x86.dll"
        JSON    = Join-Path $InstallDir "VKS3D_x86.json"
        RegKey  = "HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
    }
)

function Write-Banner {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "  VKS3D Vulkan Stereoscopic ICD" -ForegroundColor Cyan
    if ($Uninstall) {
        Write-Host "  Action: UNINSTALL" -ForegroundColor Yellow
    } else {
        Write-Host "  Action: INSTALL" -ForegroundColor Green
    }
    Write-Host "  Directory: $InstallDir"
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host ""
}

function Update-JsonLibraryPath {
    param([string]$JsonPath, [string]$DllPath)
    $content = Get-Content $JsonPath -Raw
    # Replace library_path value with the absolute DLL path (escape backslashes)
    $escaped = $DllPath -replace '\\', '\\\\'
    $content = $content -replace '"library_path"\s*:\s*"[^"]*"',
                                  "`"library_path`": `"$escaped`""
    Set-Content $JsonPath $content -Encoding UTF8
}

Write-Banner

foreach ($entry in $Entries) {
    $bits = $entry.Bits
    $dll  = $entry.DLL
    $json = $entry.JSON
    $key  = $entry.RegKey

    Write-Host "[$bits-bit] " -NoNewline -ForegroundColor White

    if ($Uninstall) {
        try {
            $regProp = Get-ItemProperty -Path $key -Name $json -ErrorAction SilentlyContinue
            if ($regProp) {
                Remove-ItemProperty -Path $key -Name $json -Force
                Write-Host "Removed from registry." -ForegroundColor Green
            } else {
                Write-Host "Not registered (nothing to remove)." -ForegroundColor Gray
            }
        } catch {
            Write-Host "Failed: $_" -ForegroundColor Red
        }
        continue
    }

    # Install
    if (-not (Test-Path $dll)) {
        Write-Host "SKIP — $dll not found." -ForegroundColor Yellow
        continue
    }
    if (-not (Test-Path $json)) {
        Write-Host "SKIP — $json not found." -ForegroundColor Yellow
        continue
    }

    try {
        # Patch JSON to use absolute DLL path
        Update-JsonLibraryPath -JsonPath $json -DllPath $dll

        # Ensure registry key exists
        if (-not (Test-Path $key)) {
            New-Item -Path $key -Force | Out-Null
        }

        # Register: value name = JSON path, data = DWORD 0
        New-ItemProperty -Path $key -Name $json -Value 0 -PropertyType DWord -Force | Out-Null
        Write-Host "Registered: $json" -ForegroundColor Green
    } catch {
        Write-Host "Failed: $_" -ForegroundColor Red
    }
}

Write-Host ""

if (-not $Uninstall) {
    Write-Host "Installation complete!" -ForegroundColor Green
    Write-Host ""
    Write-Host "Configuration environment variables:" -ForegroundColor Cyan
    Write-Host '  $env:STEREO_SEPARATION  = "0.065"   # IPD in clip-space (~65mm)'
    Write-Host '  $env:STEREO_CONVERGENCE = "0.030"   # Convergence shift'
    Write-Host '  $env:STEREO_ENABLED     = "1"       # 0 to disable'
    Write-Host '  $env:STEREO_FLIP_EYES   = "0"       # 1 for cross-eyed displays'
    Write-Host ""
    Write-Host "Verify with: vulkaninfo | Select-String VKS3D" -ForegroundColor Gray
} else {
    Write-Host "Uninstallation complete." -ForegroundColor Green
}
Write-Host ""
