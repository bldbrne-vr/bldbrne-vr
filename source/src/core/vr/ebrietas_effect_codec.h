// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <miniz.h>
#include "core/vr/ebrietas_effect.h"
#include "core/vr/tpf_patch.h"

namespace VR::EbrietasEffect {
// Only the retail Bloodborne DCX/DFLT container, not arbitrary layouts accepted
// by the shared reader. The two stream sizes are the only variable header bytes.
inline std::optional<TpfPatch::Dcx> Container(View file) {
    constexpr std::array<std::uint8_t, 76> header{
        0x44, 0x43, 0x58, 0,    0,    1,    0,    0,    0,    0,    0,    0x18, 0, 0, 0, 0x24,
        0,    0,    0,    0x44, 0,    0,    0,    0x4c, 0x44, 0x43, 0x53, 0,    0, 0, 0, 0,
        0,    0,    0,    0,    0x44, 0x43, 0x50, 0,    0x44, 0x46, 0x4c, 0x54, 0, 0, 0, 0x20,
        9,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0,
        0,    1,    1,    0,    0x44, 0x43, 0x41, 0,    0,    0,    0,    8};
    if (file.size() < 80 || file.size() > MaxPacked)
        return {};
    for (std::size_t i = 0; i < header.size(); ++i) {
        if (i >= 28 && i < 36)
            continue;
        if (file[i] != header[i])
            return {};
    }
    const TpfPatch::Dcx dcx{24, 76, TpfPatch::BigEndian(file, 28), TpfPatch::BigEndian(file, 32)};
    if (!dcx.unpacked || dcx.unpacked > MaxUnpacked - ReplacementSize - 15 ||
        dcx.packed != file.size() - dcx.stream)
        return {};
    return dcx;
}

inline std::optional<Bytes> Unpack(View file) {
    const auto dcx = Container(file);
    if (!dcx)
        return {};
    Bytes raw;
    raw.reserve(dcx->unpacked + ReplacementSize + 15);
    raw.resize(dcx->unpacked);
    mz_ulong size = raw.size();
    mz_ulong consumed = dcx->packed;
    if (mz_uncompress2(raw.data(), &size, file.data() + dcx->stream, &consumed) != MZ_OK ||
        size != raw.size() || consumed != dcx->packed)
        return {};
    return raw;
}

inline Bytes Build(const Bytes& file, View replacement, const Binder& binder) {
    const auto dcx = Container(file);
    // Refuse other archive sizes before allocating/inflating, and keep the
    // validated container for repacking rather than parsing the header again.
    if (!dcx || dcx->unpacked != binder.size || !ValidReplacement(replacement))
        return {};
    auto raw = Unpack(file);
    if (!raw)
        return {};
    auto changed = Replace(std::move(*raw), replacement, binder);
    if (!changed)
        return {};
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(changed->size()));
    Bytes stream(size);
    if (mz_compress2(stream.data(), &size, changed->data(), changed->size(), 6) != MZ_OK)
        return {};
    stream.resize(size);
    return TpfPatch::RebuildDcx(file, *dcx, stream, static_cast<std::uint32_t>(changed->size()));
}
} // namespace VR::EbrietasEffect
