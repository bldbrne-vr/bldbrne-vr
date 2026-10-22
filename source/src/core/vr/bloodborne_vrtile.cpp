// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <miniz.h>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/vr/bloodborne_assets.h"
#include "core/file_sys/file_override.h"
#include "core/file_sys/fs.h"
#include "core/vr/bloodborne_vrtile.h"
#include "core/vr/gfx_area_name.h"
#include "core/vr/gfx_talk_window.h"
#include "core/vr/gfx_tile.h"
#include "core/vr/gfx_title.h"
#include "core/vr/hook_util.h"
#include "core/vr/key_glyphs.h"
#include "core/vr/near_code.h"
#include "core/vr/tile_icon_compose.h"
#include "core/vr/tpf_patch.h"
#include "core/vr/vr_page_runtime.h"
#include "core/vr/vr_session.h"
#include "core/vr/vr_tile_patch.h"

namespace VR::Bloodborne {

namespace {

using namespace VR::TilePatch;
using Hook::Plausible;
using Hook::Read;

// The game's Options movie, as the retail v1.09 game ships it (the only one the tile is added to:
// a mod's own movie, from -mods or a patch, is left as it is).
constexpr const char* MoviePath = "/app0/dvdroot_ps4/menu/ingametop.gfx";
constexpr u64 MovieSize = 11248;
constexpr u64 MovieFnv = 0x501d498bcf9fb5a8ull;

u8* g_base = nullptr;
u64 g_size = 0;
bool g_probe = false;
std::atomic<bool> g_installed{false};
std::atomic<u32> g_presses{0};
std::unique_ptr<Xbyak::CodeGenerator> g_probe_code;

// The probe's state: the live controller (set by its refresh, dropped when the menu closes) and
// the last cursor logged.
std::atomic<u8*> g_controller{nullptr};
std::mutex g_log_mutex;
u8* g_logged_controller = nullptr;
s32 g_logged_cursor = INT_MIN;
u32 g_probe_logs = 0;
constexpr u32 MaxProbeLogs = 400;

const char* CellName(s32 cell) {
    switch (cell) {
    case 0:
        return "Inventory";
    case 1:
        return "Stats";
    case 2:
        return "System";
    case 3:
        return "VR";
    default:
        break;
    }
    if (cell >= 6 && cell <= 9) {
        return "Arms";
    }
    if (cell >= 12 && cell <= 15) {
        return "Attire";
    }
    if (cell >= 18 && cell <= 23) {
        return "Quick items";
    }
    return "empty";
}

// One line whenever the controller or its selected cell changes.
void LogCursor(u8* controller, const char* source) {
    const s32 count = Read<s32>(controller + OffsetCellCount);
    const s32 selected = Read<s32>(controller + OffsetSelected);
    const s32 override_cell = Read<s32>(controller + OffsetOverride);
    const s32 columns = Read<s32>(controller + OffsetColumns);
    const s32 cell = override_cell >= 0 ? override_cell : selected; // as the game's lambdas
    std::scoped_lock lk{g_log_mutex};
    if (controller == g_logged_controller && cell == g_logged_cursor) {
        return;
    }
    const bool fresh = controller != g_logged_controller;
    g_logged_controller = controller;
    g_logged_cursor = cell;
    if (g_probe_logs >= MaxProbeLogs) {
        return;
    }
    ++g_probe_logs;
    LOG_INFO(Core,
             "VR tile probe: cursor {} ({}, row {} col {}): +0xAE0 cells {}, +0xAE4 selected {}, "
             "+0xAE8 override {}, +0xAEC columns {} [{}{}]",
             cell, CellName(cell), columns > 0 ? cell / columns : -1,
             columns > 0 ? cell % columns : -1, count, selected, override_cell, columns, source,
             fresh ? fmt::format(", controller {}", fmt::ptr(controller)) : std::string{});
}

// The IngameTop controller's refresh (vtable slot 3), on the game's menu thread: rdi = the
// controller. Read only.
void PS4_SYSV_ABI RefreshProbe(u8* controller, u8* /*saved_ymm*/) {
    if (!Plausible(controller) || Read<u8*>(controller) != g_base + ControllerVtableRva) {
        return;
    }
    g_controller.store(controller, std::memory_order_release);
    LogCursor(controller, "refresh");
}

void InstallProbe() {
    u8* const site = g_base + RefreshRva;
    if (!CheckRefreshSite(g_base, g_size)) {
        LOG_INFO(Core, "VR tile probe: refresh 0x1BE0220 not found ({}); cursor not logged",
                 g_size >= MinModuleSize ? Hook::HexBytes(site, RefreshSig.size())
                                         : std::string{"eboot too small"});
        return;
    }
    if (!Hook::MakeWritable(site, RefreshSig.size())) {
        LOG_WARNING(Core, "VR tile probe: could not make the refresh writable");
        return;
    }
    // Replayed: the 9 position-independent prologue bytes, then `call 0x1BE8CF0` as an absolute
    // call (call [rip+2]; jmp +8; dq target), then back to site + 14.
    std::array<u8, RefreshPiLength + 16> replay{};
    std::memcpy(replay.data(), RefreshSig.data(), RefreshPiLength);
    const std::array<u8, 8> call_abs = {0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08};
    std::memcpy(replay.data() + RefreshPiLength, call_abs.data(), call_abs.size());
    const u64 target = reinterpret_cast<u64>(g_base + RefreshCallRva);
    std::memcpy(replay.data() + RefreshPiLength + 8, &target, sizeof(target));
    try {
        g_probe_code = std::make_unique<Xbyak::CodeGenerator>(512);
        void* const stub = Hook::EmitHostCallStub(
            *g_probe_code, reinterpret_cast<void*>(&RefreshProbe), replay.data(), replay.size(),
            reinterpret_cast<u64>(site + RefreshSig.size()), Xbyak::util::rdi);
        Hook::WriteJump(site, RefreshSig.size(), stub);
    } catch (const Xbyak::Error& e) {
        LOG_WARNING(Core, "VR tile probe: stub not generated ({})", e.what());
        return;
    }
    g_probe = true;
    LOG_INFO(Core, "VR tile probe: installed at the grid's refresh {:#x} (the Options grid's cursor "
                   "is logged as it moves)",
             RefreshRva);
}

// A on the VR tile: the grid calls this through its functor (0x1B8FC60) as `handler(out, arg)`;
// `out` receives the menu job to open. Null = no sub-menu: the grid stays as it is, as after
// System's own handler when its list fails. Runs on the game's menu thread.
// The trampoline also passes the functor's return address ([rbp + 8]: the functor 0x1B8FC60 keeps
// a frame) and its caller's frame ([rbp]), for the log: which game code asks for the sub-menu.
// The grid's decide routine (0x1BE9380, from 0x1BEAB24) calls the selected entry's handler TWICE
// per press, in the same frame (call sites 0x1BE97FC and 0x1BE99E4; measured, run t1d), building
// a composed job from both results: one press = calls within 250 ms of each other.
void** PS4_SYSV_ABI VrTileHandler(void** out, void* arg, u8* functor_return, u8* caller_frame) {
    *out = nullptr;
    using Clock = std::chrono::steady_clock;
    static std::atomic<s64> last_ns{0};
    static std::atomic<u32> calls{0};
    const s64 now_ns = Clock::now().time_since_epoch().count();
    const s64 previous = last_ns.exchange(now_ns, std::memory_order_relaxed);
    const bool new_press = previous == 0 || now_ns - previous > 250'000'000;
    const u32 presses = new_press ? g_presses.fetch_add(1, std::memory_order_relaxed) + 1
                                  : g_presses.load(std::memory_order_relaxed);
    const u32 call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (new_press) {
        VR::Page::Runtime::Get().RequestOpen(); // the presenter opens the VR page
    }
    if (call <= 400) {
        const auto rva = [](const u8* address) {
            return address >= g_base && address < g_base + g_size
                       ? fmt::format("{:#x}", static_cast<u64>(address - g_base))
                       : fmt::format("{}", fmt::ptr(address));
        };
        const u8* caller_return =
            Plausible(caller_frame) ? Read<u8*>(caller_frame + 8) : nullptr;
        LOG_INFO(Core,
                 "VR tile: A on the VR tile (press {}, call {}, {} ms after the last; arg {}, from "
                 "{} <- {}): no sub-menu; the VR page opens",
                 presses, call,
                 previous == 0 ? std::string{"-"}
                               : fmt::format("{:.1f}", (now_ns - previous) / 1e6),
                 fmt::ptr(arg), rva(functor_return), rva(caller_return));
    }
    return out;
}

// The retail atlas file (core/vr/tpf_patch.h AtlasFileSize): its FNV-1a 64.
constexpr u64 AtlasFnv = 0xcdea18d6035a2403ull;

// The atlas's override is registered (by PrepareAtlas), and whether it holds the VR controllers'
// button icons (then it stays even when the tile that shows the goggles does not come up).
bool g_atlas_served = false;
bool g_atlas_buttons = false;

// The game's font for the button icons' letters (the English menu font: the letters are Latin in
// every language), read through the mount now, or nullptr (logged).
std::unique_ptr<VR::GameText::GameFont> ReadButtonFont() {
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto file = mnt->ReadFile("/app0/dvdroot_ps4/menu/engus/font.gfx");
    std::string why = "menu/engus/font.gfx not readable";
    if (file) {
        if (auto font = VR::GameText::ParseGameFont(*file, why)) {
            return font;
        }
    }
    LOG_WARNING(Core, "VR buttons: no game font ({}); A, B and the pills keep the game's icons",
                why);
    return nullptr;
}

// The game's icon atlas menu/common.tpf.dcx (core/vr/tpf_patch.h), changed in one pass: the retail
// file is read and unpacked now (~0.1 s), then
//  - with `buttons` (ui_vr_buttons): the button icons redrawn as the VR controllers' buttons
//    (core/vr/key_glyphs.h: our badges, their letters in the game's font);
//  - with `icon_wanted` (the VR tile): the goggles composited from our goggles layer over the
//    atlas's own empty parchment cell (core/vr/tile_icon_compose.h: no game pixel in the source)
//    and written into a free cell;
// and the file is packed again (~0.4 s) on the file override's worker before the game opens it.
// True when the goggles went in (else, logged, the tile shows the parchment).
bool PrepareAtlas(bool icon_wanted, bool buttons) {
    using namespace VR::TpfPatch;
    if (!icon_wanted && !buttons) {
        return false;
    }
    const char* const no_icon = icon_wanted ? "; the icon is the game's parchment" : "";
    const auto started = std::chrono::steady_clock::now();
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    auto file = mnt->ReadFile(AtlasPath);
    if (!file) {
        LOG_INFO(Core, "VR atlas: {} not readable{}", AtlasPath, no_icon);
        return false;
    }
    const u64 fnv = GfxTile::Fnv1a64(*file);
    if (file->size() != AtlasFileSize || fnv != AtlasFnv) {
        LOG_INFO(Core,
                 "VR atlas: {} is not the game's own ({} bytes, fnv {:#x}; a texture mod?): left "
                 "as it is (the button icons stay the game's){}",
                 AtlasPath, file->size(), fnv, no_icon);
        return false;
    }
    const auto dcx = ParseDcx(*file);
    if (!dcx) {
        LOG_WARNING(Core, "VR atlas: {} is not a DCX{}", AtlasPath, no_icon);
        return false;
    }
    auto tpf = std::make_shared<std::vector<u8>>(dcx->unpacked);
    mz_ulong length = dcx->unpacked;
    if (mz_uncompress(tpf->data(), &length, file->data() + dcx->stream, dcx->packed) != MZ_OK ||
        length != dcx->unpacked) {
        LOG_WARNING(Core, "VR atlas: {} does not unpack{}", AtlasPath, no_icon);
        return false;
    }
    const auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
            .count();
    };

