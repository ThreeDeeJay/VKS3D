@Echo OFF
SETlocal EnableExtensions
SETlocal EnableDelayedExpansion
pushd "%~dp0"

Set VKS3D_DIR=%~dp0
Set VKS3D_BUILDS=%~dp0\Builds\dev
Set VKS3D_SKIP_SHADER_PATCHES=
Set VKS3D_SHADER_OBJECTS_MONO=
set DLLPath=%~dp0VKS3D_x64.dll
for /f "tokens=4 delims==." %%V in (
  'wmic datafile where name^="!DLLPath:\=\\!" get version /value 2^>Nul'
) do Set "VKS3DVersion=%%V"

For %%A in (%*) do (
    Set "DumpPath=%~dp0Dump/%%~nA + VKS3D v!VKS3DVersion!"
    Set "DumpPath=!DumpPath:\=/!"
    If exist "%%~A\*.dll" (
        copy /y "%%~A\*.dll" "!VKS3D_DIR!"
        If !ErrorLevel! NEQ 0 (
            Echo Failed to copy files. Error level: !ErrorLevel!
            Pause
        )
        exit
    ) else (
        If "%%~xA"==".zip" (
            for /f "tokens=2 delims=r@" %%R in ("%%~nA") do (
                If not exist "!VKS3D_BUILDS!\%%R" (MkDir "!VKS3D_BUILDS!\%%R")
                7z -y e "%%~A" -o"!VKS3D_BUILDS!\%%R" "-i^!*.dll"
                copy /y "!VKS3D_BUILDS!\%%R\*" "!VKS3D_DIR!"
                exit
            )
        ) else (
            If "%%~xA"==".spv" (
                Echo Validating "%%~nxA"
                If exist "!VULKAN_SDK!\Bin\spirv-val.exe" ("!VULKAN_SDK!\Bin\spirv-val.exe" "%%~A")
                Echo Decompiling "%%~nxA"
                If exist "!VULKAN_SDK!\Bin\spirv-dis.exe" ("!VULKAN_SDK!\Bin\spirv-dis.exe" "%%~A" -o "%%~dpA\%%~nxA.asm")
                Set Run=%%~dpA%%~nxA.asm
            ) else (
                pushD "%%~dpA"
                Set VKD3D_LOG_FILE=!DumpPath!/%%~nA+VKD3D.log
                Set VKD3D_SHADER_DUMP_PATH=!DumpPath!
                Set VKD3D_VULKAN_DEVICE=0
                Set VKD3D_FRAME_RATE=
                Set DXVK_STATE_CACHE=0 
                Set VK_ICD_FILENAMES=
                Set STEREO_ENABLED=
                Set STEREO_FLIP_EYES=
                Set STEREO_REAL_ICD=
                Set STEREO_REAL_ICD=
                Set STEREO_REAL_ICD=
                Set STEREO_REAL_ICD=
                Set STEREO_REAL_ICD=
                Set STEREO_FLIP_EYES=
                Set WINE_D3D_CONFIG=
                Set VKS3D_DISABLE_SHADER_PATCH=
                Set VKS3D_SHADER_OBJECTS_MONO=
                Set STEREO_LOGFILE_PATH=!DumpPath!/%%~nA+VKS3D.log
                Set VKS3D_DUMP_SPIRV=!DumpPath!
                Set VKS3D_SKIP_SHADER_PATCHES=
                if not exist "!DumpPath!" (
                    mkdir "!DumpPath!"
                    ) else (
                    del /S /Q "!DumpPath!/*.spv" 1>>NUL 2>&1
                    del /S /Q "!DumpPath!/*.asm" 1>>NUL 2>&1
                    del /S /Q "!DumpPath!/%%~nA+VKS3D.log" 1>>NUL 2>&1
                    del /S /Q "!DumpPath!/%%~nA+VKS3D_spirv-val_error.log" 1>>NUL 2>&1
                    If !ErrorLevel! NEQ 0 (
                        Echo Error deleting "!DumpPath!/*"
                        Pause
                        )
                    )
                Title %%~nA
                Echo %%~nA
                If exist "%%~dpA*.dxvk-cache" (del "%%~dpA*.dxvk-cache")
                If exist "%LOCALAPPDATA%\dxvk\%%~nA.dxvk-cache" (del "%LOCALAPPDATA%\dxvk\%%~nA.dxvk-cache")
                If exist "%%~dpAvkd3d-proton.cache.write" (del "%%~dpAvkd3d-proton.cache.write")
                "%%~dpA%%~nxA"
                REM "%%~dpA%%~nxA" -fullscreen --fullscreen --full-screen --width 1920 --height 1080 -mode 1920x1080x144x32
                if !ErrorLevel! NEQ 0 (
                    Echo.
                    echo %%~nA reported non-zero return code ^(!ErrorLevel!^). Re-running without launch parameters.
                    "%%~dpA%%~nxA"
                )
                If exist "!DumpPath!/%%~nA+VKS3D.log" (Start "" "!DumpPath!/%%~nA+VKS3D.log")
                If defined VKS3D_DUMP_SPIRV (
                    If exist "!DumpPath!/*.spv" (
                        If exist "!VULKAN_SDK!\Bin\*.exe" (
                            pushD "!DumpPath!"
                            Set /A Warnings=0
                            Set /A Errors=0
                            For %%S in (*.spv) do (
                                If exist "!VULKAN_SDK!\Bin\spirv-val.exe" (
                                    Echo Validating "%%~nxS" reports:
                                    "!VULKAN_SDK!\Bin\spirv-val.exe" "%%~nxS"
                                    if !ErrorLevel! NEQ 0 (
                                        If exist "!VULKAN_SDK!\Bin\spirv-dis.exe" (
                                            Set ShaderFile=%%~nxS
                                            Echo Decompiling "%%~nxS"...
                                            "!VULKAN_SDK!\Bin\spirv-dis.exe" "!ShaderFile!"     -o "!ShaderFile!.asm"
                                            "!VULKAN_SDK!\Bin\spirv-dis.exe" "!ShaderFile:+=-!" -o "!ShaderFile:+=-!.asm"
                                            If exist "!ShaderFile:+=-!.asm" (
                                                If not "!ShaderFile!"=="!ShaderFile:+=!" (
                                                    Echo "%%~nxS">>"!DumpPath!/%%~nA+VKS3D_spirv-val_error.log"
                                                    If exist "!ShaderFile!.asm" (
                                                        Set /A Warnings=!Warnings!+1
                                                    ) else (
                                                        Set /A Errors=!Errors!+1
                                                    )
                                                    Title %%~nA: Warnings=!Warnings! Errors=!Errors!
                                                )
                                            )
                                            If not defined Run (Set Run=!ShaderFile!.asm)
                                        )
REM                                        If exist "%%~nxS.asm" (Start "" "%%~nxS.asm")
REM                                        Pause
                                    )
                                )
                                )
                            )
                        ) else (
                        If not exist "!DumpPath!/*" (
                            rmdir /s /q "!DumpPath!"
                            )
                        )
                    ) else (
                    If not exist "!DumpPath!/*.log" (
                        If not exist "!DumpPath!/*.spv" (
                            If not exist "!DumpPath!/*.asm" (
                                rmdir /s /q "!DumpPath!"
                                )
                            )
                        )
                    )
                Echo %%~nA
                Echo.
                )
            )
        )
    )
Echo Patched shader dump report:
If !Warnings! GTR 0 (Echo [93mWarnings:  !Warnings!) else (Echo [92mWarnings:  !Warnings!)
If !Errors!   GTR 0 (Echo [91mErrors:    !Errors!)   else (Echo [92mErrors:    !Errors!)
If defined Run (Echo [96mPress any key to open "!Run!")
 pause >Nul
If defined ShaderFile (pause >Nul)
If defined Run ("!Run!")
exit