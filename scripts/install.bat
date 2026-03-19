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
    $content = Get-Content $JsonPath -Raw
    # JSON string escaping: each backslash must become \\
    # In PowerShell the replacement string for -replace uses \ as escape, so
    # \\ in the replacement means one literal backslash.  We need each \ in
    # $DllPath to become \\ in the JSON, so we use [regex]::Escape to get
    # the literal string into the replacement safely, then do a second pass
    # to write the JSON-escaped form.
    $json_escaped = $DllPath.Replace('\', '\\')   # C:\Foo -> C:\\Foo  (for JSON)
    # -replace replacement: $json_escaped may contain \ which means escape next char;
    # double them again so -replace treats them as literals.
    $replace_safe = $json_escaped.Replace('\', '\\')
    $content = $content -replace '"library_path"\s*:\s*"[^"]*"', "`"library_path`": `"$replace_safe`""
    Set-Content $JsonPath $content -Encoding UTF8 -NoNewline
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