    int written = 0;
    if (buttons) {
        const auto drawing = std::chrono::steady_clock::now();
        const auto font = ReadButtonFont();
        const std::string refused = VR::KeyGlyphs::Apply(*tpf, font.get(), written);
        // The gameplay HUD's own (the action prompts): the gameplay layout's buttons.
        const bool swap = VR::GetSettings().swap_sticks;
        int hud = 0;
        const std::string hud_refused = VR::KeyGlyphs::ApplyHud(*tpf, font.get(), swap, hud);
        written += hud;
        LOG_INFO(Core,
                 "VR buttons: {} of {} menu button icons are the VR controllers' (cross A, circle "
                 "B, square LG, triangle RG, L1 LT, R1 RT, OPTIONS Menu){}{}; {} of {} HUD ones "
                 "(the action prompts: cross {}, circle {}, triangle {}){}{}; in {:.1f} ms",
                 written - hud, VR::KeyGlyphs::Glyphs.size(),
                 refused.empty() ? "" : "; kept the game's: ", refused, hud,
                 VR::KeyGlyphs::HudGlyphs.size(), VR::Thumbs::LabelFor(VR::Thumbs::Control::B, swap),
                 VR::Thumbs::LabelFor(VR::Thumbs::Control::A, swap),
                 VR::Thumbs::LabelFor(VR::Thumbs::Control::X, swap),
                 hud_refused.empty() ? "" : "; kept the game's: ", hud_refused, ms_since(drawing));
    }

