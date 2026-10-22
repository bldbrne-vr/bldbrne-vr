@echo off
setlocal
pushd "%~dp0\..\.."
if not exist "tools\upscalers\local\dependencies\fsr4_shaders\opt\pass11-guard.json" (
    echo FSR assets missing. Run fetch_temporal_dependencies.py --provider fsr4, then prepare_fsr4_assets.py.
    goto failed
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 goto failed
if not defined VULKAN_SDK set "VULKAN_SDK=C:\VulkanSDK\1.4.357.0"
rem Optional bbport post-pass rewrite (FAST bit 37) and fenced SPD; a failure keeps the original
rem passes. The script returns at once when its manifest is current (same recipe and hashes).
python tools/upscalers/prepare_fsr4_post_lds.py
if errorlevel 1 (
    echo FSR4 optimized post passes not built; FAST bit 37 keeps the original post pass.
    del /q "tools\upscalers\local\dependencies\fsr4_shaders\opt\post-lds.json" 2>nul
)
cmake -S tools/upscalers/adapted/fsr4_bridge -B tools/upscalers/local/fsr4-bridge-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DVULKAN_SDK_ROOT="%VULKAN_SDK%"
if errorlevel 1 goto failed
cmake --build tools/upscalers/local/fsr4-bridge-build --parallel 2
if errorlevel 1 goto failed
python tools/upscalers/package_fsr4.py
if errorlevel 1 goto failed
if not "%~1"=="" (
    python tools/upscalers/package_fsr4.py --destination "%~1"
    if errorlevel 1 goto failed
)
popd
exit /b 0
:failed
popd
exit /b 1
