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
# VKS3D — Vulkan Stereoscopic ICD Installer  (self-contained in install.bat)
# ============================================================================
$ErrorActionPreference = "Stop"
$InstallDir = $env:VKS3D_DIR

$VkDriverKey64 = "HKLM:\SOFTWARE\Khronos\Vulkan\Drivers"
$VkDriverKey32 = "HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers"
$SaveKey64     = "HKLM:\SOFTWARE\VKS3D\DisplacedICDs64"
$SaveKey32     = "HKLM:\SOFTWARE\VKS3D\DisplacedICDs32"

$Entries = @(
    @{ Bits=64; DLL="VKS3D_x64.dll"; JSON="VKS3D_x64.json"; DriverKey=$VkDriverKey64; SaveKey=$SaveKey64 },
    @{ Bits=32; DLL="VKS3D_x86.dll"; JSON="VKS3D_x86.json"; DriverKey=$VkDriverKey32; SaveKey=$SaveKey32 }
)

function Ensure-Key([string]$Path) {
    if (-not (Test-Path $Path)) { New-Item -Path $Path -Force | Out-Null }
}

function Update-JsonLibraryPath([string]$JsonPath, [string]$DllPath) {
    # Use forward slashes in the JSON library_path.
    # Forward slashes work as path separators on Windows (LoadLibraryA accepts them),
    # require NO JSON escaping, and are read correctly by all Vulkan loader versions
    # including the old 1.1.114 loader on driver 426.06 which does NOT unescape \.
    # Backslash-escaped paths (C:\\Programs\\...) break old loaders that read
    # the JSON literally without unescaping.
    $p = $DllPath.Replace('\', '/').Replace('"', '\"')
    $json = "{`n    `"file_format_version`": `"1.0.0`",`n    `"ICD`": {`n        `"library_path`": `"$p`",`n        `"api_version`": `"1.1.0`"`n    }`n}"
    [System.IO.File]::WriteAllText($JsonPath, $json, [System.Text.Encoding]::UTF8)
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  VKS3D Vulkan Stereoscopic ICD Installer" -ForegroundColor Cyan
Write-Host "  Directory: $InstallDir" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

foreach ($entry in $Entries) {
    $bits   = $entry.Bits
    $dll    = Join-Path $InstallDir $entry.DLL
    $json   = Join-Path $InstallDir $entry.JSON
    $drvKey = $entry.DriverKey
    $savKey = $entry.SaveKey

    Write-Host "[$bits-bit] " -NoNewline -ForegroundColor White

    if (-not (Test-Path $dll))  { Write-Host "SKIP - $($entry.DLL) not found."  -ForegroundColor Yellow; continue }
    if (-not (Test-Path $json)) { Write-Host "SKIP - $($entry.JSON) not found." -ForegroundColor Yellow; continue }

    Update-JsonLibraryPath -JsonPath $json -DllPath $dll
    Ensure-Key $drvKey
    Ensure-Key $savKey

    $props = Get-Item -Path $drvKey -ErrorAction SilentlyContinue
    if ($props) {
        foreach ($name in $props.GetValueNames()) {
            $val = $props.GetValue($name)
            if ($val -ne 0) { continue }
            if ($name -match "VKS3D" -or $name -match "vks3d") { continue }
            Write-Host "  Displacing: $name" -ForegroundColor Yellow
            Set-ItemProperty -Path $savKey -Name $name -Value 0 -Type DWord -Force
            Set-ItemProperty -Path $drvKey -Name $name -Value 1 -Type DWord -Force
        }
    }

    New-ItemProperty -Path $drvKey -Name $json -Value 0 -PropertyType DWord -Force | Out-Null
    Write-Host "Registered: $json" -ForegroundColor Green
}

Write-Host ""
Write-Host "Installation complete!" -ForegroundColor Green
Write-Host "VKS3D is the sole active Vulkan ICD. Run uninstall.bat to restore others." -ForegroundColor Cyan
Write-Host ""
