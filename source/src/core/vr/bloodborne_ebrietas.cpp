// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/bloodborne_ebrietas.h"

#include <cstdlib>
#include <exception>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/file_override.h"
#include "core/file_sys/fs.h"
#include "core/vr/ebrietas_effect_codec.h"

namespace VR::Bloodborne {
namespace {
using namespace VR::EbrietasEffect;

// Bound allocation before reading a mod-controlled file, then bound inflate too.
std::optional<Bytes> ReadPacked(Core::FileSys::MntPoints& mounts, const char* path) {
    auto file = mounts.Open(path);
    if (!file)
        return {};
    const auto size = file->Size();
    if (!size || size > MaxPacked)
        return {};
    Bytes bytes(static_cast<std::size_t>(size));
    if (file->Read(bytes.data(), bytes.size()) != static_cast<s64>(bytes.size()))
        return {};
    return bytes;
}

} // namespace

void EbrietasEffectEbootLoaded() {
    // Diagnostic escape hatch, not a new user setting or a per-frame check.
    const auto* enabled = std::getenv("VR_EBRIETAS_EFFECT_FIX");
    if (enabled && enabled[0] == '0')
        return;
    try {
        auto& mounts = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
        Bytes replacement;
        {
            const auto file = ReadPacked(mounts, SourcePath);
            const auto raw = file ? Unpack(*file) : std::nullopt;
            const auto entry = raw ? Find(*raw, ReplacementName) : std::nullopt;
            if (!entry || !ValidReplacement(View{*raw}.subspan(entry->offset, entry->size))) {
                LOG_WARNING(Core,
                            "Ebrietas effect: source missing or unrecognised; files unchanged");
                return;
            }
            replacement.assign(raw->begin() + entry->offset,
                               raw->begin() + entry->offset + entry->size);
        } // source archive freed before preparing either target
        // Sequential startup work: no worker lifetime or shared clock to coordinate,
        // no gameplay allocations. Only the compressed results remain resident.
        for (const auto& binder : Binders) {
            const auto file = ReadPacked(mounts, binder.path);
            auto fixed = file ? Build(*file, replacement, binder) : Bytes{};
            if (fixed.empty()) {
                LOG_INFO(Core, "Ebrietas effect: {} missing/modified; left unchanged", binder.path);
                continue;
            }
            Core::FileSys::SetFileOverride(binder.path, std::move(fixed), "Ebrietas trail fix");
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "Ebrietas effect: preparation stopped ({})", e.what());
    }
}
} // namespace VR::Bloodborne