    bool icon_in = false;
    if (icon_wanted) {
        // The icon: our goggles over the atlas's own empty parchment (MENU_Common_00100 at
        // (720, 160)).
        const auto composing = std::chrono::steady_clock::now();
        const auto texture = FindTexture(*tpf, AtlasTexture);
        std::string composed_why = "no MENU_Common_00100";
        std::vector<u8> icon;
        if (texture && texture->format <= 1 && texture->tile_mode == 13 &&
            texture->width == 1024 && texture->height == 1024 &&
            static_cast<std::size_t>(texture->offset) + std::size_t{1024} * 1024 / 2 <=
                tpf->size()) {
            icon = VR::TileIcon::Build(ReadBc1Cell(*tpf, *texture, VR::TileIcon::ParchmentX,
                                                   VR::TileIcon::ParchmentY, VR::TileIcon::Size,
                                                   VR::TileIcon::Size),
                                       composed_why);
        } else if (texture) {
            composed_why = "MENU_Common_00100 is not the BC1 1024 x 1024 tile-mode-13 atlas";
        }
        const double compose_ms = ms_since(composing);
        if (icon.empty()) {
            LOG_WARNING(Core, "VR tile: the icon cannot be composited ({}); the icon is the "
                              "game's parchment",
                        composed_why);
        } else {
            if (const char* dump = std::getenv("VR_TILE_ICON_DUMP"); dump != nullptr) {
                // Diagnostics: the composited icon's 400 BC1 blocks (row by row) to a file.
                if (std::FILE* f = std::fopen(dump, "wb"); f != nullptr) {
                    std::fwrite(icon.data(), 1, icon.size(), f);
                    std::fclose(f);
                    LOG_INFO(Core, "VR tile: VR_TILE_ICON_DUMP wrote the icon's BC1 blocks to {}",
                             dump);
                }
            }
            const std::string why = WriteBc1Cell(*tpf, AtlasTexture, CellX, CellY,
                                                 VR::TileIcon::Size, VR::TileIcon::Size, icon);
            if (!why.empty()) {
                LOG_WARNING(Core, "VR tile: the icon cannot go into the atlas ({}); the icon is "
                                  "the game's parchment",
                            why);
            } else {
                icon_in = true;
                LOG_INFO(Core,
                         "VR tile: the icon in the game's atlas MENU_Common_00100 at ({}, {}) "
                         "(80x80 BC1: our goggles composited over the atlas's own parchment at "
                         "({}, {}) in {:.1f} ms)",
                         CellX, CellY, VR::TileIcon::ParchmentX, VR::TileIcon::ParchmentY,
                         compose_ms);
            }
        }
    }
    if (written == 0 && !icon_in) {
        return false; // nothing changed: the game's own file
    }
    auto original = std::make_shared<std::vector<u8>>(std::move(*file));
    LOG_INFO(Core,
             "VR atlas: {} read, unpacked and changed in {:.0f} ms (button icons {}, tile icon "
             "{}); packed again in the background",
             AtlasPath, ms_since(started), written, icon_in ? "yes" : "no");
    Core::FileSys::SetFileOverrideDeferred(
        AtlasPath,
        [original, tpf, dcx = *dcx]() mutable -> std::vector<u8> {
            mz_ulong packed = mz_compressBound(static_cast<mz_ulong>(tpf->size()));
            std::vector<u8> stream(packed);
            // Level 3: 9.67 MB (the game's own 9.42 MB) in ~0.4 s.
            if (mz_compress2(stream.data(), &packed, tpf->data(),
                             static_cast<mz_ulong>(tpf->size()), 3) != MZ_OK) {
                return {};
            }
            stream.resize(packed);
            std::vector<u8> out =
                RebuildDcx(*original, dcx, stream, static_cast<u32>(tpf->size()));
            original.reset();
            tpf.reset();
            return out;
        },
        written > 0 ? "VR button icons" : "VR tile icon");
    g_atlas_served = true;
    g_atlas_buttons = written > 0;
    return icon_in;
}

// The tile that would show the goggles did not come up: the game's own atlas again, unless the
// button icons are in it (the goggles' cell alone is harmless: the game never draws it).
void DropAtlasIcon() {
    if (g_atlas_served && !g_atlas_buttons) {
        Core::FileSys::ClearFileOverride(VR::TpfPatch::AtlasPath);
        g_atlas_served = false;
    }
}

// The Options movie with the tile, or empty (logged).
std::vector<u8> BuildMovie() {
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto movie = mnt->ReadFile(MoviePath);
    if (!movie) {
        LOG_INFO(Core, "VR tile: {} not readable; the tile is off", MoviePath);
        return {};
    }
    const u64 fnv = GfxTile::Fnv1a64(*movie);
    if (movie->size() != MovieSize || fnv != MovieFnv) {
        LOG_INFO(Core,
                 "VR tile: {} is not the game's own ({} bytes, fnv {:#x}; a mod's movie?): the "
                 "tile is off and the movie is left as it is",
                 MoviePath, movie->size(), fnv);
        return {};
    }
    // The icon: the goggles in a cell of the game's own icon atlas (default), drawn by the movie
    // as the game's row-0 icons are. VR_TILE_ICON=parchment (diagnostics): the game's empty
    // parchment, no icon.
    const char* env = std::getenv("VR_TILE_ICON");
    const std::string_view choice = env != nullptr ? std::string_view{env} : std::string_view{};
    GfxTile::Icon icon{};
    std::string what;
    if (choice != "parchment" && PrepareAtlas(true, VR::GetSettings().ui_vr_buttons)) {
        icon.atlas = true;
        icon.atlas_x = static_cast<u16>(VR::TpfPatch::CellX);
        icon.atlas_y = static_cast<u16>(VR::TpfPatch::CellY);
        what = fmt::format("80x80 from the game's atlas at ({}, {})", icon.atlas_x, icon.atlas_y);
    } else {
        icon.parchment = true;
        what = choice == "parchment" ? "none (VR_TILE_ICON=parchment)" : "none (the parchment)";
    }
    auto result = GfxTile::AddTile(*movie, icon);
    if (result.movie.empty()) {
        LOG_WARNING(Core, "VR tile: the Options movie could not be patched ({}); the tile is off",
                    result.error);
        if (icon.atlas) {
            // The atlas cell is harmless on its own (unused by the game), but serve the game's
            // own file when no tile shows it.
            DropAtlasIcon();
        }
        return {};
    }
    LOG_INFO(Core,
             "VR tile: Options movie with Item_0_3 at stage ({:.1f}, {:.1f}), icon {} "
             "(bitmap #{}, shape #{}, tile #{}), {} -> {} bytes",
             result.x, result.y, what, result.bitmap_id, result.shape_id, result.sprite_id,
             movie->size(), result.movie.size());
    return std::move(result.movie);
}

// Writes our UTF-16 string into near memory.
u8* NearString(const char16_t* text, std::size_t chars_with_null) {
    u8* const at = Hook::NearAlloc(reinterpret_cast<u64>(g_base), g_size,
                                   chars_with_null * sizeof(char16_t), 16);
    if (at != nullptr) {
        std::memcpy(at, text, chars_with_null * sizeof(char16_t));
    }
    return at;
}

// Installed independently of the in-game VR settings tile (vr_menu).
void InstallTitleLabel() {
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    for (const char* name : {"title.gfx", "title_dlc.gfx", "title_dlc_eu.gfx"}) {
        const std::string path = std::string{"/app0/dvdroot_ps4/menu/"} + name;
        const auto original = mnt->ReadFile(path);
        if (!original) {
            continue;
        }
        const auto stereo = VR::GetStereo();
        // Native multiview shares mode 2's camera setup; its startup switch distinguishes
        // it from DIBR, as in the renderer and graphics settings page.
        const bool dibr = stereo.stereo && stereo.mode == 2 &&
                          std::getenv("VR_NATIVE_STEREO") == nullptr;
        auto movie = GfxTitle::AddLabel(*original, dibr);
        if (movie.empty()) {
            LOG_INFO(Core, "VR title: {} is not the retail movie; left unchanged", name);
            continue;
        }
        Core::FileSys::SetFileOverride(path, std::move(movie), "VR title label");
    }
}

// The area name's dark strip out of the HUD movie (core/vr/gfx_area_name.h), on the flat screen
// and in VR alike; its smoke, rule and words stay.
// The HUD movie's two dark boxes, both patched into the one override of menu/fe.gfx: the area
// name's strip (core/vr/gfx_area_name.h) and the NPCs' subtitle box (core/vr/gfx_talk_window.h,
// vr.ini ui_dialogue_backing). Each is left as it is on its own when its layout is unexpected.
void InstallAreaNameDim() {
    constexpr const char* path = "/app0/dvdroot_ps4/menu/fe.gfx";
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto original = mnt->ReadFile(path);
    VR::PublishAreaDimRemoved(false);
    if (!original) {
        LOG_INFO(Core, "VR area name: menu/fe.gfx not readable; its dark strip and the subtitle "
                       "box stay");
        return;
    }
    std::vector<u8> movie;
    auto area = VR::GfxAreaName::ClearDim(*original);
    const bool area_done = !area.movie.empty();
    if (area_done) {
        LOG_INFO(Core, "VR area name: {} ({} gradient colours, menu/fe.gfx of {} bytes)",
                 area.why, area.colours, original->size());
        movie = std::move(area.movie);
    } else {
        LOG_INFO(Core, "VR area name: menu/fe.gfx left as it is ({}); its dark strip stays",
                 area.why);
    }
    const auto backing =
        static_cast<VR::GfxTalkWindow::Backing>(VR::GetSettings().ui_dialogue_backing);
    auto talk = VR::GfxTalkWindow::ClearBacking(
        movie.empty() ? std::span<const u8>{*original} : std::span<const u8>{movie}, backing);
    if (!talk.movie.empty()) {
        LOG_INFO(Core, "VR subtitles: {} ({} colours)", talk.why, talk.colours);
        movie = std::move(talk.movie);
    } else {
        LOG_INFO(Core, "VR subtitles: the box stays ({})", talk.why);
    }
    if (movie.empty()) {
        return;
    }
    Core::FileSys::SetFileOverride(path, std::move(movie), "VR HUD movie");
    VR::PublishAreaDimRemoved(area_done);
}

void InstallTile() {
    const std::string mismatch = CheckTileSites(g_base, g_size);
    if (!mismatch.empty()) {
        LOG_INFO(Core, "VR tile: signature mismatch ({}); the Options grid is left as it is",
                 mismatch);
        return;
    }
    std::vector<u8> movie = BuildMovie();
    if (movie.empty()) {
        return;
    }
    // Near the eboot: the label, the help line and the handler's trampoline.
    u8* const label = NearString(Label, std::size(Label));
    u8* const help = NearString(Help, std::size(Help));
    u8* const trampoline = Hook::NearAlloc(reinterpret_cast<u64>(g_base), g_size, 32, 16);
    if (label == nullptr || help == nullptr || trampoline == nullptr) {
        LOG_WARNING(Core, "VR tile: no near memory; the Options grid is left as it is");
        DropAtlasIcon();
        return;
    }
    {
        // mov rdx, [rbp + 8]; mov rcx, [rbp]; jmp qword [rip + 0]; dq VrTileHandler
        std::array<u8, 22> jump = {0x48, 0x8B, 0x55, 0x08, 0x48, 0x8B, 0x4D, 0x00,
                                   0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        const u64 handler = reinterpret_cast<u64>(&VrTileHandler);
        std::memcpy(jump.data() + 14, &handler, sizeof(handler));
        std::memcpy(trampoline, jump.data(), jump.size());
        Hook::FlushCode(trampoline, jump.size());
    }
    const auto label_lea = LeaTo(reinterpret_cast<u64>(g_base + LabelLeaRva),
                                 reinterpret_cast<u64>(label));
    const auto help_lea =
        LeaTo(reinterpret_cast<u64>(g_base + HelpLeaRva), reinterpret_cast<u64>(help));
    const auto handler_lea = LeaTo(reinterpret_cast<u64>(g_base + HandlerLeaRva),
                                   reinterpret_cast<u64>(trampoline));
    if (!label_lea || !help_lea || !handler_lea) {
        LOG_WARNING(Core, "VR tile: near memory out of reach; the Options grid is left as it is");
        DropAtlasIcon();
        return;
    }
    if (!Hook::MakeWritable(g_base + GateRva, GatePatch.size()) ||
        !Hook::MakeWritable(g_base + LabelLeaRva, HandlerLeaRva + LeaLength - LabelLeaRva)) {
        LOG_WARNING(Core, "VR tile: could not make the sites writable; the grid is left as it is");
        DropAtlasIcon();
        return;
    }
    // The movie first: until the gate opens, cell 3 stays empty and the game hides Item_0_3.
    Core::FileSys::SetFileOverride(MoviePath, std::move(movie), "VR tile");
    std::memcpy(g_base + LabelLeaRva, label_lea->data(), LeaLength);
    std::memcpy(g_base + HelpLeaRva, help_lea->data(), LeaLength);
    std::memcpy(g_base + HandlerLeaRva, handler_lea->data(), LeaLength);
    Hook::FlushCode(g_base + LabelLeaRva, HandlerLeaRva + LeaLength - LabelLeaRva);
    std::memcpy(g_base + GateRva, GatePatch.data(), GatePatch.size());
    Hook::FlushCode(g_base + GateRva, GatePatch.size());
    g_installed.store(true, std::memory_order_release);
    LOG_INFO(Core,
             "VR tile: installed (Options grid cell 3 = 'VR', right of System; gate {:#x}, label "
             "{}, help {}, handler {} -> {}; sites now {})",
             GateRva, fmt::ptr(label), fmt::ptr(help), fmt::ptr(trampoline),
             fmt::ptr(reinterpret_cast<void*>(&VrTileHandler)),
             TileSitesPatched(g_base, g_size) ? "patched" : "NOT as expected");
}

} // Anonymous namespace

void VrTileEbootLoaded(VAddr base, u64 size) {
    InstallTitleLabel();
    InstallAreaNameDim();
    g_base = reinterpret_cast<u8*>(base);
    g_size = size;
    if (VR::GetSettings().vr_menu == 0) {
        // Read only: whether the sites hold the game's own bytes (they are never written).
        const std::string mismatch = CheckTileSites(g_base, g_size);
        LOG_INFO(Core, "VR tile: off (vr_menu=0): the Options grid is the game's own; sites {}",
                 mismatch.empty() ? std::string{"retail"} : "differ at " + mismatch);
        // The button icons need no tile: the atlas pass on its own.
        PrepareAtlas(false, VR::GetSettings().ui_vr_buttons);
        return;
    }
    InstallProbe();
    InstallTile(); // the atlas pass with the goggles and the button icons, when it gets that far
    if (!g_atlas_served) {
        PrepareAtlas(false, VR::GetSettings().ui_vr_buttons);
    }
    if (IsVrTileInstalled()) {
        // The VR page's art and font, decoded in the background now (the first open shows it).
        VR::Assets::RequestMenuTextures(
            {"MENU_Common_00003", "MENU_Common_00001", "KG_L_LR", "KG_L_UD"});
    }
}

void VrTileMenuUpdate(bool menu_open) {
    if (!g_probe) {
        // vr_menu=0: once, at the first menu open, the sites are compared with the game's own
        // bytes again (read only), so a test can tell nothing was patched while the game ran.
        static bool checked = false;
        if (menu_open && !checked && g_base != nullptr && VR::GetSettings().vr_menu == 0) {
            checked = true;
            const std::string mismatch = CheckTileSites(g_base, g_size);
            const bool refresh = CheckRefreshSite(g_base, g_size);
            LOG_INFO(Core, "VR tile: off (vr_menu=0), at the first menu open: tile sites {}, "
                           "refresh {}, file override {}",
                     mismatch.empty() ? std::string{"retail"} : "differ at " + mismatch,
                     refresh ? "retail" : "differs",
                     Core::FileSys::OpenFileOverride(MoviePath) ? "SET" : "none");
        }
        return;
    }
    if (!menu_open) {
        if (g_controller.exchange(nullptr, std::memory_order_acq_rel) != nullptr) {
            std::scoped_lock lk{g_log_mutex};
            g_logged_controller = nullptr;
            g_logged_cursor = INT_MIN;
        }
        return;
    }
    u8* const controller = g_controller.load(std::memory_order_acquire);
    if (controller != nullptr && Read<u8*>(controller) == g_base + ControllerVtableRva) {
        LogCursor(controller, "poll");
    }
}

s32 VrTileCursor() {
    if (!g_probe) {
        return -1;
    }
    u8* const controller = g_controller.load(std::memory_order_acquire);
    if (controller == nullptr || Read<u8*>(controller) != g_base + ControllerVtableRva) {
        return -1;
    }
    const s32 selected = Read<s32>(controller + OffsetSelected);
    const s32 override_cell = Read<s32>(controller + OffsetOverride);
    const s32 cell = override_cell >= 0 ? override_cell : selected;
    return cell >= 0 && cell < Read<s32>(controller + OffsetCellCount) ? cell : -1;
}

bool IsVrTileInstalled() {
    return g_installed.load(std::memory_order_acquire);
}

u32 VrTilePresses() {
    return g_presses.load(std::memory_order_relaxed);
}

} // namespace VR::Bloodborne
