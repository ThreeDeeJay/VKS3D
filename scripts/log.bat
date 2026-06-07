@Echo OFF
SETlocal EnableExtensions
SETlocal EnableDelayedExpansion
pushd "%~dp0"

For %%A in (%*) do (
    cd "%%~dpA"
    Set STEREO_ENABLED=1
    Set STEREO_LOGFILE_PATH=%~dp0%%~nxA+VKS3D.log
    "%%~dpA%%~nxA" --fullscreen --width 1920 --height 1080
    Echo Error level: !ERRORLEVEL!
    If not exist "%~dp0%%~nA/*.spv" (rmdir "%~dp0%%~nA")
    )
If !ERRORLEVEL! NEQ 0 (pause)