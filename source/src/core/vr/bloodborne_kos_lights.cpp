// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/bloodborne_kos_lights.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <miniz.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_sys/file_override.h"
#include "core/file_sys/fs.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/kos_lights_data.h"
#include "core/vr/kos_lights_icon.h"
#include "core/vr/tonitrus_charge.h"
#include "core/vr/tpf_patch.h"

// Kos Lights in the game's parameter archive and item texts, before the game opens them (an
// eboot-loaded hook); in the same archive, the Tonitrus' idle crackle blanked
// (core/vr/tonitrus_charge.h). A -mods copy of the archive hid the item that
// scripts/build_cigarette_patch.py installs (2026-11-04: "no goods 9000 in the game"); built here
// from whichever file the game loads, it never can. Nothing is written to disk. The reads and
// checks take a few ms at boot; the packing runs on the file override's worker, which an early
// open waits for.
namespace VR::Bloodborne {
namespace {

using namespace VR::KosLights;
using Bytes = std::vector<u8>;

constexpr const char* ParamPath = "/app0/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx";
constexpr const char* Tag = "Kos Lights";

std::optional<Bytes> Unpack(const Bytes& file, VR::TpfPatch::Dcx& dcx) {
    const auto parsed = VR::TpfPatch::ParseDcx(file);
    if (!parsed) {
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

void Serve(const char* path, Bytes original, VR::TpfPatch::Dcx dcx, Bytes data) {
    Core::FileSys::SetFileOverrideDeferred(
        path,
        [original = std::move(original), dcx, data = std::move(data)]() {
            return Pack(original, dcx, data);
        },
        Tag);
}

// The goods row, unless the archive has it, into `bnd`. Returns why it was left as it is ("" when
// added).
std::string AddKosRow(Bytes& bnd) {
    const auto entry = FindBnd4Named(bnd, u"EquipParamGoods.param");
    if (!entry) {
        return "no EquipParamGoods.param in it";
    }
    const auto param = EntryData(bnd, *entry);
    std::string why;
    const auto rows = ParamRows(param, why);
    if (!rows) {
        return why;
    }
    if (HasRow(*rows, Goods)) {
        return "it has goods 9000 already (the patch is installed, or a mod's own item)";
    }
    const auto row = KosRow(param, *rows, why);
    if (!row) {
        return why;
    }
    auto added = AppendRow(param, Goods, *row, why);
    if (!added) {
        return why;
    }
    auto rebuilt = ReplaceBnd4(bnd, *entry, *added);
    if (!rebuilt) {
        return "the archive would pass 4 GB";
    }
    bnd = std::move(*rebuilt);
    return {};
}

// The Tonitrus' idle crackle (core/vr/tonitrus_charge.h): SpEffectVfxParam row 33's effect
// (midstSfxId, the row's first field) blanked in place, only where it is the game's own 7211, and
// only in VR with the effect sites matched: there every update ends the charge's own crackle with
// the charge (bloodborne_held.cpp), which nothing else would. Returns why it was left as it is (""
// when blanked).
std::string BlankIdleCrackle(Bytes& bnd) {
    namespace T = VR::TonitrusCharge;
    if (!TonitrusEffectsReady()) {
        return "kept (no VR, or the effect sites did not match)";
    }
    if (const char* keep = std::getenv("VR_TONITRUS_IDLE");
        keep != nullptr && std::atoi(keep) != 0) {
        return "kept (VR_TONITRUS_IDLE)";
    }
    const auto entry = FindBnd4Named(bnd, u"SpEffectVfxParam.param");
    if (!entry) {
        return "no SpEffectVfxParam.param in it";
    }
    const auto param = EntryData(bnd, *entry);
    std::string why;
    const auto rows = ParamRows(param, why);
    if (!rows) {
        return why;
    }
    for (const auto& r : *rows) {
        if (r.id != T::IdleCrackleVfxRow) {
            continue;
        }
        s32 ffx{};
        if (!ReadAt(param, r.data, ffx)) {
            return "its row 33 is out of range";
        }
        if (ffx != T::IdleCrackleFfx) {
            return fmt::format("its row 33 plays {}, not {}", ffx, T::IdleCrackleFfx);
        }
        constexpr s32 none = -1;
        std::memcpy(bnd.data() + entry->offset + r.data, &none, sizeof(none));
        return {};
    }
    return "no row 33";
}

// The game's parameter archive with our edits, served from memory when one applied (both go into
// the one archive the game opens). Returns what was done, for the log.
std::string PrepareParams(Core::FileSys::MntPoints& mnt) {
    auto file = mnt.ReadFile(ParamPath);
    if (!file) {
        return "not readable";
    }
    VR::TpfPatch::Dcx dcx{};
    auto bnd = Unpack(*file, dcx);
    if (!bnd) {
        return "not a DCX that unpacks";
    }
    const std::string kos = AddKosRow(*bnd);
    const std::string crackle = BlankIdleCrackle(*bnd);
    if (kos.empty() || crackle.empty()) {
        Serve(ParamPath, std::move(*file), dcx, std::move(*bnd));
    }
    return fmt::format("the goods row {}; the Tonitrus' idle crackle {}",
                       kos.empty() ? "served" : "left as it is (" + kos + ")",
                       crackle.empty() ? "hidden" : "left as it is (" + crackle + ")");
}

// The texts, unless they have them (any missing one is added; the rest kept).
std::string PrepareTexts(Core::FileSys::MntPoints& mnt, const std::string& path) {
    auto file = mnt.ReadFile(path);
    if (!file) {
        return "not readable";
    }
    VR::TpfPatch::Dcx dcx{};
    const auto unpacked = Unpack(*file, dcx);
    if (!unpacked) {
        return "not a DCX that unpacks";
    }
    Bytes bnd = std::move(*unpacked);
    int added = 0;
    for (const auto& [id, text] : {std::pair{NamesFmg, Name}, std::pair{InfosFmg, Info},
                                   std::pair{CaptionsFmg, Caption}}) {
        const auto entry = FindBnd4(bnd, id);
        if (!entry) {
            return "no FMG " + std::to_string(id) + " in it";
        }
        auto texts = ParseFmg(EntryData(bnd, *entry));
        if (!texts) {
            return "FMG " + std::to_string(id) + " not understood";
        }
        if (texts->contains(Goods)) {
            continue;
        }
        (*texts)[Goods] = std::u16string{text};
        auto replaced = ReplaceBnd4(bnd, *entry, PackFmg(*texts));
        if (!replaced) {
            return "the archive would pass 4 GB";
        }
        bnd = std::move(*replaced);
        ++added;
    }
    if (added == 0) {
        return "it has the texts already";
    }
    Serve(path.c_str(), std::move(*file), dcx, std::move(bnd));
    return {};
}

// Our packet art as icon file 9000 for our VR card (only our code reads it: the game's menus show
// its own icon 343), in the player's own Monocular icon file.
std::string PrepareCardIcon(Core::FileSys::MntPoints& mnt) {
    static constexpr const char* Template = "/app0/dvdroot_ps4/menu/menu_knowledge_00012.tpf.dcx";
    static constexpr const char* Path = "/app0/dvdroot_ps4/menu/menu_knowledge_09000.tpf.dcx";
    static_assert(VR::Cigarette::CardIcon == 9000);
    auto file = mnt.ReadFile(Template);
    if (!file) {
        return "no icon template (menu_knowledge_00012) to build it from";
    }
    VR::TpfPatch::Dcx dcx{};
    auto tpf = Unpack(*file, dcx);
    if (!tpf) {
        return "the template is not a DCX that unpacks";
    }
    Bytes payload(IconUnpacked);
    mz_ulong length = IconUnpacked;
    if (mz_uncompress(payload.data(), &length, IconPacked, sizeof(IconPacked)) != MZ_OK ||
        length != IconUnpacked) {
        return "our art does not unpack";
    }
    std::string why;
    if (!FillIcon(*tpf, payload, VR::Cigarette::CardIcon, why)) {
        return why;
    }
    Serve(Path, std::move(*file), dcx, std::move(*tpf));
    return {};
}

// The console language's msg folder, as core/vr/bloodborne_assets.cpp picks it, and the other
// region's folder for the same language: the game picks by its own region (the EU game reads
// enggb with the console set to English, 2026-11-04 mock), so both are served.
const char* RegionTwin(const std::string& folder) {
    return folder == "engus"   ? "enggb"
           : folder == "enggb" ? "engus"
           : folder == "spaes" ? "spaar"
           : folder == "spaar" ? "spaes"
           : folder == "porpt" ? "porbr"
           : folder == "porbr" ? "porpt"
                               : nullptr;
}
const char* LanguageFolder(int language) {
    static constexpr const char* Folders[] = {
        "jpnjp", "engus", "frafr", "spaes", "deude", "itait", "nldnl", "porpt", "rusru", "korkr",
        "zhotw", "zhocn", "finfi", "swese", "dandk", "norno", "polpl", "porbr", "enggb", "turtr",
        "spaar"};
    return language >= 0 && language < static_cast<int>(std::size(Folders)) ? Folders[language]
                                                                            : "engus";
}

} // Anonymous namespace

void KosLightsEbootLoaded() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const auto started = std::chrono::steady_clock::now();
    auto& mnt = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string param = PrepareParams(mnt);
    std::string texts;
    // The console language's texts and its other region's.
    std::vector<std::string> folders{LanguageFolder(EmulatorSettings.GetConsoleLanguage())};
    if (const char* twin = RegionTwin(folders.front())) {
        folders.emplace_back(twin);
    }
    for (const auto& folder : folders) {
        const std::string path = "/app0/dvdroot_ps4/msg/" + folder + "/item.msgbnd.dcx";
        const std::string why = PrepareTexts(mnt, path);
        texts += (texts.empty() ? "" : "; ") + folder + ": " + (why.empty() ? "served" : why);
    }
    const std::string card = PrepareCardIcon(mnt);
    if constexpr (Common::Log::Enabled) {
        LOG_INFO(
            Core,
            "VR Kos Lights: params: {}; texts {}; the card's art {} ({:.1f} ms at "
            "boot; packing on the file override's worker)",
            param, texts, card.empty() ? "served" : "not served (" + card + ")",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count());
    }
}

} // namespace VR::Bloodborne
