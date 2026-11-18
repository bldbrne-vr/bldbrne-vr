// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

/// A translation away from the thread that owns guest memory (VR_MERGED_EYES' compile thread,
/// vr_merged_eyes.h), from words that thread copied. FlattenExtendedUserdataPass then neither runs
/// its walker (the tables it reads may change or be unmapped meanwhile) nor adds it to the shared
/// walker code: it emits the walker into memory of its own and compares it with `walker`, the one
/// the owner ran. Equal bytes are the same reads into the same flattened dwords, so `flat`, what
/// the owner's walk produced, is this translation's flattened user data. Otherwise it sets
/// `differs`, and TranslateProgram stops after that pass (no later pass may read sharps then).
struct SrtSnapshot {
    std::span<const u32> flat;
    std::span<const u8> walker;
    bool differs{};
};

} // namespace Shader
