@echo off
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo ERROR: Must be run as Administrator. Right-click ^> "Run as administrator".
    pause & exit /b 1
)

reg add "HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers" /v "VKS3D_x86.json" /t REG_DWORD /d 0 /f
reg add "HKLM\SOFTWARE\Khronos\Vulkan\Drivers" /v "VKS3D_x64.json" /t REG_DWORD /d 0 /f
pause