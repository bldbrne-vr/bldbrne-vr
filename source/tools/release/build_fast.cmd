@echo off
setlocal
rem Matched performance build: ThinLTO + PGO by default. OFF disables both.
rem PLAY: the USE build with all logging compiled out (BBVR_NO_LOGS: no log file, no console
rem lines), in its own folder local\build-play so it never rebuilds the logging build.
rem RELEASE x.y.z: the PLAY build stamped as a public release: the public
rem repository and branch "release" in the window title instead of the work branch.
set "FAST_BUILD_MODE=%~1"
if "%FAST_BUILD_MODE%"=="" set "FAST_BUILD_MODE=USE"
set "FAST_BUILD_RELEASE="
if /I "%FAST_BUILD_MODE%"=="RELEASE" (
    if "%~2"=="" (
        echo Usage: build_fast.cmd RELEASE x.y.z
        exit /b 2
    )
    set "FAST_BUILD_RELEASE=%~2"
    set "FAST_BUILD_MODE=PLAY"
)
if /I not "%FAST_BUILD_MODE%"=="USE" if /I not "%FAST_BUILD_MODE%"=="GENERATE" if /I not "%FAST_BUILD_MODE%"=="OFF" if /I not "%FAST_BUILD_MODE%"=="PLAY" (
    echo Usage: build_fast.cmd [USE^|GENERATE^|OFF^|PLAY]
    exit /b 2
)
pushd "%~dp0\..\.."
set "FAST_BUILD_PGO=%FAST_BUILD_MODE%"
set "FAST_BUILD_NO_LOGS=OFF"
set "FAST_BUILD_DIR=build"
set "FAST_BUILD_NOTE="
if /I "%FAST_BUILD_MODE%"=="PLAY" (
    set "FAST_BUILD_PGO=USE"
    set "FAST_BUILD_NO_LOGS=ON"
    set "FAST_BUILD_DIR=build-play"
    set "FAST_BUILD_NOTE= Logs=OFF"
)
set "FAST_BUILD_LTO=ON"
if /I "%FAST_BUILD_MODE%"=="OFF" set "FAST_BUILD_LTO=OFF"
rem Match the compiler profile to logging: removing log calls changes hot control flow.
set "FAST_BUILD_PROFILE=%CD%/tools/release/local/pgo/yharnam-log.profdata"
if /I "%FAST_BUILD_NO_LOGS%"=="ON" set "FAST_BUILD_PROFILE=%CD%/tools/release/local/pgo/yharnam-nolog.profdata"
if /I "%FAST_BUILD_PGO%"=="USE" if not exist "%FAST_BUILD_PROFILE%" (
    echo Missing matched PGO profile: "%FAST_BUILD_PROFILE%"
    echo Restore or train this profile, or use build_fast.cmd OFF.
    popd
    exit /b 2
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not "%errorlevel%"=="0" goto failed
rem Always configure: a previous OFF or GENERATE build must not silently persist.
cmake -S . -B tools/release/local/%FAST_BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_CXX_FLAGS_RELEASE="/O2 /Ob2 /DNDEBUG" -DENABLE_OPENXR=ON -DENABLE_UPDATER=OFF -DENABLE_DISCORD_RPC=OFF -DBBVR_PGO=%FAST_BUILD_PGO% -DBBVR_PGO_PROFILE="%FAST_BUILD_PROFILE%" -DBBVR_THINLTO=%FAST_BUILD_LTO% -DBBVR_NO_LOGS=%FAST_BUILD_NO_LOGS% "-DBBVR_RELEASE_VERSION=%FAST_BUILD_RELEASE%"
if not "%errorlevel%"=="0" goto failed
cmake --build tools/release/local/%FAST_BUILD_DIR% -j 10
if not "%errorlevel%"=="0" goto failed
rem A playable build includes the verified FSR bridge and all model/shader assets.
call tools\upscalers\build_fsr4.cmd "%CD%\tools\release\local\%FAST_BUILD_DIR%\fsr4"
if not "%errorlevel%"=="0" goto failed
echo BUILD_OK Profile="%FAST_BUILD_PROFILE%" PGO=%FAST_BUILD_PGO% ThinLTO=%FAST_BUILD_LTO%%FAST_BUILD_NOTE% Release=%FAST_BUILD_RELEASE%
popd
exit /b 0
:failed
echo BUILD_FAIL
popd
exit /b 1
