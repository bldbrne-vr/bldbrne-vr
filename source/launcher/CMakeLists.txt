cmake_minimum_required(VERSION 3.24)
project(BloodborneVRLauncher VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "The launcher currently targets Windows.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BBVR_VERSION "${PROJECT_VERSION}" CACHE STRING "Launcher release version")
set(BBVR_RELEASES_URL "https://github.com/bldbrne-vr/bldbrne-vr/releases" CACHE STRING "Release page URL; empty disables online updates")
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
add_library(launcher_ui STATIC
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp ${IMGUI}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
target_include_directories(launcher_ui PUBLIC ${IMGUI} ${IMGUI}/backends ${IMGUI}/misc/cpp)
target_compile_definitions(launcher_ui PRIVATE IMGUI_DISABLE_DEMO_WINDOWS)
add_library(launcher_core STATIC core.cpp)
target_include_directories(launcher_core PUBLIC ${REPO}/src ${REPO}/externals/json/include)
target_compile_definitions(launcher_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(launcher_core PUBLIC shell32 ole32 dxgi)
add_executable(BloodborneVR WIN32 main.cpp)
target_include_directories(BloodborneVR PRIVATE "${VELOPACK_SDK}/include")
target_compile_definitions(BloodborneVR PRIVATE BBVR_VERSION="${BBVR_VERSION}" BBVR_RELEASES_URL="${BBVR_RELEASES_URL}")
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
add_executable(launcher_tests tests.cpp)
target_link_libraries(launcher_tests PRIVATE launcher_core)
if(MSVC)
    target_compile_options(launcher_tests PRIVATE /utf-8 /EHsc)
endif()
add_test(NAME launcher_core COMMAND launcher_tests)
# The headset preset's game patch sits next to the launcher in development builds; packages
# place it next to the bundled runtime.
add_custom_command(TARGET BloodborneVR POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/bbvr_patch.xml" "$<TARGET_FILE_DIR:BloodborneVR>/bbvr_patch.xml")
install(TARGETS BloodborneVR RUNTIME DESTINATION .)
install(FILES bbvr_patch.xml DESTINATION .)

