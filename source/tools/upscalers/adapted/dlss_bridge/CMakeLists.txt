# SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
# SPDX-License-Identifier: MIT
#
# vr_dlss_stereo (libvr_dlss_stereo.so / vr_dlss_stereo.dll) links the NVIDIA DLSS SDK and is built
# separately from the port. Linux: build.sh builds it when DLSS_SDK_ROOT is set. Windows (MSVC):
#   cmake -S gpu/dlss_bridge -B out/dlss-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DDLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> -DVULKAN_INCLUDE=<Vulkan-Headers include>
#   cmake --build out/dlss-bridge --config Release

cmake_minimum_required(VERSION 3.20)
project(vr_dlss_stereo LANGUAGES CXX)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory (Linux: the system headers)")

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The DLSS bridge is x86-64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nvsdk_ngx_s.lib is the static-CRT (/MT) variant: no Visual C++ runtime install needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(vr_dlss_stereo SHARED bbport_dlss_bridge.cpp bbport_dlss_bridge.h)
target_include_directories(vr_dlss_stereo PRIVATE "${DLSS_SDK_ROOT}/include" ${VULKAN_INCLUDE})
if (WIN32)
    target_link_libraries(vr_dlss_stereo PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
    target_compile_definitions(vr_dlss_stereo PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # Only VrDlssGetStereoApi is exported; NGX finds libnvidia-ngx-dlss.so.<version> next to bb-probe.
    set_target_properties(vr_dlss_stereo PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_link_libraries(vr_dlss_stereo PRIVATE "${DLSS_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a"
                                              ${CMAKE_DL_LIBS})
    target_link_options(vr_dlss_stereo PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL)
endif()

set(VR_DLSS_PROJECT_ID "" CACHE STRING "NGX project UUID for this application")
if (NOT VR_DLSS_PROJECT_ID)
    message(FATAL_ERROR "Supply this application's VR_DLSS_PROJECT_ID")
endif()
target_compile_definitions(vr_dlss_stereo PRIVATE VR_DLSS_PROJECT_ID="${VR_DLSS_PROJECT_ID}")

# Optional experimental native neural rendering. OFF leaves the SR bridge unchanged.
option(VR_DLSSNR_CHAIN "Build the experimental native Vulkan DLSS 5 chain" OFF)
if (VR_DLSSNR_CHAIN)
    if (NOT WIN32)
        message(FATAL_ERROR "The DLSSNR loader currently targets Windows")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(NR_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
    find_library(NR_VULKAN vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib" REQUIRED)
    set(NR_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../dlss5_bridge")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/compose_spv.h"
        COMMAND "${NR_GLSLC}" --target-env=vulkan1.2 -O "${NR_SOURCE}/compose.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/compose.spv"
        COMMAND "${Python3_EXECUTABLE}" "${NR_SOURCE}/embed_spirv.py" "${CMAKE_CURRENT_BINARY_DIR}/compose.spv" "${CMAKE_CURRENT_BINARY_DIR}/compose_spv.h"
        DEPENDS "${NR_SOURCE}/compose.comp" "${NR_SOURCE}/embed_spirv.py" VERBATIM)
    target_sources(vr_dlss_stereo PRIVATE "${NR_SOURCE}/chain.cpp" "${CMAKE_CURRENT_BINARY_DIR}/compose_spv.h")
    target_include_directories(vr_dlss_stereo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(vr_dlss_stereo PRIVATE VR_DLSSNR_CHAIN)
    target_link_libraries(vr_dlss_stereo PRIVATE "${NR_VULKAN}")
    add_library(vr_dlssnr_forwarder SHARED "${NR_SOURCE}/forwarder.cpp")
    target_link_libraries(vr_dlssnr_forwarder PRIVATE d3d12)
    set_target_properties(vr_dlssnr_forwarder PROPERTIES OUTPUT_NAME "nvngx.dll_dlssnr" PREFIX "")
endif()

# Optional explicit SR model for isolated comparisons; 0 preserves application hints. A host that
# selects a model at run time (VrDlssSetSrPreset, vr.ini dlss_model) replaces it.
set(VR_DLSS_SR_PRESET "0" CACHE STRING "DLSS Super Resolution model hint (13 = 4.5 Preset M)")
target_compile_definitions(vr_dlss_stereo PRIVATE VR_DLSS_SR_PRESET=${VR_DLSS_SR_PRESET})
