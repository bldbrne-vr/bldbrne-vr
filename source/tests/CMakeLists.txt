# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Find or download Google Test
include(FetchContent)
    FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
FetchContent_MakeAvailable(googletest)

set(TEST_TARGETS
    shadps4_settings_test
    shadps4_ngs2_test
    shadps4_gcn_test
)

set(SETTINGS_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    test_emulator_settings.cpp
    # Windows static guest red-zone protection
    test_windows_guest_red_zone_protection_settings.cpp
)

set(NGS2_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/libraries/ngs2/ngs2_impl.cpp

    # Minimal common and kernel support
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/core/libraries/kernel/sync/mutex.cpp
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp

    # Tests
    test_ngs2.cpp
)
set(GCN_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_composite.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_image.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_integer.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_logical.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_select.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_special.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_warp.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/emit_spirv_instructions.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/backend/spirv/spirv_emit_context.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/format.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/decode.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/fetch_shader.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/data_share.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/export.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/translate.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/scalar_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_alu.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/frontend/translate/vector_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/abstract_syntax_list.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/basic_block.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/microinstruction.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/attribute.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/ir_emitter.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/opcodes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/patch.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/program.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ir_passes.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/constant_propagation_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/dead_code_elimination_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/identity_removal_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/shader_info_collection_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/post_order.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/type.h
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.cpp
    ${CMAKE_SOURCE_DIR}/src/shader_recompiler/ir/value.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/pixel_format.h
    ${CMAKE_SOURCE_DIR}/src/video_core/amdgpu/resource.h

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/io_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp
    ${CMAKE_SOURCE_DIR}/src/common/ntapi.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/resource_tracking_pass_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp

    gcn/gcn_test_runner.hpp
    gcn/gcn_test_runner.cpp
    gcn/translator.hpp
    gcn/translator.cpp

    # Tests
    gcn/test_gcn_instructions.cpp
)

add_executable(shadps4_settings_test ${SETTINGS_TEST_SOURCES})
add_executable(shadps4_ngs2_test ${NGS2_TEST_SOURCES})
add_executable(shadps4_gcn_test ${GCN_TEST_SOURCES})

foreach(t ${TEST_TARGETS})
    target_include_directories(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}
    )
    target_compile_features(${t} PRIVATE cxx_std_23)
    target_compile_definitions(${t} PRIVATE BOOST_ASIO_STANDALONE)
endforeach()

