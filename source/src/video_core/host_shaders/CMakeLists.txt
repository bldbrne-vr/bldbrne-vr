# SPDX-FileCopyrightText: Copyright 2023 Citra Emulator Project
# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: GPL-2.0-or-later

set(SHADER_FILES
    detilers/display_micro_64bpp.comp
    detilers/macro_32bpp.comp
    detilers/macro_64bpp.comp
    detilers/macro_8bpp.comp
    detilers/micro_128bpp.comp
    detilers/micro_16bpp.comp
    detilers/micro_32bpp.comp
    detilers/micro_64bpp.comp
    detilers/micro_8bpp.comp
    color_to_ms_depth.frag
    ms_image_blit.frag
    fault_buffer_process.comp
    fs_tri.vert
    fsr.comp
    post_process.frag
    tiling.comp
    vr_dibr_prep.comp
    vr_dibr_warp.comp
    vr_eye_taa.comp
    vr_eye_upscale.comp
    vr_inframe.frag
    vr_inframe.vert
    vr_late_classify.comp
    vr_late_reproject.comp
    vr_overlay.frag
    vr_overlay.vert
    vr_smaa.comp
    vr_temporal_motion.comp
    vr_temporal_merge.comp
    vr_temporal_prepare.comp
    vr_render_taa.comp
    vr_ui_compose.vert
    vr_ui_compose.frag
    vr_ui_resolve.comp
)

set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(HOST_SHADERS_INCLUDE ${SHADER_INCLUDE} PARENT_SCOPE)

set(INPUT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/source_shader.h.in)
set(HEADER_GENERATOR ${CMAKE_CURRENT_SOURCE_DIR}/StringShaderHeader.cmake)

foreach(FILENAME IN ITEMS ${SHADER_FILES})
    string(REPLACE "." "_" SHADER_NAME ${FILENAME})
    set(SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME})
    set(SOURCE_HEADER_FILE ${SHADER_DIR}/${SHADER_NAME}.h)
    set(COLOR_DEPENDENCY)
    if(FILENAME STREQUAL "post_process.frag" OR FILENAME STREQUAL "vr_eye_upscale.comp"
       OR FILENAME STREQUAL "vr_dibr_warp.comp")
        set(COLOR_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/vr_color.glsl)
    endif()
    add_custom_command(
        OUTPUT
            ${SOURCE_HEADER_FILE}
        COMMAND
            ${CMAKE_COMMAND} -P ${HEADER_GENERATOR} ${SOURCE_FILE} ${SOURCE_HEADER_FILE} ${INPUT_FILE}
        MAIN_DEPENDENCY
            ${SOURCE_FILE}
        DEPENDS
            ${INPUT_FILE}
            ${COLOR_DEPENDENCY}
            # HEADER_GENERATOR should be included here but msbuild seems to assume it's always modified
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} ${SOURCE_HEADER_FILE})
endforeach()

set(SHADER_SOURCES ${SHADER_FILES})
list(APPEND SHADER_SOURCES ${GLSL_INCLUDES})

add_custom_target(host_shaders
    DEPENDS
        ${SHADER_HEADERS}
    SOURCES
        ${SHADER_SOURCES}
)
