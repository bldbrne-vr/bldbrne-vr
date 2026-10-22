cmake_minimum_required(VERSION 3.20)
project(vr_fsr4_stereo LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(VULKAN_SDK_ROOT "" CACHE PATH "Vulkan SDK with Include and Lib/vulkan-1.lib")
set(provider "${CMAKE_CURRENT_SOURCE_DIR}/../../upstream/fsr-vulkan")
add_library(vr_fsr4_stereo SHARED vr_fsr4_bridge.cpp
    "${provider}/src/ffx_vk_fsr4_v07.c" "${provider}/src/ffx_vk_fsr4_v07_provider.c"
    "${provider}/src/ffx_vk_fsr4_v07_schedule.c" "${provider}/src/ffx_vk_fsr4_v07_assets.c")
target_include_directories(vr_fsr4_stereo PRIVATE "${provider}/include" "${VULKAN_SDK_ROOT}/Include")
target_compile_definitions(vr_fsr4_stereo PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(vr_fsr4_stereo PRIVATE "${VULKAN_SDK_ROOT}/Lib/vulkan-1.lib")
# FAST bit 45 (EvaluatePair): every command the backend records goes through the bridge's
# vrFsr4TapeCmd* functions, which forward to Vulkan unless the command buffer is a bridge tape.
# The pinned source stays byte-identical (IMPORT_FSR.json); only this file's definitions rename
# the calls.
set(vr_fsr4_tape_commands
    BindDescriptorSets BindPipeline ClearColorImage CopyBuffer Dispatch FillBuffer PipelineBarrier)
set(vr_fsr4_tape_definitions)
foreach(name IN LISTS vr_fsr4_tape_commands)
  list(APPEND vr_fsr4_tape_definitions "vkCmd${name}=vrFsr4TapeCmd${name}")
endforeach()
set_source_files_properties("${provider}/src/ffx_vk_fsr4_v07.c" PROPERTIES
    COMPILE_DEFINITIONS "${vr_fsr4_tape_definitions}")
# A tape handle that reached Vulkan through a command missing from the list would crash, so a
# backend update that records anything else stops the build here.
file(READ "${provider}/src/ffx_vk_fsr4_v07.c" vr_fsr4_backend_source)
string(REGEX MATCHALL "vkCmd[A-Za-z0-9]+" vr_fsr4_backend_commands "${vr_fsr4_backend_source}")
list(REMOVE_DUPLICATES vr_fsr4_backend_commands)
foreach(command IN LISTS vr_fsr4_backend_commands)
  string(REGEX REPLACE "^vkCmd" "" name "${command}")
  if(NOT name IN_LIST vr_fsr4_tape_commands)
    message(FATAL_ERROR "The FSR backend calls ${command}, which the bridge tape does not redirect")
  endif()
endforeach()
# The other provider sources are compiled without the renaming, so a tape handle passed to them
# must never reach Vulkan: they may record nothing.
foreach(source ffx_vk_fsr4_v07_provider.c ffx_vk_fsr4_v07_schedule.c ffx_vk_fsr4_v07_assets.c)
  file(READ "${provider}/src/${source}" vr_fsr4_other_source)
  if(vr_fsr4_other_source MATCHES "vkCmd[A-Za-z0-9]+")
    message(FATAL_ERROR "${source} calls ${CMAKE_MATCH_0}, which the bridge tape does not redirect")
  endif()
endforeach()
# Re-run these checks whenever a scanned source changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${provider}/src/ffx_vk_fsr4_v07.c" "${provider}/src/ffx_vk_fsr4_v07_provider.c"
    "${provider}/src/ffx_vk_fsr4_v07_schedule.c" "${provider}/src/ffx_vk_fsr4_v07_assets.c")