target_link_libraries(shadps4_settings_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)
target_link_libraries(shadps4_ngs2_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)
target_link_libraries(shadps4_gcn_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    nlohmann_json::nlohmann_json
    magic_enum::magic_enum
    toml11::toml11
    Boost::headers
    Vulkan::Headers
    sirit
    SDL3::SDL3
    spdlog::spdlog
    half::half
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        foreach(t ${TEST_TARGETS})
            target_compile_options(${t} PRIVATE -fexperimental-library)
        endforeach()
    endif()
endif()

foreach(t ${TEST_TARGETS})
    if (WIN32)
        target_link_libraries(${t} PRIVATE onecore)
        target_compile_definitions(${t} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            NTDDI_VERSION=0x0A000006
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
        )
        if (MSVC)
            target_compile_definitions(${t} PRIVATE
                _CRT_SECURE_NO_WARNINGS
                _CRT_NONSTDC_NO_DEPRECATE
                _SCL_SECURE_NO_WARNINGS
                _TIMESPEC_DEFINED
            )
        endif()
    endif()
endforeach()

include(GoogleTest)

foreach(t ${TEST_TARGETS})
    gtest_discover_tests(${t}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DISCOVERY_TIMEOUT 60
    )
endforeach()

# ====================================================================
# HTTP tests (libSceHttp - URI helpers and status-line parsing)
# ====================================================================
# Self-contained block - mirrors the structure of the targets above so the
# new test does not depend on any earlier foreach iterations.

set(HTTP_TEST_SOURCES
    # Under test
    ${CMAKE_SOURCE_DIR}/src/core/libraries/network/http.cpp
    # Required to link RegisterLib's LIB_FUNCTION calls and the logger's
    # access to EmulatorSettings.
    ${CMAKE_SOURCE_DIR}/src/core/emulator_settings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/emulator_state.cpp

    # Minimal common support
    ${CMAKE_SOURCE_DIR}/src/common/path_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/assert.cpp
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/string_util.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logging/log.cpp

    # Stubs that replace dependencies
    stubs/common_stub.cpp
    stubs/scm_rev_stub.cpp
    stubs/sdl_stub.cpp
    stubs/loader_stub.cpp
    stubs/core_stub.cpp
    stubs/signals_stub.cpp
    stubs/file_sys_stub.cpp
    stubs/kernel_stub.cpp

    # Tests
    network/test_http_uri.cpp
    network/test_http_status_line.cpp
    network/test_http_parse_response_header.cpp
    network/test_http_lifecycle.cpp
)

add_executable(shadps4_http_test ${HTTP_TEST_SOURCES})

list(APPEND TEST_TARGETS shadps4_http_test)

target_include_directories(shadps4_http_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_http_test PRIVATE cxx_std_23)
target_compile_definitions(shadps4_http_test PRIVATE BOOST_ASIO_STANDALONE)

target_link_libraries(shadps4_http_test PRIVATE
    GTest::gtest_main
    fmt::fmt
    magic_enum::magic_enum
    nlohmann_json::nlohmann_json
    toml11::toml11
    SDL3::SDL3
    spdlog::spdlog
)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
    if (LIBCPP)
        target_compile_options(shadps4_http_test PRIVATE -fexperimental-library)
    endif()
endif()

if (WIN32)
    target_link_libraries(shadps4_http_test PRIVATE onecore)
    target_compile_definitions(shadps4_http_test PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        NTDDI_VERSION=0x0A000006
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    if (MSVC)
        target_compile_definitions(shadps4_http_test PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            _SCL_SECURE_NO_WARNINGS
            _TIMESPEC_DEFINED
        )
    endif()
endif()

gtest_discover_tests(shadps4_http_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR melee swing classifier (header-only, no emulator dependencies)
# ====================================================================
# Synthetic swings (min-jerk trajectories at 72/90/120 Hz) through the
# big-swing heavy classifier in src/core/vr/swing_detector.h.

add_executable(shadps4_swing_arc_test test_swing_arc.cpp)

list(APPEND TEST_TARGETS shadps4_swing_arc_test)

target_include_directories(shadps4_swing_arc_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_swing_arc_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_swing_arc_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_swing_arc_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR body facing (header-only, no emulator dependencies)
# ====================================================================
# The hunter's VR heading in src/core/vr/body_facing.h: deadzone, catch-up,
# turns vs recenters, the stand-in lock point.

add_executable(shadps4_body_facing_test test_body_facing.cpp)

list(APPEND TEST_TARGETS shadps4_body_facing_test)

target_include_directories(shadps4_body_facing_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_facing_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_facing_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_facing_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR cinematic flat mode state machine (header-only, no emulator dependencies)
# ====================================================================
# Phase timing, throw/event signals, category toggles, escape hatch, death
# hold and fade brightness in src/core/vr/cinematic_state.h.

add_executable(shadps4_cinematic_test test_cinematic.cpp)

list(APPEND TEST_TARGETS shadps4_cinematic_test)

target_include_directories(shadps4_cinematic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_cinematic_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_cinematic_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_cinematic_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR arm IK core (header-only, no emulator dependencies)
# ====================================================================
# Havok FK rules, the two-bone solver and SolveArm on the c0000 arms (the tests' mannequin,
# tests/c0000_test_skeleton.h)
# (reach, clamping, pole and wrist-driven elbow, twist helpers, blending,
# guards) in src/core/vr/arm_ik.h.

add_executable(shadps4_arm_ik_test test_arm_ik.cpp)

list(APPEND TEST_TARGETS shadps4_arm_ik_test)

target_include_directories(shadps4_arm_ik_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_arm_ik_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_arm_ik_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_arm_ik_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR physical evasion model (header-only, no emulator dependencies)
# ====================================================================
# Body profile, caps, gain, wall limit and its release, the lean budget (hold,
# recharge, drift settle), fades, glitches, the pose-shift subtraction and the
# neck model in src/core/vr/evasion.h.

add_executable(shadps4_evasion_test test_evasion.cpp)

list(APPEND TEST_TARGETS shadps4_evasion_test)

target_include_directories(shadps4_evasion_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_evasion_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_evasion_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_evasion_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR physical evasion hit log geometry (header-only, no emulator dependencies)
# ====================================================================
# Segment distances, swept attack capsules against hurtbox capsules and the
# evaded / traded classification in src/core/vr/evasion_hitgeom.h.

add_executable(shadps4_evasion_hitgeom_test test_evasion_hitgeom.cpp)

list(APPEND TEST_TARGETS shadps4_evasion_hitgeom_test)

target_include_directories(shadps4_evasion_hitgeom_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_evasion_hitgeom_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_evasion_hitgeom_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_evasion_hitgeom_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR dodge i-frame extension (header-only, no emulator dependencies)
# ====================================================================
# The state machine in src/core/vr/iframe_extend.h: extension length at 30 and
# 60 fps, backsteps, jumps and knockdowns, airborne, chained dodges, a skipped
# reset, extra = 0 and jittery frame times.

add_executable(shadps4_iframes_test test_iframes.cpp)

list(APPEND TEST_TARGETS shadps4_iframes_test)

target_include_directories(shadps4_iframes_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_iframes_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_iframes_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_iframes_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR DIBR stereo math (header-only, no emulator dependencies)
# ====================================================================
# Depth-to-disparity formula against the game's projection, the near-field
# knee, the HUD panel shift and the submitted eye offset in
# src/video_core/renderer_vulkan/vr_stereo_warp_math.h.

add_executable(shadps4_dibr_math_test test_dibr_math.cpp)

list(APPEND TEST_TARGETS shadps4_dibr_math_test)

target_include_directories(shadps4_dibr_math_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_dibr_math_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_dibr_math_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_dibr_math_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR torch held up (header-only, no emulator dependencies)
# ====================================================================
# The lift levels, their hysteresis and finding the torch's own flame in
# src/core/vr/torch_lift.h.

add_executable(shadps4_torch_lift_test test_torch_lift.cpp)

list(APPEND TEST_TARGETS shadps4_torch_lift_test)

target_include_directories(shadps4_torch_lift_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_torch_lift_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_torch_lift_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_torch_lift_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR bow (header-only, no emulator dependencies)
# ====================================================================
# Simon's Bowblade's bow in src/core/vr/bow.h: nock, draw, the charge, the
# release's line and the put-back cases.

add_executable(shadps4_bow_test test_bow.cpp)

list(APPEND TEST_TARGETS shadps4_bow_test)

target_include_directories(shadps4_bow_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_bow_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_bow_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_bow_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/trick_weapons.h: trick weapons played by hand (hand_transform, shield_raise): the
# dual blades split and joined, the sheath taken over the shoulder and the sword drawn, Tonitrus
# struck, the Chikage cut, the Bloodletter stabbed, the cooldown, the shield's raise, the wave's aim.

add_executable(shadps4_trick_weapons_test test_trick_weapons.cpp)

list(APPEND TEST_TARGETS shadps4_trick_weapons_test)

target_include_directories(shadps4_trick_weapons_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_trick_weapons_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_trick_weapons_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_trick_weapons_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/solid_weapon.h: solid weapons (weapon_wall): driven into a wall the weapon stops at
# it and slides along it, a slash stops turning at it, a corner, a thin pole or bar on the tip's
# way, the bare hand; pushed too far or starting inside it passes through until clear; two weapons
# crossing are pushed apart; the sweeps an update takes.

add_executable(shadps4_solid_weapon_test test_solid_weapon.cpp)

list(APPEND TEST_TARGETS shadps4_solid_weapon_test)

target_include_directories(shadps4_solid_weapon_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_solid_weapon_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_solid_weapon_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_solid_weapon_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/sparks.h: weapon_wall's sparks: a slow scrape throws none, a faster one a trickle;
# they fly along the scrape and out, fall, cool from white to red and die; a strike sprays by its
# speed; the pool is bounded.

add_executable(shadps4_sparks_test test_sparks.cpp)

list(APPEND TEST_TARGETS shadps4_sparks_test)

target_include_directories(shadps4_sparks_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_sparks_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_sparks_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_sparks_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/blade_sounds.h: the two weapons against each other ring (blade_sounds): a touch
# taps or clinks, a draw along the blade is a sharpening stroke once it has slid, back and forth a
# stroke each way, jiggling is silent, never the same sound twice in a row.

add_executable(shadps4_blade_sounds_test test_blade_sounds.cpp)

list(APPEND TEST_TARGETS shadps4_blade_sounds_test)

target_include_directories(shadps4_blade_sounds_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_blade_sounds_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_blade_sounds_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_blade_sounds_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/grind_audio.h, fsb_sample.h: the blades' grind: silent without its loop, at level 0
# or on a stale frame; louder and brighter with the slide, eased out; the loop's seam crossfaded;
# with VR_FSB_BANK (the player's own sprj_main_eng.fsb) the sparks loop decodes from it.

add_executable(shadps4_grind_audio_test
    test_grind_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/fsb_sample.cpp
)

list(APPEND TEST_TARGETS shadps4_grind_audio_test)

target_include_directories(shadps4_grind_audio_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_grind_audio_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_grind_audio_test PRIVATE GTest::gtest_main LibAtrac9)

gtest_discover_tests(shadps4_grind_audio_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/weapon_wall.h: how a solid weapon's contact feels (weapon_wall): driven into a
# surface it strikes once, sliding along it scrapes, resting is silent, a short gap is the same
# contact, a rattle strikes once, a jump is nothing.

add_executable(shadps4_weapon_wall_test test_weapon_wall.cpp)

list(APPEND TEST_TARGETS shadps4_weapon_wall_test)

target_include_directories(shadps4_weapon_wall_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_weapon_wall_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_weapon_wall_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_weapon_wall_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR interactive visceral (header-only, no emulator dependencies)

# ====================================================================
# The hold before the rip, the learned rip times and the pull detector behind
# visceral_pull in src/core/vr/visceral_pull.h.

add_executable(shadps4_visceral_pull_test test_visceral_pull.cpp)

list(APPEND TEST_TARGETS shadps4_visceral_pull_test)

target_include_directories(shadps4_visceral_pull_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_visceral_pull_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_visceral_pull_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_visceral_pull_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR physical plunge (header-only, no emulator dependencies)
# ====================================================================
# The air tracker behind melee_plunge in src/core/vr/plunge.h: falls, steps,
# the flat leap, the landing grace and the hold limit.

add_executable(shadps4_plunge_test test_plunge.cpp)

list(APPEND TEST_TARGETS shadps4_plunge_test)

target_include_directories(shadps4_plunge_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_plunge_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_plunge_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_plunge_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR eye-facing sprites (header-only, no emulator dependencies)
# ====================================================================
# The rotation VR_BILLBOARD_FACE_EYE emits into the game's camera-facing sprite
# shaders, in src/shader_recompiler/ir/passes/billboard_face_eye.h.

add_executable(shadps4_billboard_face_eye_test test_billboard_face_eye.cpp)

list(APPEND TEST_TARGETS shadps4_billboard_face_eye_test)

target_include_directories(shadps4_billboard_face_eye_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_billboard_face_eye_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_billboard_face_eye_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_billboard_face_eye_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_GPU_MIRROR_FLIP (header-only: scratchpad/dual-eye-perf/
# asymmetric_projection.h): a planar reflection's mirrored camera is told
# from the frame's by its clip plane, its eye frustum upside down puts the
# water's flipped read on the reflection in each eye, its clip plane follows
# each eye, and its pass runs to the next scene camera.
# ====================================================================

add_executable(shadps4_mirror_flip_test test_mirror_flip.cpp)

list(APPEND TEST_TARGETS shadps4_mirror_flip_test)

target_include_directories(shadps4_mirror_flip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_mirror_flip_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_mirror_flip_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_mirror_flip_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR late warp math (header-only, no emulator dependencies)
# ====================================================================
# The epipolar line of a view pixel in the DIBR capture, the camera point
# against the game camera's offsets and the scan (the CPU reference of
# vr_late_reproject.comp) on ray-cast scenes in
# src/video_core/renderer_vulkan/vr_late_warp_math.h.

add_executable(shadps4_late_warp_math_test test_late_warp_math.cpp)

list(APPEND TEST_TARGETS shadps4_late_warp_math_test)

target_include_directories(shadps4_late_warp_math_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_late_warp_math_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_late_warp_math_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_late_warp_math_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the 6DoF room (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/head_room.h: positional_limit_m's sphere above the recentered
# head, a cylinder positional_limit_down_m deep below it (a duck 30 cm deeper,
# a forward-leaning duck followed all the way), the old sphere bit for bit with
# 0, the nearest-point clamp, the floor guard, physical evasion's duck cap
# (DuckCap), and the late warp's CameraPoint.

add_executable(shadps4_head_room_test test_head_room.cpp)

list(APPEND TEST_TARGETS shadps4_head_room_test)

target_include_directories(shadps4_head_room_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_head_room_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_head_room_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_head_room_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: a gap in the camera updates (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/camera_gap.h: after camera_stale_ms without a camera update, a
# hitch (the game camera where it was left) resumes the view with its heading
# and 6DoF origin (a duck through a hitch stays a duck); a load or teleport
# (the follow camera's pivot moved) or a turned game camera takes it over.

add_executable(shadps4_camera_gap_test test_camera_gap.cpp)

list(APPEND TEST_TARGETS shadps4_camera_gap_test)

target_include_directories(shadps4_camera_gap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_camera_gap_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_camera_gap_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_camera_gap_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# vr.ini render_pitch_bias_deg: the vertical FOV with and without the bias,
# the late warp's room at rest, and the late warp undoing the bias exactly, in
# src/core/vr/render_tilt.h (header-only).

add_executable(shadps4_render_tilt_test test_render_tilt.cpp)

list(APPEND TEST_TARGETS shadps4_render_tilt_test)

target_include_directories(shadps4_render_tilt_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_render_tilt_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_render_tilt_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_render_tilt_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR published DIBR pairs waiting to go up (header-only, no emulator dependencies)
# ====================================================================
# vr.ini stereo_adopt_finished: which waiting pair goes up and which pair a new
# frame is copied into while the GPU runs behind, in
# src/video_core/renderer_vulkan/vr_stereo_queue.h.

add_executable(shadps4_stereo_queue_test test_stereo_queue.cpp)

list(APPEND TEST_TARGETS shadps4_stereo_queue_test)

target_include_directories(shadps4_stereo_queue_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_stereo_queue_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_stereo_queue_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_stereo_queue_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR eye image size for the headset (header-only, no emulator dependencies)
# ====================================================================
# vr.ini eye_upscale: the factor (fixed, or auto from the runtime's recommended
# eye size) and its caps, in src/video_core/renderer_vulkan/vr_eye_scale.h.

add_executable(shadps4_eye_scale_test test_eye_scale.cpp)

list(APPEND TEST_TARGETS shadps4_eye_scale_test)

target_include_directories(shadps4_eye_scale_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_eye_scale_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_eye_scale_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_eye_scale_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR headset swapchain sizes against the runtime's limits (header-only)
# ====================================================================
# The centre image's swapchains (the largest swapchain image) and the DIBR eyes (each eye's
# largest image rect), in src/video_core/renderer_vulkan/vr_xr_size.h.

add_executable(shadps4_xr_size_test test_xr_size.cpp)

list(APPEND TEST_TARGETS shadps4_xr_size_test)

target_include_directories(shadps4_xr_size_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_xr_size_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_xr_size_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_xr_size_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the OpenXR runtime's own queue and the mipped swapchains, by runtime (header-only)
# ====================================================================
# VR_XR_OWN_QUEUE, VR_SWAPCHAIN_MIPS and the waits between the two queues, in
# src/video_core/renderer_vulkan/vr_xr_queue_policy.h.

add_executable(shadps4_xr_queue_policy_test test_xr_queue_policy.cpp)

list(APPEND TEST_TARGETS shadps4_xr_queue_policy_test)

target_include_directories(shadps4_xr_queue_policy_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_xr_queue_policy_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_xr_queue_policy_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_xr_queue_policy_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# OpenXR: the base layer and the HUD's cut without the colour scale extension (header-only)
# ====================================================================
# VR_XR_BASE_LAYER, the HUD's hand check and cut, the submitted layers' kinds and the XrTime
# offset, in src/video_core/renderer_vulkan/vr_xr_layers_policy.h.

add_executable(shadps4_xr_layers_policy_test test_xr_layers_policy.cpp)

list(APPEND TEST_TARGETS shadps4_xr_layers_policy_test)

target_include_directories(shadps4_xr_layers_policy_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_xr_layers_policy_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_xr_layers_policy_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_xr_layers_policy_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR eye temporal anti-aliasing: the history's reprojection (header-only)
# ====================================================================
# vr.ini eye_taa: where an eye pixel was in the previous refresh's eye image, in
# src/video_core/renderer_vulkan/vr_eye_taa_math.h (vr_eye_taa.comp's CPU reference).

add_executable(shadps4_eye_taa_test test_eye_taa.cpp test_eye_taa_images.cpp)

list(APPEND TEST_TARGETS shadps4_eye_taa_test)

target_include_directories(shadps4_eye_taa_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_eye_taa_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_eye_taa_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_eye_taa_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR held vial policy (header-only, no emulator dependencies)
# ====================================================================
# The request word, every suppression, the animation class table, the left
# weapon hide masks and the effect slot rotation in src/core/vr/held_item.h.

add_executable(shadps4_held_item_test test_held_item.cpp)

list(APPEND TEST_TARGETS shadps4_held_item_test)

target_include_directories(shadps4_held_item_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_held_item_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_held_item_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_held_item_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR held-item grip (header-only, no emulator dependencies)
# ====================================================================
# The left hand's finger shapes round a held item in src/core/vr/hand_grip.h:
# the game's own fist reproduced, every shape a valid, forward curl.

add_executable(shadps4_hand_grip_test test_hand_grip.cpp test_grip_preview.cpp)

list(APPEND TEST_TARGETS shadps4_hand_grip_test)

target_include_directories(shadps4_hand_grip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_hand_grip_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_hand_grip_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_hand_grip_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Quick items in VR (no emulator, OpenXR or Vulkan)
# ====================================================================
# The goods table and held effects, the launch speed clamps, the release
# velocity, the arc's layout and hover, and the arc's controller (Y, the left
# grip and trigger, throws, uses, the game's own use, the gun, the vial and
# context resets) in src/core/vr/quick_items.h.

add_executable(shadps4_quick_items_test test_quick_items.cpp)

list(APPEND TEST_TARGETS shadps4_quick_items_test)

target_include_directories(shadps4_quick_items_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_items_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_items_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_items_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# left_scheme=2 (the left grip opens the arc, the left trigger takes, throws on
# its release and uses at the pull, the gun off the cards, the belt vial at the
# left hip): src/core/vr/quick_items.h and src/core/vr/belt_vial.h.

add_executable(shadps4_quick_items_left_test test_quick_items_left.cpp test_holsters.cpp)

list(APPEND TEST_TARGETS shadps4_quick_items_left_test)

target_include_directories(shadps4_quick_items_left_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_items_left_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_items_left_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_items_left_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# quick_use_mouth (eat and drink by bringing the item to the mouth: what goes to
# the mouth, the mouth point, the deliberate bring-in, both left-hand layouts):
# src/core/vr/quick_items.h.

add_executable(shadps4_quick_mouth_test test_quick_mouth.cpp)

list(APPEND TEST_TARGETS shadps4_quick_mouth_test)

target_include_directories(shadps4_quick_mouth_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_mouth_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_mouth_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_mouth_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# quick_use_wipe (Fire / Bolt Paper wiped along the blade: which items, the
# paper against the blade, the stroke detector against a tap, a cross-cut and a
# hand too far, both left-hand layouts): src/core/vr/quick_items.h.

add_executable(shadps4_quick_wipe_test test_quick_wipe.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/quick_settings.cpp
)

list(APPEND TEST_TARGETS shadps4_quick_wipe_test)

target_include_directories(shadps4_quick_wipe_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_wipe_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_wipe_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_wipe_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# quick_hold_use (the misc items used by holding them: which items, the rising
# hum, the ring's fill, release / hit / dodge / menu / death cancel, both
# left-hand layouts): src/core/vr/quick_items.h.

add_executable(shadps4_quick_hold_test test_quick_hold.cpp)

list(APPEND TEST_TARGETS shadps4_quick_hold_test)

target_include_directories(shadps4_quick_hold_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_hold_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_hold_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_hold_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# Item gestures (quick_use_crush / quick_use_shake / quick_use_stab: the crush's
# squeeze, the bell's strokes on synthetic paths, a walk / turn / throw / tumble
# never firing, the stab's use, both left-hand layouts): src/core/vr/quick_items.h.

add_executable(shadps4_quick_gestures_test test_quick_gestures.cpp)

list(APPEND TEST_TARGETS shadps4_quick_gestures_test)

target_include_directories(shadps4_quick_gestures_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_gestures_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_gestures_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_gestures_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# Use sounds (quick_use_sounds: the use clips' own sounds, the closest where a clip
# has none, nothing added to the game's own animation): src/core/vr/quick_items.h.

add_executable(shadps4_quick_use_sounds_test test_quick_use_sounds.cpp)

list(APPEND TEST_TARGETS shadps4_quick_use_sounds_test)

target_include_directories(shadps4_quick_use_sounds_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_quick_use_sounds_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_use_sounds_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_quick_use_sounds_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR gun (header-only, no emulator dependencies)
# ====================================================================
# The guns by behaviour, the direct-fire table, the barrel in the grip (against
# the body's own weapon bone), the emitter look-at and spread fans, the start on
# the barrel line, the gentle lock-on assist, the recoil kick and a pull's fate
# in src/core/vr/gun_aim.h.

add_executable(shadps4_gun_reload_test test_gun_reload.cpp test_gun_reload_channel.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/combat_channel.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/gun_settings.cpp)
list(APPEND TEST_TARGETS shadps4_gun_reload_test)
target_include_directories(shadps4_gun_reload_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_gun_reload_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_gun_reload_test PRIVATE GTest::gtest_main fmt::fmt)
gtest_discover_tests(shadps4_gun_reload_test DISCOVERY_TIMEOUT 60)

add_executable(shadps4_gun_reload_assets_test test_gun_reload_assets.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/gun_reload_audio.cpp)
list(APPEND TEST_TARGETS shadps4_gun_reload_assets_test)
target_include_directories(shadps4_gun_reload_assets_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_gun_reload_assets_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_gun_reload_assets_test PRIVATE GTest::gtest_main res::embedded)
gtest_discover_tests(shadps4_gun_reload_assets_test DISCOVERY_TIMEOUT 60)

add_executable(shadps4_gun_aim_test test_gun_aim.cpp)

list(APPEND TEST_TARGETS shadps4_gun_aim_test)

target_include_directories(shadps4_gun_aim_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_gun_aim_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_gun_aim_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_gun_aim_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR physical melee (header-only, no emulator dependencies)
# ====================================================================
# The move table, which row a swing opens, the damage factor's bounds, the
# presenter's swing window (onset, commit, abort, keep-alive, charge), the
# game side's one-attack-per-window latch and the blade's bite in
# src/core/vr/melee_window.h, with the swing classifier on synthetic strokes.

add_executable(shadps4_melee_window_test test_melee_window.cpp)

list(APPEND TEST_TARGETS shadps4_melee_window_test)

target_include_directories(shadps4_melee_window_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_melee_window_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_melee_window_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_melee_window_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR physical viscerals (header-only, no emulator dependencies)
# ====================================================================
# The right hand's thrust (fast, forward along the blade) against swings,
# wind-ups, raising the weapon, steps and waggles; a melee window's thrust
# verdict; the heartbeat cue (src/core/vr/visceral_thrust.h).

add_executable(shadps4_visceral_thrust_test test_visceral_thrust.cpp)

list(APPEND TEST_TARGETS shadps4_visceral_thrust_test)

target_include_directories(shadps4_visceral_thrust_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_visceral_thrust_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_visceral_thrust_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_visceral_thrust_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR controller vibration (header-only, no emulator dependencies)
# ====================================================================
# When the presenter calls the runtime for the controllers' vibration
# (src/core/vr/haptic_output.h): one call per steady tick, the shortest tick,
# ramps and hums without gaps, the floor, the gain, a failed call tried again.

add_executable(shadps4_haptic_output_test test_haptic_output.cpp)

list(APPEND TEST_TARGETS shadps4_haptic_output_test)

target_include_directories(shadps4_haptic_output_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_haptic_output_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_haptic_output_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_haptic_output_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR weapon-only transform (header-only, no emulator dependencies)
# ====================================================================
# The trick weapons' clip table, which clip a squeeze plays (or why L1
# stays), the clip clock, the replayed cues and the squeeze latch in
# src/core/vr/transform.h and src/core/vr/transform_clips.h.

add_executable(shadps4_transform_test test_transform.cpp)

list(APPEND TEST_TARGETS shadps4_transform_test)

target_include_directories(shadps4_transform_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_transform_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_transform_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_transform_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR physical whip (header-only, no emulator dependencies)
# ====================================================================
# The whip form's chain on the hand in src/core/vr/whip_chain.h: the idle
# line at rest, exact links, the payout to the game's reach and back, a
# scripted forehand's lash, a teleporting hand, the bone rotations; the
# Beast Cutter's flick recall (src/core/vr/whip_flick.h); the whips' damage
# pace (src/core/vr/whip_damage.h PaceMeter).

add_executable(shadps4_test_stats_test test_givekit_stats.cpp)
list(APPEND TEST_TARGETS shadps4_test_stats_test)
target_include_directories(shadps4_test_stats_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_test_stats_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_test_stats_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_test_stats_test)

add_executable(shadps4_whip_chain_test test_whip_chain.cpp test_whip_flick.cpp test_whip_pace.cpp)

list(APPEND TEST_TARGETS shadps4_whip_chain_test)

target_include_directories(shadps4_whip_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_whip_chain_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_whip_chain_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_whip_chain_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR overlay meshes, materials, projection and vial animation (no Vulkan / OpenXR)
# ====================================================================
# Every procedural mesh watertight with outward winding and unit normals, the
# vial's size and place in the grip, the reversed-Z projection, the vial
# animator (arm, X again, the game's own vial, timeout warning, injection,
# tracking loss) and the draw-list pass order in
# src/video_core/renderer_vulkan/vr_overlay_meshes.*.

add_executable(shadps4_vr_overlay_test test_vr_overlay.cpp test_apothecary_mesh.cpp test_goods_meshes.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/renderer_vulkan/vr_overlay_meshes.cpp
)

list(APPEND TEST_TARGETS shadps4_vr_overlay_test)

target_include_directories(shadps4_vr_overlay_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vr_overlay_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vr_overlay_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vr_overlay_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR UI layer layout (header-only, no emulator dependencies)
# ====================================================================
# The HUD band's segments (no overlap, in view, readable), the dialog crops and
# the band's lazy follow (dead zones, chase, hard limit, wrap, pitch clamp,
# recenter, origin smoothing) in src/core/vr/ui_layout.h.

add_executable(shadps4_ui_layout_test test_ui_layout.cpp)

list(APPEND TEST_TARGETS shadps4_ui_layout_test)

target_include_directories(shadps4_ui_layout_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_ui_layout_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_ui_layout_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_ui_layout_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR lock-on marker on the UI layer (header-only, no emulator dependencies)
# ====================================================================
# The marker's factors and their packing, the scale about its box's centre, the dimmed effect and
# the marker under the rest of the UI in src/core/vr/ui_lock_marker.h (vr_ui_resolve.comp mirrors
# it).

add_executable(shadps4_ui_lock_marker_test test_ui_lock_marker.cpp)

list(APPEND TEST_TARGETS shadps4_ui_lock_marker_test)

target_include_directories(shadps4_ui_lock_marker_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_ui_lock_marker_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_ui_lock_marker_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_ui_lock_marker_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR status build-ups: the faster drain (header-only, no emulator dependencies)
# ====================================================================
# status_drain_pct in src/core/vr/status_drain.h: +pct % of the game's recovery while a build-up
# falls, never above max, never on the way up, never beasthood, a lost compare-exchange writes
# nothing.

add_executable(shadps4_status_drain_test test_status_drain.cpp)

list(APPEND TEST_TARGETS shadps4_status_drain_test)

target_include_directories(shadps4_status_drain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_status_drain_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_status_drain_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_status_drain_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR status build-up meter as its own HUD piece (header-only, no emulator dependencies)
# ====================================================================
# ui_conditions in src/core/vr/ui_conditions.h: the rows from the game's block, the cut's rect,
# the strip below the atlas's halves (vr_ui_resolve.comp mirrors it), the hold, the piece's place.

add_executable(shadps4_ui_conditions_test test_ui_conditions.cpp)

list(APPEND TEST_TARGETS shadps4_ui_conditions_test)

target_include_directories(shadps4_ui_conditions_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_ui_conditions_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_ui_conditions_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_ui_conditions_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR UI layer: pixels without UI exactly transparent (header-only, no emulator dependencies)
# ====================================================================
# K and W at their clear values to within one 8-bit step (the grey's rounding) read as empty, every
# other pixel resolves as before (src/core/vr/ui_empty_pixel.h; vr_ui_resolve.comp mirrors it).

add_executable(shadps4_ui_empty_pixel_test test_ui_empty_pixel.cpp)

list(APPEND TEST_TARGETS shadps4_ui_empty_pixel_test)

target_include_directories(shadps4_ui_empty_pixel_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_ui_empty_pixel_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_ui_empty_pixel_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_ui_empty_pixel_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR immersive vitals (header-only, no emulator dependencies)
# ====================================================================
# The wrist Blood Gauge's levels, change edges and animation, the low-HP heartbeat's
# timing, the dark edges, the fit to the arm's surface from the game's depth (a
# ray-cast sleeve), the glance and the counter's reading direction in
# src/core/vr/vitals.h.

add_executable(shadps4_vitals_test test_vitals.cpp)

list(APPEND TEST_TARGETS shadps4_vitals_test)

target_include_directories(shadps4_vitals_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vitals_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vitals_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vitals_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR view recenter (header-only, no emulator dependencies)
# ====================================================================
# The tracking-space shift of a recenter (the head becomes the origin and
# forward, the view yaw and the game mapping stay), its composition and the
# fade that hides it in src/core/vr/space_shift.h.

add_executable(shadps4_space_shift_test test_space_shift.cpp)

list(APPEND TEST_TARGETS shadps4_space_shift_test)

target_include_directories(shadps4_space_shift_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_space_shift_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_space_shift_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_space_shift_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR controller bindings per headset (header-only, no emulator dependencies)
# ====================================================================
# The OpenXR suggested bindings of every controller profile in
# src/core/vr/xr_bindings.h: every path valid for its profile and hand per
# the specification, both poses and vibration outputs in every map, the
# Touch map unchanged, no system button, d-pad paths only in d-pad maps.

add_executable(shadps4_xr_bindings_test test_xr_bindings.cpp)

list(APPEND TEST_TARGETS shadps4_xr_bindings_test)

target_include_directories(shadps4_xr_bindings_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_xr_bindings_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_xr_bindings_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_xr_bindings_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The Blood Vial injection's hand hold (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/inject_hold.h: the hunter's hands held where they were at the
# stab for vial_inject_hold_frames of the game's own frames (FreezeFrames: the
# body's pose copies counted, timed without them, capped in a stall), then
# eased back to the controllers with no snap (smoothstep / slerp), ended by a
# recenter, the gun, a swing or lost tracking; and X's
# rule (VialButton): a press arms or puts back the vial, a hold does nothing more.

add_executable(shadps4_inject_hold_test test_inject_hold.cpp)

list(APPEND TEST_TARGETS shadps4_inject_hold_test)

target_include_directories(shadps4_inject_hold_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_inject_hold_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_inject_hold_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_inject_hold_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/root_height_hold.h: the first-person eye's root height through turns in place: a turn
# in place holds the model's offset over the physics root until he moves; stairs, walking and
# jumps follow the model; yaw noise, the yaw wrap, both eyes of a pair, bad input.

add_executable(shadps4_root_height_hold_test test_root_height_hold.cpp)

list(APPEND TEST_TARGETS shadps4_root_height_hold_test)

target_include_directories(shadps4_root_height_hold_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_root_height_hold_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_root_height_hold_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_root_height_hold_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/body_respawn.h: a new player under the VR body (another player object, the same one
# after a load, alive again after HP 0), the settle without writes after it; RestoreGame's checks
# before a stale copy's bones go back to the game (with pose_refresh.h's bitwise check).

add_executable(shadps4_body_respawn_test test_body_respawn.cpp)

list(APPEND TEST_TARGETS shadps4_body_respawn_test)

target_include_directories(shadps4_body_respawn_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_respawn_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_respawn_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_respawn_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/vial_drain.h: the injection's drain (vial_inject_drain): the
# fill curve (a plunger: soft start, steady pour, soft stop), its timing from the start and
# from the published word, how long the vial stays, the resets; and the held
# decision's drained vial (held_item.h FlagDrain: ours only when shown).

add_executable(shadps4_vial_drain_test test_vial_drain.cpp)

list(APPEND TEST_TARGETS shadps4_vial_drain_test)

target_include_directories(shadps4_vial_drain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vial_drain_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vial_drain_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vial_drain_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/vial_needle.h: the needle on the Blood Vial (vial_needle): the stabbing end and
# the axis from the model's vertices, the needle's and the hub's rows from the vial's through any
# pose, the fist in their meshes, the least width on screen, and when it is drawn (exactly while
# the vial is: held_item.h's decision).

add_executable(shadps4_vial_needle_test test_vial_needle.cpp)

list(APPEND TEST_TARGETS shadps4_vial_needle_test)

target_include_directories(shadps4_vial_needle_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vial_needle_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vial_needle_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vial_needle_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/vial_zones.h: the Blood Vial's stab zones (the right hand, wrist,
# forearm, elbow and upper arm from the controller; the body from the chest to
# the upper thighs from the neck pivot; seated the lap) and the gate (a stab is
# the vial hand's own motion into a zone it was just outside of), played at
# 90 Hz: every stab injects once, armed or resting inside, the weapon arm
# swinging onto a still vial hand, the gun's aim pose, walking, turning, a hand
# lowered to the side and one-frame glitches never do.

add_executable(shadps4_vial_zones_test test_vial_zones.cpp)

list(APPEND TEST_TARGETS shadps4_vial_zones_test)

target_include_directories(shadps4_vial_zones_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vial_zones_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vial_zones_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vial_zones_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The left stick click (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/stick_click.h: the sprint jump on the press, the hold modifier
# and its combos, and the tap that toggles the quick-item arc like Y
# (quick_arc_l3) or is L3, carried through the quick-item controller.

add_executable(shadps4_stick_click_test test_stick_click.cpp)

list(APPEND TEST_TARGETS shadps4_stick_click_test)

target_include_directories(shadps4_stick_click_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_stick_click_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_stick_click_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_stick_click_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/y_menu.h: Y as the menu button (y_menu, SteamVR by default): the
# tap is Options, the hold the recenter, combos and sticks cancel; without it
# X and the right stick only make Y's hold a combo (the old Square and D-pad).

add_executable(shadps4_y_menu_test test_y_menu.cpp)

list(APPEND TEST_TARGETS shadps4_y_menu_test)

target_include_directories(shadps4_y_menu_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_y_menu_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_y_menu_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_y_menu_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The left hand's locomotion, left_scheme=2 (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/left_locomotion.h: the sprint latch on the left stick click, A's
# leap / dodge blend with its grace, the second click's leap, the Circle edges
# of the game's jump (the Sequencer), and the Y modifier.

add_executable(shadps4_left_locomotion_test test_left_locomotion.cpp)

list(APPEND TEST_TARGETS shadps4_left_locomotion_test)

target_include_directories(shadps4_left_locomotion_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_left_locomotion_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_left_locomotion_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_left_locomotion_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The A+B chord (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/roll_chord.h: A and B within 70 ms in either order are the roll
# (let go before 1.2 s, or held while the stick is pushed; centred: back) or,
# held 1.2 s still, the grip adjust's A+B; no A or B leaks during a chord; A or
# B alone after the window or as one tap; the bypass and roll_chord=0.

add_executable(shadps4_roll_chord_test test_roll_chord.cpp)

list(APPEND TEST_TARGETS shadps4_roll_chord_test)

target_include_directories(shadps4_roll_chord_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_roll_chord_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_roll_chord_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_roll_chord_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The controls' scheme tables and Move with (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/thumb_layout.h: what each control does in left_scheme 1 / 2 and
# the classic layout, swap_sticks (the sticks, their clicks and the face buttons
# trade hands; triggers, grips and Menu stay), the labels, and the hands read
# into their logical slots driving the stick-click and sprint-latch machines.

add_executable(shadps4_thumb_layout_test test_thumb_layout.cpp)

list(APPEND TEST_TARGETS shadps4_thumb_layout_test)

target_include_directories(shadps4_thumb_layout_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_thumb_layout_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_thumb_layout_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_thumb_layout_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Instant item pickups (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/pickup_gate.h: when a pickup leaves the game's animation out
# (pickup_instant, first-person VR) and the log text of a lot's items.

add_executable(shadps4_pickup_gate_test test_pickup_gate.cpp)

list(APPEND TEST_TARGETS shadps4_pickup_gate_test)

target_include_directories(shadps4_pickup_gate_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pickup_gate_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pickup_gate_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_pickup_gate_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The VR tile in the Options grid (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/gfx_tile.h (Item_0_3 added to ingametop.gfx) and
# src/core/vr/vr_tile_patch.h (the eboot sites and signatures), the icon composited
# at install (src/core/vr/tile_icon_compose.h: our goggles layer over a parchment);
# the tests that need the game's own movie / eboot pass trivially without them
# (BBVR_GAME_DATA_TESTS=ON: the icon composited over the dump's own atlas, miniz).

add_executable(shadps4_vr_tile_test test_vr_tile.cpp)

list(APPEND TEST_TARGETS shadps4_vr_tile_test)

target_include_directories(shadps4_vr_tile_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vr_tile_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vr_tile_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vr_tile_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The game's button icons as the VR controllers' (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/key_glyphs.h: the badges, the BC7 encoder (checked with bc7_decode.h) and the
# atlas pass; on the game's own files when BBVR_KEYGLYPH_TPF / BBVR_KEYGLYPH_FONT are set.

add_executable(shadps4_key_glyphs_test test_key_glyphs.cpp)

list(APPEND TEST_TARGETS shadps4_key_glyphs_test)

target_include_directories(shadps4_key_glyphs_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_key_glyphs_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_key_glyphs_test PRIVATE GTest::gtest_main fmt::fmt)

gtest_discover_tests(shadps4_key_glyphs_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR body solver (header-only, no emulator dependencies)
# ====================================================================
# The VR body solver and the arm gate (src/core/vr/body_solver.h, anim_classes.h): the upper
# body under the eye, the trunk lean, the torso yaw, the animation classes and hard gates.

add_executable(shadps4_body_solver_test test_body_solver.cpp)

list(APPEND TEST_TARGETS shadps4_body_solver_test)

target_include_directories(shadps4_body_solver_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_solver_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_solver_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_solver_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR body split (header-only, no emulator dependencies)
# ====================================================================
# The hunter's face sets in first-person VR (src/video_core/renderer_vulkan/vr_body_split.h, the
# generated vr_body_table.h and vr_hands_table.h): the cutaway, the hidden cape, body_head_shadow
# and body_mode=arms_only (only the hands and forearms in the colour passes).

add_executable(shadps4_body_split_test test_body_split.cpp)

list(APPEND TEST_TARGETS shadps4_body_split_test)

target_include_directories(shadps4_body_split_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_split_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_split_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_split_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR body feedback (header-only, no emulator dependencies)
# ====================================================================
# The VR body's feedback (src/core/vr/body_feedback.h): reach ticks, detached hands.

add_executable(shadps4_body_feedback_test test_body_feedback.cpp)

list(APPEND TEST_TARGETS shadps4_body_feedback_test)

target_include_directories(shadps4_body_feedback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_feedback_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_feedback_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_feedback_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR body frame (header-only, no emulator dependencies)
# ====================================================================
# The VR body's frame records and replays (src/core/vr/body_frame.h, body_presets.h).

add_executable(shadps4_body_frame_test test_body_frame.cpp)

list(APPEND TEST_TARGETS shadps4_body_frame_test)

target_include_directories(shadps4_body_frame_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_frame_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_frame_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_frame_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR lean dodge (header-only, no emulator dependencies)
# ====================================================================
# Physical evasion's lean and the dodge comfort camera's helpers (src/core/vr/lean_dodge.h).

add_executable(shadps4_roomscale_walk_test test_roomscale_walk.cpp)
list(APPEND TEST_TARGETS shadps4_roomscale_walk_test)
target_include_directories(shadps4_roomscale_walk_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_roomscale_walk_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_roomscale_walk_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_roomscale_walk_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

add_executable(shadps4_lean_dodge_test test_lean_dodge.cpp)

list(APPEND TEST_TARGETS shadps4_lean_dodge_test)

target_include_directories(shadps4_lean_dodge_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_lean_dodge_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_lean_dodge_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_lean_dodge_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR motion camera (header-only, no emulator dependencies)
# ====================================================================
# The dodge comfort camera (src/core/vr/motion_camera.h): the body and height springs.

add_executable(shadps4_motion_camera_test test_motion_camera.cpp)

list(APPEND TEST_TARGETS shadps4_motion_camera_test)

target_include_directories(shadps4_motion_camera_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_motion_camera_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_motion_camera_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_motion_camera_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The VR settings page, vr_menu=1 (no emulator dependencies but fmt)
# ====================================================================
# src/core/vr/vr_ini.h (the safe vr.ini writer: only the changed keys, temp
# file + rename, the lock file, the debounced worker), vr_page.h (the rows,
# their values, navigation, Defaults, the pointer), vr_page_geom.h (the ray on
# the page's layer), bc7_decode.h (the game's menu art) and the CPU renderer
# (vr_page_render.cpp, with synthetic art).

add_executable(shadps4_vr_page_test test_vr_page.cpp test_vr_graphics_resolution.cpp test_vr_color.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/vr_page_render.cpp)

list(APPEND TEST_TARGETS shadps4_vr_page_test)

target_include_directories(shadps4_vr_page_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vr_page_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vr_page_test PRIVATE GTest::gtest_main fmt::fmt)

gtest_discover_tests(shadps4_vr_page_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Your height (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/player_body.h: the body scale and the eye height over the floor
# the hunter's eyes map to, the measurement (ticks, median, seated detection,
# failures), the arms' lengthScale in arm_ik.h (1 = the old solve bit for bit),
# the belt's draw zone scaled (belt_vial.h) and the VR page's measuring state.

add_executable(shadps4_player_body_test test_player_body.cpp)

list(APPEND TEST_TARGETS shadps4_player_body_test)

target_include_directories(shadps4_player_body_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_player_body_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_player_body_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_player_body_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Show VR controls, vr_menu=1 (no emulator dependencies but fmt)
# ====================================================================
# src/core/vr/controls_view.h: the callouts from the scheme tables (thumb_layout.h,
# left_scheme 1 / 2, classic, Move with: Right stick) and the bound controller's map
# (xr_bindings.h: Touch, Index, Vive, WMR, simple), the menus' layout, the gestures, the live
# model (lights, the selection, the D-pad, the pointer); controls_render.cpp (synthetic art).

add_executable(shadps4_controls_view_test test_controls_view.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/controls_render.cpp)

list(APPEND TEST_TARGETS shadps4_controls_view_test)

target_include_directories(shadps4_controls_view_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_controls_view_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_controls_view_test PRIVATE GTest::gtest_main fmt::fmt)

gtest_discover_tests(shadps4_controls_view_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The reader-priority shared mutex (header-only)
# ====================================================================
# src/common/shared_first_mutex.h: the memory manager's lock (every stream-buffer upload), the
# guest pthread rwlocks and the rasterizer's mapped ranges. Its atomic fast path keeps the old
# semantics: readers wait only for an active writer, timed waits, no lost wake-up under contention.

add_executable(shadps4_shared_first_mutex_test test_shared_first_mutex.cpp)

list(APPEND TEST_TARGETS shadps4_shared_first_mutex_test)

target_include_directories(shadps4_shared_first_mutex_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_shared_first_mutex_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_shared_first_mutex_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_shared_first_mutex_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The two-handed grip (header-only)
# ====================================================================
# src/core/vr/two_hand_grip.h: the handle zone, the squeeze-edge rule (a held grip sliding onto
# the handle never grabs), the weapon's axis from both hands with the right hand's roll, the left
# hand on the game's own two-handed grip point, the release rules and the two-handed swing bonus.

add_executable(shadps4_two_hand_grip_test test_two_hand_grip.cpp)

list(APPEND TEST_TARGETS shadps4_two_hand_grip_test)

target_include_directories(shadps4_two_hand_grip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_two_hand_grip_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_two_hand_grip_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_two_hand_grip_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/camera_shake.h: the camera shake measurement (VR_SHAKE_LOG, vr_camera_shake): the
# angle between two camera orientations, the game's yaw/pitch convention, roll, off-axis position.

add_executable(shadps4_camera_shake_test test_camera_shake.cpp)

list(APPEND TEST_TARGETS shadps4_camera_shake_test)

target_include_directories(shadps4_camera_shake_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_camera_shake_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_camera_shake_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_camera_shake_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/stagger_speed.h and daze.h: shorter hit staggers (stagger_shorten_ms) and the stagger
# vignette fitted to the displayed field (stagger_vignette_fit, its onset dip).

add_executable(shadps4_stagger_test test_stagger.cpp)

list(APPEND TEST_TARGETS shadps4_stagger_test)

target_include_directories(shadps4_stagger_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_stagger_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_stagger_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_stagger_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/hit_stun.h: the hit-stun vignette (hitstun_vignette): the red hit flash held while a
# blow's on-feet hit reaction refuses swings (the arm gate's clip before its R1 window), the black
# stagger tunnel stepping aside for a light stagger, the hold's timing.

add_executable(shadps4_hitstun_test test_hitstun.cpp)

list(APPEND TEST_TARGETS shadps4_hitstun_test)

target_include_directories(shadps4_hitstun_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_hitstun_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_hitstun_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_hitstun_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# src/core/vr/hit_direction.h: directional hit feedback (hit_vignette_directional,
# hit_haptics_directional): the blow's source, its direction from the view, the flash's wedge and
# the controllers' share.

add_executable(shadps4_hit_direction_test test_hit_direction.cpp)

list(APPEND TEST_TARGETS shadps4_hit_direction_test)

target_include_directories(shadps4_hit_direction_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_hit_direction_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_hit_direction_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_hit_direction_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# quick_music_box_crank (the Tiny Music Box played by its crank: the box's
# geometry, the grab zone, the crank's turn accumulator and ratchet ticks, the
# replay, the restore rule, and the quick items' Controller in both left-hand
# layouts): src/core/vr/music_box.h, src/core/vr/quick_items.h.

add_executable(shadps4_music_box_test test_music_box.cpp)

list(APPEND TEST_TARGETS shadps4_music_box_test)

target_include_directories(shadps4_music_box_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_music_box_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_music_box_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_music_box_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# inframe_hand_sync (the held item and the wrist gauge re-placed from the
# hunter's newest pose into the camera the game draws it with): the view to view
# move of a list's items and depth probe, and which items of the older record's
# list are replaced: src/core/vr/inframe.h.

add_executable(shadps4_inframe_sync_test test_inframe_sync.cpp)

list(APPEND TEST_TARGETS shadps4_inframe_sync_test)

target_include_directories(shadps4_inframe_sync_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_inframe_sync_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_inframe_sync_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_inframe_sync_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# vr.ini aa_mode (header-only)
# ====================================================================
# The game's FXAA draw kept, skipped (the eyes' TAA only) or replaced by SMAA:
# the word parser, the per-draw decision, SMAA's image checks and the CPU mirror
# of its luma edge test, in src/core/vr/aa_mode.h.

add_executable(shadps4_aa_mode_test test_aa_mode.cpp)

list(APPEND TEST_TARGETS shadps4_aa_mode_test)

target_include_directories(shadps4_aa_mode_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_aa_mode_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_aa_mode_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_aa_mode_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# FAST_GPU_SMAA_TRIM: the switch values and FAST_GPU_SMAA_TRIM_VERIFY's texel compare, in
# src/video_core/renderer_vulkan/vr_smaa_trim.h.

add_executable(shadps4_smaa_trim_test test_smaa_trim.cpp)

list(APPEND TEST_TARGETS shadps4_smaa_trim_test)

target_include_directories(shadps4_smaa_trim_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(shadps4_smaa_trim_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_smaa_trim_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_smaa_trim_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# vr.ini aa_mode=taa: temporal AA in the game frame (header-only)
# ====================================================================
# The jitter sequence and its push encoding, the vertex epilogue's rule, the
# reprojection between render cameras, the depth maths, YCoCg clipping and the
# blend, the stable depth, the history's reset guards and a CPU mirror of the
# resolve on a synthetic scene, in src/core/vr/render_taa.h.

add_executable(shadps4_render_taa_test test_render_taa.cpp)

list(APPEND TEST_TARGETS shadps4_render_taa_test)

target_include_directories(shadps4_render_taa_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_render_taa_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_render_taa_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_render_taa_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# vr.ini render_steady (header-only)
# ====================================================================
# The render camera holds through standing still's micro-motion, is pulled at
# the band's edge beyond it and follows real head motion exactly, in
# src/core/vr/render_steady.h.

add_executable(shadps4_render_steady_test test_render_steady.cpp)

list(APPEND TEST_TARGETS shadps4_render_steady_test)

target_include_directories(shadps4_render_steady_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_render_steady_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_render_steady_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_render_steady_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: only the runtime's EXITING closes the emulator, never a session stop or loss (header-only,
# no emulator dependencies) src/core/vr/session_exit.h.
# ====================================================================

add_executable(shadps4_session_exit_test test_session_exit.cpp test_session_pause.cpp)

list(APPEND TEST_TARGETS shadps4_session_exit_test)

target_include_directories(shadps4_session_exit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_session_exit_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_session_exit_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_session_exit_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Windows: a privileged-instruction fault of a torn fetch at a CPU-patch site resumes there
# (header-only, no emulator dependencies) src/core/patch_fault.h.
# ====================================================================

add_executable(shadps4_patch_fault_test test_patch_fault.cpp)

list(APPEND TEST_TARGETS shadps4_patch_fault_test)

target_include_directories(shadps4_patch_fault_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_patch_fault_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_patch_fault_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_patch_fault_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the process's end after a close (header-only, no emulator dependencies)
# src/core/vr/exit_sequence.h.
# ====================================================================

add_executable(shadps4_exit_sequence_test test_exit_sequence.cpp)

list(APPEND TEST_TARGETS shadps4_exit_sequence_test)

target_include_directories(shadps4_exit_sequence_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_exit_sequence_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_exit_sequence_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_exit_sequence_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the vignettes drawn by the late warp (vignette_in_warp)
# src/video_core/renderer_vulkan/vr_vignette_warp.h against the overlay's ring mesh.
# ====================================================================

add_executable(shadps4_vignette_warp_test test_vignette_warp.cpp
    ${CMAKE_SOURCE_DIR}/src/video_core/renderer_vulkan/vr_overlay_meshes.cpp
)

list(APPEND TEST_TARGETS shadps4_vignette_warp_test)

target_include_directories(shadps4_vignette_warp_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vignette_warp_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vignette_warp_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vignette_warp_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the desktop window while the headset shows the game (header-only)
# src/core/vr/desktop_mirror.h.
# ====================================================================

add_executable(shadps4_desktop_mirror_test test_desktop_mirror.cpp)

list(APPEND TEST_TARGETS shadps4_desktop_mirror_test)

target_include_directories(shadps4_desktop_mirror_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_desktop_mirror_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_desktop_mirror_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_desktop_mirror_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The on-disk pipeline cache (header-only: video_core/pipeline_cache_policy.h,
# common/serdes.h): version words, scoped revisions, SPIR-V check, prune rule,
# permutation ids' names and the warm-up's collection of unnamed files, the
# reader that fails instead of crashing on a truncated or corrupted file.
# ====================================================================

add_executable(shadps4_pipeline_cache_policy_test test_pipeline_cache_policy.cpp)

list(APPEND TEST_TARGETS shadps4_pipeline_cache_policy_test)

target_include_directories(shadps4_pipeline_cache_policy_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pipeline_cache_policy_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pipeline_cache_policy_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_pipeline_cache_policy_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Native stereo's per-draw VR values in the push constants (header-only:
# shader_recompiler/vr_push_data.h): the push block's layout, the body
# cutaway decoded exactly as the baked permutations had it, the defaults.
# ====================================================================

add_executable(shadps4_vr_push_data_test test_vr_push_data.cpp)

list(APPEND TEST_TARGETS shadps4_vr_push_data_test)

target_include_directories(shadps4_vr_push_data_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vr_push_data_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vr_push_data_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vr_push_data_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Object-motion permutations in the disk cache (header-only:
# shader_recompiler/runtime_info.h, common/serdes.h): a motion variant's
# runtime info (the flag alone with VrPushData) loads equal from the meta
# and matches every later allocation of the motion buffers.
# ====================================================================

add_executable(shadps4_vr_motion_cache_test test_vr_motion_cache.cpp)

list(APPEND TEST_TARGETS shadps4_vr_motion_cache_test)

target_include_directories(shadps4_vr_motion_cache_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vr_motion_cache_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vr_motion_cache_test PRIVATE GTest::gtest_main fmt::fmt spdlog::spdlog)

gtest_discover_tests(shadps4_vr_motion_cache_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The colour targets in a fragment permutation (header-only:
# shader_recompiler/runtime_info.h, common/serdes.h): only the targets
# the program writes are kept, so draws binding other targets, or other
# formats for them, select one permutation (also after the meta).
# ====================================================================

add_executable(shadps4_fragment_color_buffers_test test_fragment_color_buffers.cpp)

list(APPEND TEST_TARGETS shadps4_fragment_color_buffers_test)

target_include_directories(shadps4_fragment_color_buffers_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_fragment_color_buffers_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_fragment_color_buffers_test PRIVATE GTest::gtest_main fmt::fmt spdlog::spdlog)

gtest_discover_tests(shadps4_fragment_color_buffers_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Fast bit 40's skip-the-draw in native-stereo VR (header-only:
# video_core/renderer_vulkan/vk_pipeline_skip.h): on by default there, the
# vetoes that draw a candidate after all, the age cap, and the lock-free
# table of each target's last view mask.
# ====================================================================

add_executable(shadps4_pipeline_skip_test test_pipeline_skip.cpp)

list(APPEND TEST_TARGETS shadps4_pipeline_skip_test)

target_include_directories(shadps4_pipeline_skip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pipeline_skip_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pipeline_skip_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_pipeline_skip_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_GPU_PASS_MERGE (header-only: video_core/renderer_vulkan/
# vk_pass_merge.h): the VR UI layer's shadow draws after the game's own
# draw while its pass goes on; the depth/stencil write rule, the decision's
# reasons, and a pass model: the same draws per target in the same order,
# never more passes than the old order.
# ====================================================================

add_executable(shadps4_pass_merge_test test_pass_merge.cpp)

list(APPEND TEST_TARGETS shadps4_pass_merge_test)

target_include_directories(shadps4_pass_merge_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pass_merge_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pass_merge_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_pass_merge_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_VIEW_PREDICT (header-only: video_core/renderer_vulkan/
# vk_pipeline_skip.h PredictedViewMask): the view mask Stage A and the
# preparation workers select a native-stereo draw's pipeline with, from the
# recording's mask for its set of targets; as before when nothing is known.
# ====================================================================

add_executable(shadps4_view_predict_test test_view_predict.cpp)

list(APPEND TEST_TARGETS shadps4_view_predict_test)

target_include_directories(shadps4_view_predict_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_view_predict_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_view_predict_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_view_predict_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_FAULT_WINDOW (header-only: video_core/buffer_cache/
# region_definitions.h): a write fault's window first ends as the previous
# order did, in at most one protection change; the verify comparison finds
# no mismatch in any mode and finds a wrong window.
# ====================================================================

add_executable(shadps4_fault_window_test test_fault_window.cpp)

list(APPEND TEST_TARGETS shadps4_fault_window_test)

target_include_directories(shadps4_fault_window_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_fault_window_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_fault_window_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_fault_window_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_LAZY_WAKE and FAST_CPU_IDLE_CLOCK (header-only: scratchpad/
# fast-renderer/byte_ring.h, busy_clock.h): with threads, a sleeper is
# woken once at its target, nobody loses a wakeup under stress in any
# mode, the verify mode finds none and finds dropped ones; the idle
# clock's windows add up to the busy time.
# ====================================================================

add_executable(shadps4_ring_wake_test test_ring_wake.cpp)

list(APPEND TEST_TARGETS shadps4_ring_wake_test)

target_include_directories(shadps4_ring_wake_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_ring_wake_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_ring_wake_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_ring_wake_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_PUSH_SPLIT's held block (header-only: video_core/
# renderer_vulkan/vk_push_split.h): with other pushes, raw borrows,
# control packets and new command buffers mixed in, every draw sees the
# bytes a whole push leaves.
# ====================================================================

add_executable(shadps4_push_split_test test_push_split.cpp)

list(APPEND TEST_TARGETS shadps4_push_split_test)

target_include_directories(shadps4_push_split_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_push_split_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_push_split_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_push_split_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_SPARSE_FAST's area cache (header-only: scratchpad/
# dual-eye-perf/sparse_range_cache.h, used by Core::MemoryManager): the
# recent area answers every slot, only a release that changed no area
# keeps the cache, and copies through it equal the tree walk's bytes
# through mapping changes and guest fence writes, also with readers on
# other threads.
# ====================================================================

add_executable(shadps4_sparse_fast_test test_sparse_fast.cpp)

list(APPEND TEST_TARGETS shadps4_sparse_fast_test)

target_include_directories(shadps4_sparse_fast_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_sparse_fast_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_sparse_fast_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_sparse_fast_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_LRU_TICK (header-only: common/lru_cache.h, modelled on
# TextureCache::GetImage / GetImageTouch): skipping GetImage's LRU touch
# of an image already touched in the GC period keeps the LRU order, its
# ticks and the collected images exactly as touching on every call; the
# verify check accepts every skip and catches a tick set without a touch.
# ====================================================================

add_executable(shadps4_lru_tick_test test_lru_tick.cpp)

list(APPEND TEST_TARGETS shadps4_lru_tick_test)

target_include_directories(shadps4_lru_tick_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(shadps4_lru_tick_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_lru_tick_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_lru_tick_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_GPU_READ_BARRIERS (header-only: video_core/buffer_cache/
# read_barrier_logic.h, modelled on Buffer::GetBarrier): one barrier to
# every read after a write, none for later reads of any kind; writes and
# excluded reads keep the old step; the verify comparison never fires on
# random request sequences and catches a missing, weaker or diverged one.
# ====================================================================

add_executable(shadps4_read_barriers_test test_read_barriers.cpp)

list(APPEND TEST_TARGETS shadps4_read_barriers_test)

target_include_directories(shadps4_read_barriers_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(shadps4_read_barriers_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_read_barriers_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_read_barriers_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_PASS_MEMO (header-only parts: video_core/renderer_vulkan/
# vk_pipeline_skip.h, common/lru_cache.h): TargetViews::Note's repeat
# skip keeps the table exactly as noting every draw (Memo and Verify),
# and a GetImage touch repeated in the period of the draw's first one
# (what FAST_CPU_LRU_TICK cuts short) changes no LRU order or tick.
# ====================================================================

add_executable(shadps4_pass_memo_test test_pass_memo.cpp)

list(APPEND TEST_TARGETS shadps4_pass_memo_test)

target_include_directories(shadps4_pass_memo_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pass_memo_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pass_memo_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_pass_memo_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_GUARD (header-only: scratchpad/fast-renderer/
# constant_write_guard.h, video_core/texture_cache/download_log.h): the
# pending-write guard refuses exactly what a queued write, an unwritten
# signal or a queued unknown writer covers, never more than the old merged
# guard; image downloads after a snapshot are all reported.
# ====================================================================

add_executable(shadps4_constant_guard_test test_constant_guard.cpp)

list(APPEND TEST_TARGETS shadps4_constant_guard_test)

target_include_directories(shadps4_constant_guard_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_constant_guard_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_constant_guard_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_constant_guard_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_FUSED_PAIR's pass (header-only: scratchpad/dual-eye-perf/
# fused_pair.h): random draws paired from the binding's carried handles
# give the writes, infos, created right buffers and stream mirrors of a
# model of NativeStereo::PairDescriptors' registry pass.
# ====================================================================

add_executable(shadps4_fused_pair_test test_fused_pair.cpp)

list(APPEND TEST_TARGETS shadps4_fused_pair_test)

target_include_directories(shadps4_fused_pair_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_fused_pair_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_fused_pair_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_fused_pair_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_SLIM_PACKET's codec pieces (header-only: scratchpad/
# fast-renderer/packet_codec.h): ArrayIf writes the kept values as Array
# would, PacketCursor reads every layout with and without bounds checks
# and finds the verify packet's second form, and a record kept by the
# consumer always equals the producer's when only changes are sent.
# ====================================================================

add_executable(shadps4_slim_packet_test test_slim_packet.cpp)

list(APPEND TEST_TARGETS shadps4_slim_packet_test)

target_include_directories(shadps4_slim_packet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_slim_packet_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_slim_packet_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_slim_packet_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_PUSH_TEMPLATE's slots (header-only: video_core/
# renderer_vulkan/vk_push_template.h): random paired pushes read back
# through their update template from the payload the recorder copies give
# every descriptor of every write; only a push of the same shape matches.
# ====================================================================

add_executable(shadps4_push_template_test test_push_template.cpp)

list(APPEND TEST_TARGETS shadps4_push_template_test)

target_include_directories(shadps4_push_template_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(shadps4_push_template_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_push_template_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_push_template_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# The command processor's per-draw shortcuts (header-only:
# video_core/renderer_vulkan/vk_perf_memo.h, video_core/amdgpu/resource.h):
# the switches' epoch, the spec projection of the bound sharps (equal only
# where StageSpecialization is equal), the byte-keyed render-target memo.
# ====================================================================

add_executable(shadps4_perf_memo_test test_perf_memo.cpp)

list(APPEND TEST_TARGETS shadps4_perf_memo_test)

target_include_directories(shadps4_perf_memo_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_perf_memo_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_perf_memo_test PRIVATE GTest::gtest_main fmt::fmt spdlog::spdlog)

gtest_discover_tests(shadps4_perf_memo_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_TAN_MEMO (header-only: video_core/renderer_vulkan/
# vr_half_tan_memo.h): the eye shift's half-vfov tangent from the memo is
# bit for bit std::tan(0.5f * vfov) over random camera streams and edge
# values (signed zeros, NaN payloads, infinities), never a stale one.
# ====================================================================

add_executable(shadps4_half_tan_memo_test test_half_tan_memo.cpp)

list(APPEND TEST_TARGETS shadps4_half_tan_memo_test)

target_include_directories(shadps4_half_tan_memo_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_half_tan_memo_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_half_tan_memo_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_half_tan_memo_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# FAST_CPU_DRIVER_CALLS' set-write packet (header-only: video_core/
# renderer_vulkan/vk_set_write_codec.h): random descriptor writes of every
# carried type, encoded anywhere in a ring and decoded, give a set exactly
# the caller's descriptors and verify hash; an uncarried write is refused.
# ====================================================================

add_executable(shadps4_set_write_codec_test test_set_write_codec.cpp)

list(APPEND TEST_TARGETS shadps4_set_write_codec_test)

target_include_directories(shadps4_set_write_codec_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_set_write_codec_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_set_write_codec_test PRIVATE GTest::gtest_main Vulkan::Headers)

gtest_discover_tests(shadps4_set_write_codec_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Why a shader or pipeline compiled (header-only:
# video_core/renderer_vulkan/vk_compile_reason.h): a new permutation's
# fields against its program's closest one, a new pipeline key's fields
# against the closest key of its vertex module, and the counters.
# ====================================================================

add_executable(shadps4_compile_reason_test test_compile_reason.cpp)

list(APPEND TEST_TARGETS shadps4_compile_reason_test)

target_include_directories(shadps4_compile_reason_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_compile_reason_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_compile_reason_test PRIVATE GTest::gtest_main fmt::fmt
    spdlog::spdlog Boost::headers Vulkan::Headers xxHash::xxhash)

gtest_discover_tests(shadps4_compile_reason_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: instant ragdoll on a right-hand melee kill (header-only, no emulator dependencies)
# src/core/vr/ragdoll_kill.h, src/core/vr/ragdoll_deaths.h.
# ====================================================================

add_executable(shadps4_ragdoll_kill_test test_ragdoll_kill.cpp)

list(APPEND TEST_TARGETS shadps4_ragdoll_kill_test)

target_include_directories(shadps4_ragdoll_kill_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_ragdoll_kill_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_ragdoll_kill_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_ragdoll_kill_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the body cutaway boost (header-only, no emulator dependencies)
# src/core/vr/body_cut_boost.h: a hit, stagger or kept dodge, or the camera behind or below the
# hunter's eye, fades his near body out more (body_cutaway_boost).
# ====================================================================

add_executable(shadps4_body_cut_boost_test test_body_cut_boost.cpp)

list(APPEND TEST_TARGETS shadps4_body_cut_boost_test)

target_include_directories(shadps4_body_cut_boost_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_body_cut_boost_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_body_cut_boost_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_body_cut_boost_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# VR: the hunter's skeleton read at runtime (header-only, no emulator dependencies)
# src/core/vr/c0000_rig.h: the FLVER / BND4 / HKX readers, the build (FK, dummies, lengths, the
# Havok -> FLVER map), the digests; tests/c0000_test_skeleton.h's mannequin.
# BBVR_GAME_DATA_TESTS=ON: the skeleton tests also run on the hunter's own skeleton read from the
# user's dump (BBVR_GAME_DIR at run time: the folder holding chr/), linked with miniz.
# ====================================================================

add_executable(shadps4_c0000_rig_test test_c0000_rig.cpp)

list(APPEND TEST_TARGETS shadps4_c0000_rig_test)

target_include_directories(shadps4_c0000_rig_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_c0000_rig_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_c0000_rig_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_c0000_rig_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# GPU work between and inside passes: the resync memo and the depth layout (header-only)
# ====================================================================
# vr.ini perf_resync_memo / perf_depth_one_layout: when an image -> buffer resync may be skipped,
# which transitions count as writes, the depth target's view and layout, in
# src/video_core/renderer_vulkan/vr_perf_logic.h.

add_executable(shadps4_vr_perf_logic_test test_vr_perf_logic.cpp)

list(APPEND TEST_TARGETS shadps4_vr_perf_logic_test)

target_include_directories(shadps4_vr_perf_logic_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_vr_perf_logic_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vr_perf_logic_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_vr_perf_logic_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# ====================================================================
# Physical gestures (header-only, no emulator dependencies)
# ====================================================================
# src/core/vr/pose_gestures.h: the settings, the learned list, the pose
# classifier in the head's frame, the recogniser (settle, stillness, grace,
# hold) and the game side's gates and markers (the Brain's hold timer and
# region, the Doll's cooldown, nothing for an unlearned gesture).

add_executable(shadps4_pose_gestures_test test_pose_gestures.cpp)

list(APPEND TEST_TARGETS shadps4_pose_gestures_test)

target_include_directories(shadps4_pose_gestures_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_pose_gestures_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_pose_gestures_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_pose_gestures_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

option(BBVR_GAME_DATA_TESTS "Run the VR skeleton / tile icon / whip tests on the user's game data" OFF)
if(BBVR_GAME_DATA_TESTS)
    foreach(t shadps4_c0000_rig_test shadps4_arm_ik_test shadps4_body_solver_test
              shadps4_body_frame_test shadps4_player_body_test shadps4_hand_grip_test
              shadps4_gun_aim_test shadps4_vial_zones_test shadps4_body_feedback_test
              shadps4_two_hand_grip_test shadps4_space_shift_test shadps4_vr_tile_test
              shadps4_whip_chain_test)
        if(TARGET ${t})
            target_compile_definitions(${t} PRIVATE BBVR_GAME_DATA_TESTS)
            target_link_libraries(${t} PRIVATE miniz)
        endif()
    endforeach()
endif()

# Optional physical weapon switching and health/stamina color feedback.
add_executable(shadps4_shoulder_vital_color_test test_shoulder_vital_color.cpp)
list(APPEND TEST_TARGETS shadps4_shoulder_vital_color_test)
target_include_directories(shadps4_shoulder_vital_color_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_shoulder_vital_color_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_shoulder_vital_color_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_shoulder_vital_color_test)

# Beast Cutter recall event timing and PCM mixing (no emulator/audio device required).
add_executable(shadps4_whip_audio_test test_whip_audio.cpp)
list(APPEND TEST_TARGETS shadps4_whip_audio_test)
target_include_directories(shadps4_whip_audio_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_whip_audio_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_whip_audio_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_whip_audio_test)

# Continuous physical lever interaction (header-only, no game data required).
add_executable(shadps4_physical_lever_test test_physical_lever.cpp)
list(APPEND TEST_TARGETS shadps4_physical_lever_test)
target_include_directories(shadps4_physical_lever_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_physical_lever_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_physical_lever_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_physical_lever_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)

# Windows-only execution tests for the opt-in 1.09 guest detours.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(shadps4_shoulder_equipment_test
        test_shoulder_equipment.cpp
        ${CMAKE_SOURCE_DIR}/src/core/vr/bloodborne_shoulder.cpp
        ${CMAKE_SOURCE_DIR}/src/core/vr/hook_util.cpp)
    list(APPEND TEST_TARGETS shadps4_shoulder_equipment_test)
    target_include_directories(shadps4_shoulder_equipment_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(shadps4_shoulder_equipment_test PRIVATE cxx_std_23)
    target_compile_definitions(shadps4_shoulder_equipment_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(shadps4_shoulder_equipment_test PRIVATE
        GTest::gtest_main fmt::fmt spdlog::spdlog xbyak::xbyak)
    gtest_discover_tests(shadps4_shoulder_equipment_test DISCOVERY_TIMEOUT 60)
    add_executable(shadps4_lever_probe_test
        test_lever_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/core/vr/bloodborne_levers.cpp
        ${CMAKE_SOURCE_DIR}/src/core/vr/hook_util.cpp)
    list(APPEND TEST_TARGETS shadps4_lever_probe_test)
    target_include_directories(shadps4_lever_probe_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(shadps4_lever_probe_test PRIVATE cxx_std_23)
    target_compile_definitions(shadps4_lever_probe_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(shadps4_lever_probe_test PRIVATE
        GTest::gtest_main fmt::fmt spdlog::spdlog xbyak::xbyak)
    gtest_discover_tests(shadps4_lever_probe_test DISCOVERY_TIMEOUT 60)
endif()

add_executable(shadps4_quick_use_audio_test test_quick_use_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/quick_settings.cpp)
list(APPEND TEST_TARGETS shadps4_quick_use_audio_test)
target_include_directories(shadps4_quick_use_audio_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_quick_use_audio_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_quick_use_audio_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_quick_use_audio_test)

# Held liquid motion and volume, without an emulator or graphics device.
add_executable(shadps4_vial_liquid_test test_vial_liquid.cpp)
list(APPEND TEST_TARGETS shadps4_vial_liquid_test)
target_include_directories(shadps4_vial_liquid_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_vial_liquid_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_vial_liquid_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_vial_liquid_test)

# Lamp catalog, destination filtering, navigation and cross-thread request lifetime.
add_executable(shadps4_lamp_travel_test test_lamp_travel.cpp
    ${CMAKE_SOURCE_DIR}/src/core/vr/interaction_module.cpp)
list(APPEND TEST_TARGETS shadps4_lamp_travel_test)
target_include_directories(shadps4_lamp_travel_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_lamp_travel_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_lamp_travel_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_lamp_travel_test DISCOVERY_TIMEOUT 60)

# S1's temporal provider after a run of incomplete frames (FAST_GPU_S1_ASYM): the failure streak
# and the idle retry with its doubling wait, in vr_temporal_failure.h (header-only).
add_executable(shadps4_s1_retry_test test_s1_retry.cpp)
list(APPEND TEST_TARGETS shadps4_s1_retry_test)
target_include_directories(shadps4_s1_retry_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(shadps4_s1_retry_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_s1_retry_test PRIVATE GTest::gtest_main)
gtest_discover_tests(shadps4_s1_retry_test DISCOVERY_TIMEOUT 60)

# ====================================================================
# FAST_CPU_ASYNC_IDLE and FAST_CPU_SIGNAL_AT_REPLAY (header-only:
# video_core/renderer_vulkan/vk_signal_policy.h): a model of the producer,
# the recorder's FIFO replay and the guest signals; every sync point leaves
# no signal unwritten in each mode, and the async retirement decides the
# last outstanding submission as the old path did.
# ====================================================================

add_executable(shadps4_signal_policy_test test_signal_policy.cpp)

list(APPEND TEST_TARGETS shadps4_signal_policy_test)

target_include_directories(shadps4_signal_policy_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}
)
target_compile_features(shadps4_signal_policy_test PRIVATE cxx_std_23)
target_link_libraries(shadps4_signal_policy_test PRIVATE GTest::gtest_main)

gtest_discover_tests(shadps4_signal_policy_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DISCOVERY_TIMEOUT 60
)
