// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/bloodborne_lamp_light.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <miniz.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/file_override.h"
#include "core/file_sys/fs.h"
#include "core/vr/door_knock.h"
#include "core/vr/lamp_light.h"
#include "core/vr/tpf_patch.h"
#include "core/vr/vr_session.h"

// Lighting a lamp at once (core/vr/lamp_light.h) and knocking on a resident's door without the
// hunter's animation (core/vr/door_knock.h): the event scripts that do them, rewritten, served from
// memory in place of the player's own files before the game opens them (the Kos Lights' way,
// core/vr/bloodborne_kos_lights.cpp). Nothing is written to disk; the reads and checks take a few
// ms at boot, the packing runs on the file override's worker. Read at boot: a change of
// vr_interactions reaches lamps and knocks at the next start.
namespace VR::Bloodborne {
namespace {

using Bytes = std::vector<u8>;
constexpr const char* Tag = "lamp light";
// The largest script it unpacks (m29.emevd is 0.6 MB): a header claiming more is not the game's.
constexpr u32 MaxUnpacked = 16u << 20;

std::optional<Bytes> Unpack(const Bytes& file, VR::TpfPatch::Dcx& dcx) {
    const auto parsed = VR::TpfPatch::ParseDcx(file);
    if (!parsed || parsed->unpacked > MaxUnpacked) {
        return std::nullopt;
    }
    dcx = *parsed;
    Bytes out(dcx.unpacked);
    mz_ulong length = dcx.unpacked;
    if (mz_uncompress(out.data(), &length, file.data() + dcx.stream, dcx.packed) != MZ_OK ||
        length != dcx.unpacked) {
        return std::nullopt;
    }
    return out;
}

// The original DCX header around a new stream (empty when packing fails: the game's own file).
Bytes Pack(const Bytes& original, const VR::TpfPatch::Dcx& dcx, const Bytes& data) {
    mz_ulong packed = mz_compressBound(static_cast<mz_ulong>(data.size()));
    Bytes stream(packed);
    if (mz_compress2(stream.data(), &packed, data.data(), static_cast<mz_ulong>(data.size()), 6) !=
        MZ_OK) {
        return {};
    }
    stream.resize(packed);
    return VR::TpfPatch::RebuildDcx(original, dcx, stream, static_cast<u32>(data.size()));
}

// What a rewrite did to one script: nothing to serve when `changed` is 0.
struct Rewritten {
    bool understood{false};
    int changed{0};
    std::string what;
};

Rewritten LampScript(Bytes& script) {
    const auto result = VR::LampLight::Rewrite(script);
    std::string what = std::to_string(result.sounded) + " with the lamp's sound";
    if (result.silenced != 0) {
        what += ", " + std::to_string(result.silenced) + " silent";
    }
    what += ", " + std::to_string(result.unwaited) + " wait(s) removed";
    return {result.understood, result.sounded + result.silenced, std::move(what)};
}

Rewritten KnockScript(Bytes& script) {
    const auto result = VR::DoorKnock::Rewrite(script);
    std::string what = std::to_string(result.unknocked) + " knock(s) without the animation, " +
                       std::to_string(result.sounded) + " with its sound";
    if (result.unwarped != 0) {
        what += ", " + std::to_string(result.unwarped) + " silent";
    }
    return {result.understood, result.unknocked, std::move(what)};
}

// One script: served with `rewrite`'s changes. Returns what was done, for the log.
std::string Prepare(Core::FileSys::MntPoints& mnt, const char* path,
                    Rewritten (*rewrite)(Bytes&), const char* nothing) {
    auto file = mnt.ReadFile(path);
    if (!file) {
        return "not readable";
    }
    VR::TpfPatch::Dcx dcx{};
    auto script = Unpack(*file, dcx);
    if (!script) {
        return "not a DCX that unpacks";
    }
    Rewritten result = rewrite(*script);
    if (!result.understood) {
        return "not an event script it understands: the game's own";
    }
    if (result.changed == 0) {
        return std::string{nothing} + ": the game's own";
    }
    std::string done = std::move(result.what);
    // Packed on the override's worker; its inputs are freed there (the worker's task outlives it).
    Core::FileSys::SetFileOverrideDeferred(
        path,
        [original = std::move(*file), dcx, data = std::move(*script)]() mutable {
            Bytes packed = Pack(original, dcx, data);
            Bytes{}.swap(original);
            Bytes{}.swap(data);
            return packed;
        },
        Tag);
    return done + ": served";
}

} // Anonymous namespace

void LampLightEbootLoaded() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    // Third person keeps the hunter's animation, as chests and doors do (ChestOpen::Decide); a
    // knock then plays in VR (DoorKnock::KeepInVr, core/vr/bloodborne_cinematic.cpp).
    if (!VR::GetSettings().vr_interactions || !VR::IsFirstPerson()) {
        if constexpr (Common::Log::Enabled) {
            LOG_INFO(Core, "VR lamps and knocks: the hunter's animation plays ({})",
                     VR::GetSettings().vr_interactions ? "third person" : "vr_interactions=0");
        }
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto& mnt = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto serve = [&mnt](const auto& scripts, Rewritten (*rewrite)(Bytes&),
                              const char* nothing) {
        std::string what;
        for (const char* path : scripts) {
            std::string done_here;
            try {
                done_here = Prepare(mnt, path, rewrite, nothing);
            } catch (const std::exception& e) { // out of memory, an unreadable file: the game's own
                done_here = std::string{"failed ("} + e.what() + "): the game's own";
            }
            what += (what.empty() ? "" : "; ") + std::string{path} + ": " + done_here;
        }
        return what;
    };
    const std::string lamps = serve(VR::LampLight::Scripts, &LampScript, "no lamp lighting in it");
    const std::string knocks = serve(VR::DoorKnock::Scripts, &KnockScript, "no knock in it");
    if constexpr (Common::Log::Enabled) {
        LOG_INFO(Core, "VR lamps: lighting a lamp at once, without the hunter's animation "
                       "(vr_interactions=1): {}",
                 lamps);
        LOG_INFO(Core, "VR knocks: knocking on a door at once, without the hunter's animation "
                       "(vr_interactions=1): {} ({:.1f} ms at boot for both)",
                 knocks,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           started)
                     .count());
    }
}

} // namespace VR::Bloodborne
