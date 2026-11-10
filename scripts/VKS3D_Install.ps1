<#
.SYNOPSIS
    VKS3D Vulkan Stereoscopic ICD Installer / Uninstaller

.DESCRIPTION
    Registers VKS3D_x64.dll and VKS3D_x86.dll with the Windows Vulkan loader.

    CRITICAL: The Vulkan loader loads every active ICD it finds in the registry.
    If the real GPU driver (e.g. NVIDIA nvoglv64.dll) is ALSO registered, the app
    will see two physical devices and can bypass VKS3D entirely, rendering in 2D.

    This installer displaces competing ICDs by setting their registry value from
    0 (active) to 1 (disabled), and saves the list so uninstall can restore them.

.PARAMETER Uninstall
    Remove VKS3D from the registry and restore any displaced ICDs.

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

# Registry locations
$VkDriverKey64 = "HKLM:\SOFTWARE\Khronos\Vulkan\Drivers"
$VkDriverKey32 = "HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
# Where we save the list of displaced ICDs so uninstall can restore them
$SaveKey64     = "HKLM:\SOFTWARE\VKS3D\DisplacedICDs64"
$SaveKey32     = "HKLM:\SOFTWARE\VKS3D\DisplacedICDs32"

$Entries = @(
    @{ Bits=64; DLL=Join-Path $InstallDir "VKS3D_x64.dll"; JSON=Join-Path $InstallDir "VKS3D_x64.json"
       DriverKey=$VkDriverKey64; SaveKey=$SaveKey64 },
    @{ Bits=32; DLL=Join-Path $InstallDir "VKS3D_x86.dll"; JSON=Join-Path $InstallDir "VKS3D_x86.json"
       DriverKey=$VkDriverKey32; SaveKey=$SaveKey32 }
)

function Write-Banner {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "  VKS3D Vulkan Stereoscopic ICD" -ForegroundColor Cyan
    Write-Host ("  Action: " + $(if ($Uninstall) {"UNINSTALL"} else {"INSTALL"})) `
        -ForegroundColor $(if ($Uninstall) {"Yellow"} else {"Green"})
    Write-Host "  Directory: $InstallDir"
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host ""
}

function Update-JsonLibraryPath([string]$JsonPath, [string]$DllPath) {
    $content = Get-Content $JsonPath -Raw
    $escaped = $DllPath -replace '\\', '\\\\'
    $content = $content -replace '"library_path"\s*:\s*"[^"]*"', "`"library_path`": `"$escaped`""
    Set-Content $JsonPath $content -Encoding UTF8
}

function Ensure-Key([string]$Path) {
    if (-not (Test-Path $Path)) { New-Item -Path $Path -Force | Out-Null }
}

# ── Install ────────────────────────────────────────────────────────────────

function Install-Entry($entry) {
    $bits    = $entry.Bits
    $dll     = $entry.DLL
    $json    = $entry.JSON
    $drvKey  = $entry.DriverKey
    $saveKey = $entry.SaveKey

    Write-Host "[$bits-bit] " -NoNewline -ForegroundColor White

    if (-not (Test-Path $dll))  { Write-Host "SKIP — $dll not found."  -ForegroundColor Yellow; return }
    if (-not (Test-Path $json)) { Write-Host "SKIP — $json not found." -ForegroundColor Yellow; return }

    # Patch JSON with absolute DLL path
    Update-JsonLibraryPath -JsonPath $json -DllPath $dll

    # Ensure driver key exists
    Ensure-Key $drvKey
    Ensure-Key $saveKey

    # ── Displace competing ICDs ─────────────────────────────────────────────
    # Read all active (value=0) entries, disable any that aren't VKS3D
    $props = Get-Item -Path $drvKey -ErrorAction SilentlyContinue
    if ($props) {
        foreach ($name in $props.GetValueNames()) {
            $val = $props.GetValue($name)
            if ($val -ne 0) { continue }                        # already disabled
            if ($name -match "VKS3D" -or $name -match "vks3d") { continue }  # us

            Write-Host "  Displacing: $name" -ForegroundColor Yellow
            # Save original path in our key so uninstall can restore it
            Set-ItemProperty -Path $saveKey -Name $name -Value 0 -Type DWord -Force
            # Disable the competing ICD (value 1 = skip by loader)
            Set-ItemProperty -Path $drvKey  -Name $name -Value 1 -Type DWord -Force
        }
    }

    # Register VKS3D (value 0 = active)
    New-ItemProperty -Path $drvKey -Name $json -Value 0 -PropertyType DWord -Force | Out-Null
    Write-Host "Registered: $json" -ForegroundColor Green
}

# ── Uninstall ──────────────────────────────────────────────────────────────

function Uninstall-Entry($entry) {
    $bits    = $entry.Bits
    $json    = $entry.JSON
    $drvKey  = $entry.DriverKey
    $saveKey = $entry.SaveKey

    Write-Host "[$bits-bit] " -NoNewline -ForegroundColor White

    # Remove VKS3D registration
    $existing = Get-ItemProperty -Path $drvKey -Name $json -ErrorAction SilentlyContinue
    if ($existing) {
        Remove-ItemProperty -Path $drvKey -Name $json -Force
        Write-Host "Removed VKS3D registration." -ForegroundColor Green
    } else {
        Write-Host "VKS3D not registered (nothing to remove)." -ForegroundColor Gray
    }

    # Restore displaced ICDs
    $saved = Get-Item -Path $saveKey -ErrorAction SilentlyContinue
    if ($saved) {
        foreach ($name in $saved.GetValueNames()) {
            Write-Host "  Restoring:  $name" -ForegroundColor Cyan
            Set-ItemProperty -Path $drvKey -Name $name -Value 0 -Type DWord -Force
        }
        Remove-Item -Path $saveKey -Recurse -Force -ErrorAction SilentlyContinue
        # Remove parent key if empty
        $parent = Split-Path $saveKey -Parent
        $parentItem = Get-Item $parent -ErrorAction SilentlyContinue
        if ($parentItem -and @($parentItem.GetSubKeyNames()).Count -eq 0 -and
            @($parentItem.GetValueNames()).Count -eq 0) {
            Remove-Item -Path $parent -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# ── Main ───────────────────────────────────────────────────────────────────

Write-Banner

foreach ($entry in $Entries) {
    if ($Uninstall) { Uninstall-Entry $entry }
    else            { Install-Entry   $entry }
}

Write-Host ""
if (-not $Uninstall) {
    Write-Host "Installation complete!" -ForegroundColor Green
    Write-Host ""
    Write-Host "VKS3D is now the sole active Vulkan ICD.  Other ICDs have been" -ForegroundColor Cyan
    Write-Host "disabled in the registry and will be restored by -Uninstall." -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Per-game config: drop a vks3d.ini next to the game's .exe"
    Write-Host "Global config:   $InstallDir\vks3d.ini"
} else {
    Write-Host "Uninstallation complete.  Original ICDs restored." -ForegroundColor Green
}
Write-Host ""
