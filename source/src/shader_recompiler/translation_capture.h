// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The startup shader build's recording (video_core/renderer_vulkan/vk_shader_record.cpp): while
// a translation runs on this thread with a capture set, the reads it makes beyond the code and
// the flattened user data are kept as it makes them - the V#s read through user-data pointers
// (Info::ReadUdReg: the vertex attributes, the tessellation constants' buffer) and the
// tessellation constants. The parsed fetch shader is kept by Gcn::ParseFetchShader
// (fetch_shader.h). Null (the default): nothing is kept.

#include <array>
#include <cstring>
#include <vector>

#include "common/types.h"

namespace Shader {

struct TranslationCapture {
    struct Read {
        u32 pointer_index{};
        u32 dword_offset{};
        std::array<u32, 4> words{};
    };
    std::vector<Read> reads;
    std::vector<u8> tess_constants;

    void NoteRead(u32 pointer_index, u32 dword_offset, const void* data) {
        for (const auto& read : reads) {
            if (read.pointer_index == pointer_index && read.dword_offset == dword_offset) {
                return;
            }
        }
        Read read{pointer_index, dword_offset};
        std::memcpy(read.words.data(), data, sizeof(read.words));
        reads.push_back(read);
    }
};

inline thread_local TranslationCapture* g_translation_capture = nullptr;

} // namespace Shader
