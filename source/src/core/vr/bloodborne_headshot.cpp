// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
#include "core/vr/bloodborne_headshot.h"
#include "core/vr/gun_aim.h"
#include "core/vr/gun_settings.h"
#include "core/vr/headshot.h"

namespace VR::Bloodborne {
namespace {
template <class T>
T Read(const u8* at) {
    T value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
bool enabled = false, trace = false, dry = false;
std::atomic<unsigned> lines{0};
bool Copy(std::uint64_t address, void* out, size_t size) {
    if (address < 0x10000 || address >= 0x0000800000000000ULL ||
        size > 0x0000800000000000ULL - address)
        return false;
#ifdef _WIN32
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size,
                             &copied) &&
           copied == size;
#else
    return false; // Unknown on hosts without a fault-safe reader; no bonus.
#endif
}
} // namespace
bool HeadshotsWanted() {
    return enabled && (trace || Gun::GetGunSettings().headshot_multiplier > 1);
}
void InitHeadshots(VAddr base, u64 size) {
    // 18612A0: r9d collision part saved at rbp-294 -> result (rbp-290)+1B0.
    constexpr std::array<u8, 12> CollisionPart = {0x8B, 0x85, 0x6C, 0xFD, 0xFF, 0xFF,
                                                  0x89, 0x85, 0x20, 0xFF, 0xFF, 0xFF};
    // 1A2B9F9: same part used to index the target's ragdoll-body metadata.
    constexpr std::array<u8, 11> DamagePart = {0x49, 0x63, 0x8D, 0xB0, 0x01, 0x00,
                                               0x00, 0x48, 0x8B, 0x72, 0x20};
    const auto* module = reinterpret_cast<const u8*>(base);
    enabled = base != 0 && size >= 0x1A2B9F9 + DamagePart.size() &&
              std::memcmp(module + 0x18612A0, CollisionPart.data(), CollisionPart.size()) == 0 &&
              std::memcmp(module + 0x1A2B9F9, DamagePart.data(), DamagePart.size()) == 0;
    const auto flag = [](const char* name) {
        const char* v = std::getenv(name);
        return v && std::strcmp(v, "1") == 0;
    };
    lines.store(0, std::memory_order_relaxed);
    trace = flag("VR_HEADSHOT_LOG");
    dry = flag("VR_HEADSHOT_DRYRUN");
    if (trace)
        std::fprintf(stderr, "VR headshot: layout=%s multiplier=%.2f dry=%d base=%llx\n",
                     enabled ? "verified" : "unavailable",
                     Gun::GetGunSettings().headshot_multiplier, int(dry),
                     static_cast<unsigned long long>(base));
}
float GunHeadshotDamage(const u8* result, const u8* target, float damage) {
    const float multiplier = Gun::GetGunSettings().headshot_multiplier;
    if (!enabled || !result || !target || (!trace && multiplier <= 1))
        return damage;
    const int hand = Read<s32>(result + 0x44), bullet = Read<s32>(result + 0x14C),
              behavior = Read<s32>(result + 0x38);
    if (!Headshot::Eligible(hand, bullet, behavior, damage))
        return damage;
    const auto family = Gun::GunOfBehavior(behavior).family;
    if (family == Gun::Family::None || family == Gun::Family::Sprayer)
        return damage;
    const int index = Read<s32>(result + 0x1B0);
    // Capture the actual failed lookup only in the explicitly enabled diagnostic session.
    // This distinguishes an unreadable pointer from an unexpected enemy skeleton layout.
    struct Sample {
        std::uint64_t address{}, value{};
        size_t size{};
        bool ok{};
    };
    std::array<Sample, 16> samples{};
    size_t sample_count{};
    const bool report = trace && lines.fetch_add(1, std::memory_order_relaxed) < 128;
    const auto reader = [&](std::uint64_t address, void* out, size_t size) {
        const bool ok = Copy(address, out, size);
        if (report && sample_count < samples.size()) {
            auto& sample = samples[sample_count++];
            sample.address = address;
            sample.size = size;
            sample.ok = ok;
            if (ok)
                std::memcpy(&sample.value, out, std::min(size, sizeof(sample.value)));
        }
        return ok;
    };
    const auto part = Headshot::Resolve(reinterpret_cast<std::uint64_t>(target), index, reader);
    const float after = Headshot::Scale(damage, part.region, multiplier);
    if (report) {
        std::fprintf(
            stderr,
            "VR headshot: gun=%s bullet=%d part=%d bone=%s region=%s damage=%.4f->%.4f dry=%d\n",
            Gun::FamilyName(family), bullet, index, part.name.data(),
            part.region == Headshot::Region::Head    ? "head"
            : part.region == Headshot::Region::Other ? "other"
                                                     : "unknown",
            damage, after, int(dry));
        if (part.region == Headshot::Region::Unknown) {
            std::fprintf(stderr, "VR headshot lookup: target=%llx",
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target)));
            for (size_t i = 0; i < sample_count; ++i) {
                const auto& sample = samples[i];
                std::fprintf(stderr, " [%llx/%zu=%llx %s]",
                             static_cast<unsigned long long>(sample.address), sample.size,
                             static_cast<unsigned long long>(sample.value),
                             sample.ok ? "ok" : "unreadable");
            }
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    }
    return dry ? damage : after;
}
} // namespace VR::Bloodborne
