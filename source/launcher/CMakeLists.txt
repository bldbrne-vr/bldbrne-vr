cmake_minimum_required(VERSION 3.24)
project(BloodborneVRLauncher VERSION 0.2.0 LANGUAGES C CXX)
if(NOT WIN32)
    message(FATAL_ERROR "The launcher currently targets Windows.")
endif()
enable_language(RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BBVR_VERSION "${PROJECT_VERSION}" CACHE STRING "Launcher release version")
# The one-time bridge release under the old installer id (main.cpp BBVR_BRIDGE).
option(BBVR_BRIDGE "Build the bridge launcher for the old installer" OFF)
set(VELOPACK_SDK "" CACHE PATH "Unpacked Velopack native SDK")
if(NOT VELOPACK_SDK)
    include(FetchContent)
    FetchContent_Declare(velopack
        URL https://github.com/velopack/velopack/releases/download/1.2.161/velopack_libc_1.2.161.zip
        URL_HASH SHA256=31de8f2cf05a47bd5328a9f98b0c41aec1582156f6075f1058b26941f7f17c20
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(velopack)
    set(VELOPACK_SDK "${velopack_SOURCE_DIR}")
endif()
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(IMGUI "${REPO}/externals/dear_imgui")
add_subdirectory("${REPO}/externals/miniz" miniz EXCLUDE_FROM_ALL)
add_library(launcher_ui STATIC
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp ${IMGUI}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(launcher_ui PUBLIC ${IMGUI} ${IMGUI}/backends ${IMGUI}/misc/cpp)
target_compile_definitions(launcher_ui PRIVATE IMGUI_DISABLE_DEMO_WINDOWS)
add_library(launcher_core STATIC core.cpp benchmark.cpp benchmark_runner.cpp sky_upscale.cpp controls_guide.cpp "${REPO}/src/core/vr/dlss5_file.cpp" "${REPO}/src/core/vr/sky_textures.cpp" "${REPO}/externals/pugixml/src/pugixml.cpp")
target_sources(launcher_core PRIVATE mods.cpp)
target_link_libraries(launcher_core PRIVATE miniz)
target_include_directories(launcher_core PUBLIC ${REPO}/src ${REPO}/externals/json/include ${REPO}/externals/pugixml/src ${REPO}/externals/vulkan-headers/include)
target_sources(launcher_core PRIVATE "${REPO}/src/core/vr/card_memory.cpp")
target_include_directories(launcher_core PRIVATE ${REPO}/externals/stb ${REPO}/externals/xxhash)
target_compile_definitions(launcher_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(launcher_core PUBLIC shell32 ole32 dxgi bcrypt user32 advapi32 wintrust crypt32 version)
# The images are compiled in, not resources (embed_assets.cmake says why); ids from resource.h.
set(BBVR_EMBEDDED_ASSETS
    201=assets/controllers/quest-left.png 202=assets/controllers/quest-right.png
    203=assets/controllers/psvr2-left.png 204=assets/controllers/psvr2-right.png
    205=assets/controllers/frame-left.png 206=assets/controllers/frame-right.png
    207=assets/controllers/index-left.png 208=assets/controllers/index-right.png
    220=assets/theme/yharnam.jpg 221=assets/theme/leather.png 222=assets/theme/frame.png
    223=assets/theme/button-frame.png 224=assets/theme/launch-plaque.png
    225=assets/theme/cabinet-frame.png 226=assets/theme/hardware.png
    227=assets/theme/scrollbar-eye.png)
set(embedded_asset_files "")
foreach(entry IN LISTS BBVR_EMBEDDED_ASSETS)
    string(REGEX REPLACE "^[0-9]+=" "" path "${entry}")
    list(APPEND embedded_asset_files "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
endforeach()
string(REPLACE ";" "," embedded_assets_arg "${BBVR_EMBEDDED_ASSETS}")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.cpp"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.cpp"
        "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DASSETS=${embedded_assets_arg}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.cmake" ${embedded_asset_files}
    COMMENT "Embedding the launcher's images" VERBATIM)
add_executable(BloodborneVR WIN32 main.cpp launcher.rc "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.cpp")
target_include_directories(BloodborneVR PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties(launcher.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon.ico")
# The launcher ships as Bldbrne-VR.exe (2026-10-17: no game name in the product's own files).
set_target_properties(BloodborneVR PROPERTIES OUTPUT_NAME "Bldbrne-VR")
target_include_directories(BloodborneVR PRIVATE "${VELOPACK_SDK}/include")
target_compile_definitions(BloodborneVR PRIVATE BBVR_VERSION="${BBVR_VERSION}")
# The version resource (launcher.rc) carries the numeric version and the copyright notice.
string(REPLACE "." ";" bbvr_version_parts "${BBVR_VERSION}")
list(GET bbvr_version_parts 0 bbvr_version_major)
list(GET bbvr_version_parts 1 bbvr_version_minor)
list(GET bbvr_version_parts 2 bbvr_version_patch)
target_compile_definitions(BloodborneVR PRIVATE BBVR_VERSION_MAJOR=${bbvr_version_major} BBVR_VERSION_MINOR=${bbvr_version_minor} BBVR_VERSION_PATCH=${bbvr_version_patch})
if(BBVR_BRIDGE)
    target_compile_definitions(BloodborneVR PRIVATE BBVR_BRIDGE=1)
    target_link_libraries(BloodborneVR PRIVATE urlmon wininet)
endif()
target_link_libraries(BloodborneVR PRIVATE launcher_core launcher_ui
    "${VELOPACK_SDK}/lib-static/velopack_libc_win_x64_msvc.lib"
    psapi d3d11 dxgi d3dcompiler dwmapi windowscodecs comdlg32
    bcrypt crypt32 ws2_32 userenv ntdll secur32 netapi32 rpcrt4 version winhttp wintrust shlwapi)
if(MSVC)
    foreach(target launcher_core launcher_ui BloodborneVR)
        target_compile_options(${target} PRIVATE /utf-8 /EHsc)
    endforeach()
    target_link_options(BloodborneVR PRIVATE /OPT:REF /OPT:ICF)
endif()
enable_testing()
add_executable(mod_tests mod_tests.cpp)
target_link_libraries(mod_tests PRIVATE launcher_core miniz)
add_test(NAME launcher_mods COMMAND mod_tests)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests.cpp")
    add_executable(launcher_tests tests.cpp)
    target_link_libraries(launcher_tests PRIVATE launcher_core)
    if(MSVC)
        target_compile_options(launcher_tests PRIVATE /utf-8 /EHsc)
endif()
# The headset preset's game patch sits next to the launcher in development builds; packages
# place it next to the bundled runtime.
add_custom_command(TARGET BloodborneVR POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/bbvr_patch.xml" "$<TARGET_FILE_DIR:BloodborneVR>/bbvr_patch.xml")
install(TARGETS BloodborneVR RUNTIME DESTINATION .)
install(FILES bbvr_patch.xml DESTINATION .)
# Sharper skies (sky_upscale.h): Real-ESRGAN's official Windows build (realesrgan-ncnn-vulkan,
# MIT; Real-ESRGAN and its model BSD-3-Clause; ncnn BSD-3-Clause), only the exe, its OpenMP
# runtime and the realesrgan-x4plus model, in realesrgan/ next to the launcher.
include(FetchContent)
FetchContent_Declare(realesrgan
    URL https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip
    URL_HASH SHA256=abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(realesrgan)
foreach(file realesrgan-ncnn-vulkan.exe vcomp140.dll
        models/realesrgan-x4plus.param models/realesrgan-x4plus.bin)
    add_custom_command(TARGET BloodborneVR POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${realesrgan_SOURCE_DIR}/${file}" "$<TARGET_FILE_DIR:BloodborneVR>/realesrgan/${file}")
endforeach()


# Exercise the shipped patch template in launcher regression tests.
add_custom_command(TARGET launcher_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/bbvr_patch.xml" "$<TARGET_FILE_DIR:launcher_tests>/bbvr_patch.xml")

add_executable(benchmark_tests benchmark_tests.cpp)
target_link_libraries(benchmark_tests PRIVATE launcher_core)
target_compile_definitions(benchmark_tests PRIVATE BBVR_BENCHMARK_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_test(NAME launcher_benchmark COMMAND benchmark_tests)
add_executable(performance_search_tests performance_search_tests.cpp)
target_link_libraries(performance_search_tests PRIVATE launcher_core)
add_dependencies(benchmark_tests performance_search_tests)
add_test(NAME launcher_performance_search COMMAND performance_search_tests)

# Developer end-to-end driver for the same runner used by the UI; not installed.
add_executable(benchmark_cli benchmark_cli.cpp)
target_link_libraries(benchmark_cli PRIVATE launcher_core)
# The same for sharper skies (sky_upscale.h); not installed.
add_executable(sky_cli sky_cli.cpp)
target_link_libraries(sky_cli PRIVATE launcher_core)
