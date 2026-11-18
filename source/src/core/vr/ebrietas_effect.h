// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Data-only workaround described in Jgamrot/yharnam-pc#165 (after Nexus mod 41).
// The original 1251720 trail, used by Ebrietas' 1251721 rock effect, can emit forever
// above 30 FPS. Use the player's own 1454436 trail, whose emitters terminate.
// No game code, effect clocks, damage, or VR timing is changed. Only known retail
// binders qualify; a mod's archive is deliberately left alone.
namespace VR::EbrietasEffect {
using Bytes = std::vector<std::uint8_t>;
using View = std::span<const std::uint8_t>;
constexpr std::size_t MaxPacked = 8 * 1024 * 1024;
constexpr std::size_t MaxUnpacked = 32 * 1024 * 1024;
constexpr std::size_t ReplacementSize = 113608;
constexpr std::u16string_view OriginalName = u"f001251720.fxr";
constexpr std::u16string_view ReplacementName = u"f001454436.fxr";
constexpr const char* SourcePath = "/app0/dvdroot_ps4/sfx/frpg_sfxbnd_m36.ffxbnd.dcx";

struct Binder {
    const char* path;
    std::size_t size;
    std::uint64_t before, after;
};
// Size + FNV-1a identify the same input/output bytes checked with SHA-256 by
// upstream. tests/test_ebrietas_effect.cpp also checks local retail fixtures.
constexpr Binder Binders[] = {
    {"/app0/dvdroot_ps4/sfx/frpg_sfxbnd_m24_02.ffxbnd.dcx", 4132752, 0x0451fc8e0fe4f2b4ULL,
     0x83f0d40c79dba703ULL},
    {"/app0/dvdroot_ps4/sfx/frpg_sfxbnd_m29a.ffxbnd.dcx", 19843968, 0x1d28d0681fd8a33bULL,
     0x09760af91fc3d8a4ULL},
};

inline std::uint64_t Fingerprint(View bytes) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const auto b : bytes)
        h = (h ^ b) * 1099511628211ULL;
    return h;
}

template <typename T>
inline T Read(View d, std::size_t at) {
    T value;
    std::memcpy(&value, d.data() + at, sizeof(T));
    return value;
}

struct Entry {
    std::size_t header, offset, size;
};

// Strict, bounded BND4 0x74 reader. Validate every entry before returning a unique
// basename match, including names we don't need. No unbounded string scanning.
inline std::optional<Entry> Find(View d, std::u16string_view name) {
    if (d.size() < 64 || d.size() > MaxUnpacked || std::memcmp(d.data(), "BND4", 4) ||
        d[0x30] != 1 || d[0x31] != 0x74 || Read<std::uint64_t>(d, 0x20) != 36)
        return {};
    const auto count = Read<std::uint32_t>(d, 12);
    if (!count || count > 4096 || count > (d.size() - 64) / 36)
        return {};
    const std::size_t table_end = 64 + count * 36;
    std::optional<Entry> found;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = 64 + i * 36;
        const auto size = Read<std::uint64_t>(d, at + 8);
        const auto offset = Read<std::uint32_t>(d, at + 24);
        const auto start = Read<std::uint32_t>(d, at + 32);
        if (size != Read<std::uint64_t>(d, at + 16) || offset < table_end || offset > d.size() ||
            size > d.size() - offset || start < table_end || start > d.size() - 2 || (start & 1))
            return {};
        auto end = std::size_t{start}, base = end;
        bool terminated = false;
        for (std::size_t chars = 0; chars < 1024 && end <= d.size() - 2; ++chars, end += 2) {
            const auto ch = Read<std::uint16_t>(d, end);
            if (!ch) {
                terminated = true;
                break;
            }
            if (ch == '/' || ch == '\\')
                base = end + 2;
        }
        if (!terminated)
            return {};
        if ((end - base) / 2 != name.size())
            continue;
        bool same = true;
        for (std::size_t c = 0; c < name.size(); ++c)
            same &= Read<std::uint16_t>(d, base + 2 * c) == name[c];
        if (!same)
            continue;
        if (found)
            return {}; // ambiguous archive
        found = Entry{at, offset, static_cast<std::size_t>(size)};
    }
    return found;
}

inline bool ValidReplacement(View d) {
    return d.size() == ReplacementSize && Fingerprint(d) == 0x466ba205a0cd556eULL;
}

// Takes ownership of the unpacked archive so it need not be copied. The caller
// publishes only a fully verified result; rejection never modifies a game file.
inline std::optional<Bytes> Replace(Bytes d, View replacement, const Binder& expected) {
    if (d.size() != expected.size || d.size() > MaxUnpacked - ReplacementSize - 15 ||
        Fingerprint(d) != expected.before || !ValidReplacement(replacement))
        return {};
    const auto e = Find(d, OriginalName);
    if (!e || e->size != 40912 ||
        Fingerprint(View{d}.subspan(e->offset, e->size)) != 0x7ed42930fdae5158ULL)
        return {};
    const auto offset = (d.size() + 15) & ~std::size_t{15};
    d.resize(offset, 0);
    d.insert(d.end(), replacement.begin(), replacement.end());
    const std::uint64_t size = replacement.size();
    const auto offset32 = static_cast<std::uint32_t>(offset);
    std::memcpy(d.data() + e->header + 8, &size, 8);
    std::memcpy(d.data() + e->header + 16, &size, 8);
    std::memcpy(d.data() + e->header + 24, &offset32, 4);
    if (Fingerprint(d) != expected.after)
        return {};
    return d;
}
} // namespace VR::EbrietasEffect
