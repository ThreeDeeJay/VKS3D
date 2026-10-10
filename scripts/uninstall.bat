<# :
@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Requesting administrator privileges ^(required to unregister Vulkan ICDs^)...
    powershell.exe -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)
set VKS3D_DIR=%~dp0
if "%VKS3D_DIR:~-1%"=="\" set VKS3D_DIR=%VKS3D_DIR:~0,-1%
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$d=$env:VKS3D_DIR; $lines=Get-Content '%~f0'; $ps=$lines[($lines.IndexOf('#>')+1)..($lines.Length-1)] -join \"`n\"; Invoke-Expression $ps"
exit /b 0
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
    @{ Bits=32; JSON="VKS3D_x86.json"; DriverKey=$VkDriverKey32; SaveKey=$SaveKey32 },
    @{ Bits=64; JSON="VKS3D_x64.json"; DriverKey=$VkDriverKey64; SaveKey=$SaveKey64 }
)

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  VKS3D Vulkan Stereoscopic ICD Uninstaller" -ForegroundColor Cyan
Write-Host "  Directory: $InstallDir" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

foreach ($entry in $Entries) {
    $bits   = $entry.Bits
    $json   = $entry.JSON
    $drvKey = $entry.DriverKey
    $savKey = $entry.SaveKey

    Write-Host "[$bits-bit] " -NoNewline -ForegroundColor White

    # Remove VKS3D
    $regItem = Get-Item -Path $drvKey -ErrorAction SilentlyContinue
    if ($regItem) {
        $matched = $regItem.GetValueNames() | Where-Object { $_ -match [regex]::Escape($json) -or $_ -match "VKS3D.*\.json" }
        if ($matched) {
            foreach ($keyName in $matched) {
                Remove-ItemProperty -Path $drvKey -Name $keyName -Force -ErrorAction SilentlyContinue
                Write-Host "Removed VKS3D registration: $keyName" -ForegroundColor Green
            }
        } else {
            Write-Host "VKS3D not registered (nothing to remove)." -ForegroundColor Gray
        }
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

Write-Host "Press Enter to delete the VKS3D files from the current folder."
Read-Host

Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "VKS3D")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "ReadMe.txt")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "License.txt")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "VKS3D_x86.json")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "VKS3D_x64.json")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "VKS3D_x86.dll")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "VKS3D_x64.dll")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "vks3d.ini")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "debug.cmd")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "Portable.reg")
Remove-Item -ErrorAction SilentlyContinue -Recurse -Force -Path (Join-Path $InstallDir "Install.bat")

# Schedule self-deletion and cleanup.vbs deletion using a vbscript trick to avoid batch file reference errors
$BatchPath = Join-Path $InstallDir "Uninstall.bat"
$VbsPath = Join-Path $InstallDir "cleanup.vbs"
$VbsCode = @"
Set fso = CreateObject("Scripting.FileSystemObject")
WScript.Sleep 500
On Error Resume Next
fso.DeleteFile "$BatchPath", True
fso.DeleteFile "$VbsPath", True
"@
Set-Content -Path $VbsPath -Value $VbsCode -Force
Start-Process "cscript.exe" -ArgumentList $VbsPath -WindowStyle Hidden
