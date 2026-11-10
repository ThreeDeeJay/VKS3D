@Echo OFF
SETlocal EnableExtensions
SETlocal EnableDelayedExpansion

pushd "%~dp0"

Set VKS3D_DIR=%~dp0
set DLLPath=%~dp0VKS3D_x64.dll
set DLLPath=!DLLPath:\=\\!
Set VKS3D_SKIP_SHADER_PATCHES=

for /f "tokens=4 delims==." %%V in (
  'wmic datafile where name^="!DLLPath!" get Version /value 2^>Nul'
) do Set "VKS3DVersion=%%V"

for /f "tokens=3 delims=/#@" %%C in (
  'wmic datafile where name^="!DLLPath!" get Manufacturer /value 2^>Nul'
) do Set "VKS3DCompany=%%C"
If defined VKS3DCompany (
    Set VKS3D_BUILDS=%~dp0Builds\!VKS3DCompany!
) 

If not "%~1"=="" (
    For %%A in (%*) do (
        Set "DumpPath=%~dp0VKS3D_Dump/%%~nA + VKS3D v!VKS3DVersion!"
        Set "DumpPath=!DumpPath:/=\!"
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
                    7z -y e "%%~A" -o"!VKS3D_DIR!" "-i^!*.dll" 1>>NUL 2>&1
                    for /f "tokens=3 delims=/#@" %%C in (
                      'wmic datafile where name^="!DLLPath!" get Manufacturer /value 2^>Nul'
                  ) do Set "VKS3DCompany=%%C"
                    If defined VKS3DCompany (
                        Set VKS3D_BUILDS=%~dp0Builds\!VKS3DCompany!
                    ) 
                    If not exist "!VKS3D_BUILDS!\%%R" (MkDir "!VKS3D_BUILDS!\%%R")
                    7z -y e "%%~A" -o"!VKS3D_BUILDS!\%%R" "-i^!*.dll"
                    copy /y "!VKS3D_BUILDS!\%%R\*" "!VKS3D_DIR!"
                    exit
                )
            ) else (
                If "%%~xA"==".spv" (
                    Echo Validating "%%~nxA" reports:
                    If exist "!VULKAN_SDK!\Bin\spirv-val.exe" (
                        Set ValidationStarted=True
                        "!VULKAN_SDK!\Bin\spirv-val.exe" "%%~A"
                    )
                    Echo Decompiling "%%~nxA"
                    If exist "!VULKAN_SDK!\Bin\spirv-dis.exe" ("!VULKAN_SDK!\Bin\spirv-dis.exe" "%%~A" -o "%%~dpA\%%~nxA.asm")
                    Set Run=%%~dpA%%~nxA.asm
                ) else (
                    pushD "%%~dpA"
                    Set DXVK_STATE_CACHE=0
                    Set DXVK_LOG_PATH=!DumpPath!
                    Set VKD3D_LOG_FILE=!DumpPath!/%%~nA+VKD3D.log
                    Set VKD3D_SHADER_CACHE_PATH=0
                    Set VKD3D_VULKAN_DEVICE=0
                    Set STEREO_LOGFILE_PATH=!DumpPath!/%%~nA+VKS3D.log
                    Set VKS3D_LOG_FILE_PATH=!DumpPath!/%%~nA+VKS3D.log
                    Set VKS3D_DUMP_SPIRV=!DumpPath!
                    if not exist "!DumpPath!" (
                        mkdir "!DumpPath!"
                    ) else (
                        del /S /Q "!DumpPath!\*.spv" 1>>NUL 2>&1
                        del /S /Q "!DumpPath!\*.asm" 1>>NUL 2>&1
                        del /S /Q "!DumpPath!\%%~nA+VKS3D.log" 1>>NUL 2>&1
                        del /S /Q "!DumpPath!\%%~nA+VKS3D_spirv-val_error.log" 1>>NUL 2>&1
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
                    If exist "%%~dpAvkd3d-proton.cache" (del "%%~dpAvkd3d-proton.cache")
                    If exist "%%~dpA*_dxgi.log" (del "%%~dpA*_dxgi.log")
                    "%%~dpA%%~nxA"
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
                                        Set ValidationStarted=True
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
) else (
    Echo [93mNo drag and drop input specified.
    Echo [94mEnabling temporary logging and shader dumping for games that fail to dump them via drag and drop...
    setx VKS3D_DUMP_SPIRV "%~dp0VKS3D_Dump" 1>>NUL 2>&1
    setx STEREO_LOGFILE_PATH "%~dp0VKS3D_Dump\VKS3D.log" 1>>NUL 2>&1
    setx VKS3D_LOG_FILE_PATH "%~dp0VKS3D_Dump\VKS3D.log" 1>>NUL 2>&1
    setx DXVK_LOG_PATH "%~dp0VKS3D_Dump" 1>>NUL 2>&1
    setx VKD3D_SHADER_DUMP_PATH "%~dp0VKS3D_Dump" 1>>NUL 2>&1
    setx VKD3D_LOG_FILE "%~dp0VKS3D_Dump\VKD3D.log" 1>>NUL 2>&1
    If not exist "%~dp0VKS3D_Dump" (MkDir "%~dp0VKS3D_Dump")
    Echo [92mDone.
    Echo.
    Echo [96mPress Enter after running and closing a game using Vulkan ^(native or wrapped to it^) to validate the shader dumps from:
    Echo "%~dp0VKS3D_Dump\*.spv"
    Echo Then dumping and logging will be disabled, or you can just close this window now to keep them enabled
    Echo [93m^(not recommended to do permanently^).
    pause >Nul
    Echo [0m
    If exist "%~dp0VKS3D_Dump\*.spv" (
        If exist "!VULKAN_SDK!\Bin\*.exe" (
            pushD "%~dp0VKS3D_Dump"
            Set /A Warnings=0
            Set /A Errors=0
            For %%S in (*.spv) do (
                If exist "!VULKAN_SDK!\Bin\spirv-val.exe" (
                    Set ValidationStarted=True
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
                                    If exist "!ShaderFile!.asm" (
                                        Set /A Warnings=!Warnings!+1
                                    ) else (
                                        Set /A Errors=!Errors!+1
                                    )
                                    Title Warnings=!Warnings! Errors=!Errors!
                                )
                            )
                            If not defined Run (Set Run=!ShaderFile!.asm)
                        )
                    )
                )
            )
        )
    )
)
If "!ValidationStarted!"=="True" (
    Echo.
    Echo Patched shader dump report:
    If !Warnings! GTR 0 (Echo [93mWarnings:  !Warnings!) else (Echo [92mWarnings:  !Warnings!)
    If !Errors!   GTR 0 (Echo [91mErrors:    !Errors!)   else (Echo [92mErrors:    !Errors!)
)
Echo [0m
Echo Disabling logging and dumping...
setx VKS3D_DUMP_SPIRV "" 1>>NUL 2>&1
setx STEREO_LOGFILE_PATH "" 1>>NUL 2>&1
setx VKS3D_LOG_FILE_PATH "" 1>>NUL 2>&1
setx DXVK_LOG_PATH "" 1>>NUL 2>&1
setx VKD3D_SHADER_DUMP_PATH "" 1>>NUL 2>&1
setx VKD3D_LOG_FILE "" 1>>NUL 2>&1
Echo [92mDone
If defined Run (
    Echo [96mPress any key to open "!Run!"
) else (
    Echo [96mPress any key to exit.
)
pause >Nul
If defined ShaderFile (pause >Nul)
If defined Run ("!Run!")
exit