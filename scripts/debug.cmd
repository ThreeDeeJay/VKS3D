@Echo OFF
SETlocal EnableExtensions
SETlocal EnableDelayedExpansion
pushd "%~dp0"

For %%A in (%*) do (
    pushD "%%~dpA"
    Set STEREO_LOGFILE_PATH=%~dp0%%~nxA\%%~nxA+VKS3D.log
    Set VKS3D_DUMP_SPIRV=%~dp0%%~nxA
    Set VKS3D_SKIP_SHADER_PATCHES=
    if not exist "%~dp0%%~nxA" (mkdir "%~dp0%%~nxA")
    Echo %%~nA
    "%%~dpA%%~nxA"
    If exist "%~dp0%%~nxA\%%~nxA+VKS3D.log" (Start "" "%~dp0%%~nxA\%%~nxA+VKS3D.log")
    If exist "%~dp0%%~nxA/*.spv" (
        If exist "!VULKAN_SDK!\Bin\spirv-dis.exe" (
            pushD "%~dp0%%~nxA"
            For %%S in (*.spv) do (
                Echo Disassembling "%%~nxS"...
                "!VULKAN_SDK!\Bin\spirv-dis.exe" "%%~nxS" -o "%%~nxS.asm"
                if !ERRORLEVEL! NEQ 0 (Pause)
                Echo.
                )
            Explorer "%~dp0%%~nxA"
            )
        ) else (
        If not exist "%~dp0%%~nxA/*.spv" (
            rmdir /s /q "%~dp0%%~nxA"
            )
        )
    )