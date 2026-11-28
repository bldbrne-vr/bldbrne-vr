// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/bloodborne_woods_shadows.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <miniz.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/file_override.h"
#include "core/file_sys/fs.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/fog_emitters.h"
#include "core/vr/game_area.h"
#include "core/vr/map_lights.h"
#include "core/vr/map_objects.h"
#include "core/vr/mist_ground.h"
#include "core/vr/tpf_patch.h"
#include "core/vr/vr_session.h"
#include "core/vr/woods_shadows.h"
#include "core/vr/graphics_settings.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/fog_patches.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"

// Moon shadows in the Forbidden Woods (core/vr/woods_shadows.h): the woods' map file with the
// shadow-source bit on its trees and rocks, and its draw parameters with a longer shadow fade,
// served from memory in place of the player's own files before the game opens them (the lamp
// scripts' way, core/vr/bloodborne_lamp_light.cpp). Nothing is written to disk.
// On by default since 2026-11-15 (the tester: "i want the fog grid and the tree shadows in
// fastv2-premigration"); VR_WOODS_SHADOWS=0: the game's own files.
namespace VR::Bloodborne {
namespace {

using Bytes = std::vector<u8>;
constexpr const char* Tag = "woods shadows";
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

// One file: unpacked, changed by `change` (false: left as the game's), served packed again.
template <typename Change>
std::string Serve(Core::FileSys::MntPoints& mnt, const char* path, Change&& change) {
    auto file = mnt.ReadFile(path);
    if (!file) {
        return "not readable";
    }
    VR::TpfPatch::Dcx dcx{};
    auto data = Unpack(*file, dcx);
    if (!data) {
        return "not a DCX that unpacks";
    }
    std::string what;
    if (!change(*data, what)) {
        return what + ": the game's own";
    }
    Core::FileSys::SetFileOverrideDeferred(
        path,
        [original = std::move(*file), dcx, unpacked = std::move(*data)]() mutable {
            Bytes packed = Pack(original, dcx, unpacked);
            Bytes{}.swap(original);
            Bytes{}.swap(unpacked);
            return packed;
        },
        Tag);
    return what + ": served";
}

// The game's lingering fog taken over by the fog grid (core/vr/fog_emitters.h). A first stage,
// behind VR_FOG_EMITTERS=<kinds: floor,far,indoor,boss> (unset: the game's own fog, nothing
// changed): Central Yharnam's map files are served with those kinds' effects not started, and
// their places go to the fog grid as patches. VR_FOG_EMITTERS_OURS=0 (tests: the cost of the
// game's own): not started, and none of ours in their place.
// A kind's patch: VR_FOG_EMIT_FLOOR / _FAR / _INDOOR / _BOSS = "rx,ry,rz,density,fall,edge,noise,glow".
struct FogPlan {
    u32 kinds{};  // 1 << FogEmitters::Kind: taken over (while the fog is on: FogWanted)
    bool ours{};  // ... with the fog grid's patches in their places
    bool live{};  // on and off by the menu's Fog row (vr.ini fog_banks); else VR_FOG_EMITTERS's
    struct Shape {
        float rx, ry, rz, density, fall, edge, noise, glow, clear;
    };
    Shape shapes[5]{};
    std::string asked;
};

// The plan, read once (VR_FOG_EMITTERS and the kinds' shapes): what the fog banks take over while
// they are on, whatever the menu's row says at the start (FogWanted: it switches them at the next
// load of an area since 2026-11-21).
const FogPlan& Fog() {
    static const FogPlan plan = [] {
        namespace F = VR::FogEmitters;
        FogPlan p;
        // (The menu's Fog row, vr.ini fog_banks, by the card where not set: what VR_FOG_EMITTERS=1
        // asks for. The switch, where given, wins for the session: tests, and =0 for the game's
        // own fog.)
        const char* env = std::getenv("VR_FOG_EMITTERS");
        p.live = env == nullptr || env[0] == 0;
        if (p.live) {
            env = "1";
        }
        if (!VR::IsEnabled()) {
            return p;
        }
        // Only with the fog grid drawing this session and the moonlight in the air (on with the
        // fog whatever moon_shafts says, VR::MoonGlowOn; off only by VR_MOON_SHAFTS=0). Without it
        // the game's fog stays the game's: never a place with none.
        if (!Shader::Optimization::FogGrid::Enabled() ||
            !Shader::Optimization::MoonShafts::Enabled(true, true, std::getenv("VR_MOON_SHAFTS"))) {
            return p;
        }
        p.asked = env;
        // (VR_FOG_EMITTERS=1: the kinds that are ours by default, the fog near the hunter. The
        // distant-view banks stay the game's own: far off the grid's cells are large and a bright
        // bank shows them, the tester, 2026-11-16: "they look kind of blocky far away"; and the game's
        // give the pale glow down a street that ours did not.)
        // (The indoor fog too stays the game's, the tester, 2026-11-17: ours is lit by what reaches
        // it and a room in the moon's shadow leaves it next to dark, and a 5 m ball of it by an
        // outside wall would show in the street; the boss rooms' is ours: "boss room keep ours".)
        if (p.asked == "1" || p.asked == "on") {
            p.asked = "floor,boss";
        }
        const auto want = [&](const char* name, F::Kind kind) {
            if (p.asked.find(name) != std::string::npos) {
                p.kinds |= 1u << static_cast<u32>(kind);
            }
        };
        want("floor", F::Kind::Floor);
        want("far", F::Kind::Far);
        want("indoor", F::Kind::Indoor);
        want("boss", F::Kind::BossFloor);
        want("plain", F::Kind::Plain);
        const char* const ours_env = std::getenv("VR_FOG_EMITTERS_OURS");
        p.ours = ours_env == nullptr || ours_env[0] != '0';
        const auto shape = [](const char* name, FogPlan::Shape s) {
            if (const char* const text = std::getenv(name)) {
                float v[9] = {s.rx, s.ry, s.rz, s.density, s.fall, s.edge, s.noise, s.glow, s.clear};
                std::sscanf(text, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]);
                s = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};
            }
            return s;
        };
        // (2026-11-16, tuned by eye against the game's own in Central Yharnam, checked in the Forbidden
        // Woods: the game's floor fog is thin near the eye, bright 10 to 25 m off and spreads some
        // 10 m from its emitter; its distant-view banks are low and faint.)
        // (2026-11-16 evening, the user through the clinic's gate: "these patches feel weird and too
        // low maybe": a floor bank knee to waist high with a soft top, 0.7 m and not 0.3, thinner a
        // metre so the column is as bright; its noise 1.4 and not 2.5, wisps and not holes.)
        // (Later that evening, base game against ours through the same gate: at 0.22 the taller bank
        // was fainter than the game's own puff; 0.38, 0.8 m high, a little more noise and glow: a
        // bank that fills the yard waist-high, the same in both eyes.)
        // (Later still: at 0.38 with the merge's limit lifted three banks piled into a white lump
        // in the alley; 0.26 with the limit on the banks' shapes, wisps 2.4: fog one sees through
        // at 10 m, with thin and thick of it.)
        // (Floor fog from 2 m of the eye, not 6: the tester, 2026-11-16, "yes do the fog around the feet
        // too": it lies knee-high round him, and his wake shows in it.)
        p.shapes[0] = shape("VR_FOG_EMIT_FLOOR", {9.0f, 2.2f, 9.0f, 0.26f, 0.8f, 0.7f, 2.4f, 2.8f, 2.0f});
        p.shapes[1] = shape("VR_FOG_EMIT_FAR", {18.0f, 4.0f, 18.0f, 0.08f, 0.0f, 0.95f, 0.5f, 1.8f, 16.0f});
        p.shapes[2] = shape("VR_FOG_EMIT_INDOOR", {5.0f, 2.5f, 5.0f, 0.20f, 0.0f, 0.7f, 1.5f, 1.0f, 0.0f});
        p.shapes[3] = shape("VR_FOG_EMIT_BOSS", {10.0f, 2.2f, 10.0f, 0.26f, 0.8f, 0.7f, 2.4f, 2.8f, 2.0f});
        p.shapes[4] = shape("VR_FOG_EMIT_PLAIN", {8.0f, 2.5f, 8.0f, 0.15f, 1.0f, 0.8f, 1.5f, 2.0f, 0.0f});
        return p;
    }();
    return plan;
}

// The fog banks on now: the menu's row (switched at the next load of an area; read through its
// published copy, any thread), or the session's VR_FOG_EMITTERS.
bool FogWanted(const FogPlan& plan) {
    return plan.kinds != 0 && (!plan.live || VR::FogBanksAsked());
}

// The fog banks switch without a restart (2026-11-21, the user: "change the graphics and see and
// feel the difference without requiring to reboot the game"). The game reads an area's map file as
// it loads the area, and the game's lingering fog is in it: so each map file carries ours as a
// switched layer (file_override.h SetFileOverrideLayer), on while the fog is (FogFilesOn, asked at
// each open: a change made at the title screen holds for the first load), and the area loaded now
// keeps the fog it came with until it is loaded again (travel to another area: a lamp of the same
// area and a death there reload nothing, the mock showed): never the game's fog and ours at once,
// nor neither. Each open says which it was (FogFileOpened), and the fog grid
// draws ours in a map only when its file came with them (FogStateTick).
// The fog row as the frame tick last saw it (its log line, the layers' first build).
std::atomic<bool> g_fog_files_on{false};
// The fog each map's file was last opened with: 0 not opened yet, 1 the game's, 2 ours (MapSlot).
std::array<std::atomic<u8>, 64> g_map_fog{};
// The files with a fog layer (path, map), filled at the start; queued at the first want.
std::vector<std::pair<std::string, u16>> g_fog_layers;
// Every layer queued or built: set only once QueueFogLayers has queued them all (the 2026-11-21
// review: set before, another thread's open found its layer neither built nor queued).
std::atomic<bool> g_fog_layers_queued{false};

// A map's place in g_map_fog: areas 21 to 36, blocks 0 to 3 (MapFiles); -1 for any other.
constexpr int MapSlot(u32 area, u32 block) {
    return area >= 21 && area <= 36 && block <= 3 ? static_cast<int>((area - 21) * 4 + block) : -1;
}

// The game opens a map's file as it loads that area ("/app0/dvdroot_ps4/map/mapstudio/
// m24_01_00_01.msb.dcx"): whether it came with our fog (file_override.h FileOverrideLayerOpened).
void FogFileOpened(std::string_view path, bool applied) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto digit = [&](std::size_t i) { return name[i] >= '0' && name[i] <= '9'; };
    if (name.size() < 6 || (name[0] != 'm' && name[0] != 'M') || !digit(1) || !digit(2) || name[3] != '_' ||
        !digit(4) || !digit(5)) {
        return;
    }
    const u32 area = static_cast<u32>(name[1] - '0') * 10 + static_cast<u32>(name[2] - '0');
    const u32 block = static_cast<u32>(name[4] - '0') * 10 + static_cast<u32>(name[5] - '0');
    if (const int slot = MapSlot(area, block); slot >= 0) {
        g_map_fog[static_cast<std::size_t>(slot)].store(applied ? 2 : 1, std::memory_order_relaxed);
    }
    LOG_INFO(Core, "VR fog: m{:02}_{:02} loaded with {} fog ({})", area, block, applied ? "our" : "the game's",
             name);
}

// A map's file: its lingering fog effects of the plan's kinds found; with `silence` not started
// (the fog layer's file), with `add_places` their places given to the fog grid (once, at the
// start).
// True: the file was changed.
bool FogChange(Bytes& msb, u16 map, std::string& what, bool silence, bool add_places,
               bool* understood = nullptr) {
    namespace F = VR::FogEmitters;
    namespace Patches = Shader::Optimization::FogPatches;
    const FogPlan& plan = Fog();
    if (plan.kinds == 0) {
        return false;
    }
    std::vector<F::Found> found;
    if (!F::Find(msb, found)) {
        what += "fog: not a map file it understands";
        if (understood != nullptr) {
            *understood = false;
        }
        return false;
    }
    // (The places left to the game: neither stopped nor made a patch. fog_emitters.h GamesOwn.)
    std::erase_if(found, [map](const F::Found& f) { return F::GamesOwn(map, f.emitter); });
    auto& patches = Patches::EmitterPatches();
    const u32 silenced =
        silence ? F::Silence(msb, found, plan.kinds)
                : static_cast<u32>(std::ranges::count_if(found, [&](const F::Found& f) {
                      return (plan.kinds >> static_cast<u32>(f.emitter.kind) & 1u) != 0;
                  }));
    u32 added = 0;
    for (const F::Found& f : found) {
        if (!add_places) {
            break;
        }
        const u32 k = static_cast<u32>(f.emitter.kind);
        if (!plan.ours || (plan.kinds >> k & 1u) == 0) {
            continue;
        }
        // (The map's files of its other states list the same places: once each.)
        bool known = false;
        for (const Patches::Patch& p : patches) {
            known = known || (p.map == map && std::abs(p.x - f.emitter.x) < 0.1f && std::abs(p.y - f.emitter.y) < 0.1f &&
                              std::abs(p.z - f.emitter.z) < 0.1f);
        }
        if (known) {
            continue;
        }
        const FogPlan::Shape& s = plan.shapes[k];
        // (As heavy and as wide as its own effect says, fog_emitters.h WeightOf;
        // VR_FOG_EFFECT_WEIGHT=0: every one of a kind the same.)
        static const bool weigh = [] {
            const char* const env = std::getenv("VR_FOG_EFFECT_WEIGHT");
            return env == nullptr || env[0] != '0';
        }();
        const F::Weight w = weigh ? F::WeightOf(f.emitter.effect) : F::Weight{1.0f, 1.0f};
        patches.push_back(Patches::Patch{map, f.emitter.x, f.emitter.y, f.emitter.z, s.rx * w.size, s.ry, s.rz * w.size,
                                         s.density * w.density, s.fall, s.edge, false, s.noise, s.glow, 0.0f, s.clear});
        ++added;
    }
    // The Shadows of Yharnam's room (the Forbidden Woods; the tester, 2026-11-17: "add some fog patches
    // in the battle with shadows of yharnam"): the map's own file has one floor bank in the
    // clearing (-336.5 -186.0 473.7) and one at each end; three more of ours across it, between
    // the fog gate (-322.9 -185.9 494.4) and the way out (-347.2 -185.9 469.3), as floor banks
    // with the floor fog taken over. VR_FOG_SHADOWS_ROOM=0: none; any other number: their light,
    // times (0.85 of the floor banks' where it is not set: "a tiny bit darker", the same day).
    if (add_places && map == 0x1B00 && plan.ours && (plan.kinds >> static_cast<u32>(F::Kind::Floor) & 1u) != 0) {
        static const float light = [] {
            const char* const env = std::getenv("VR_FOG_SHADOWS_ROOM");
            return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 2.0f) : 0.85f;
        }();
        const bool on = light > 0.0f;

        static constexpr float places[3][3] = {
            {-328.5f, -186.0f, 487.5f}, {-343.0f, -186.0f, 484.5f}, {-327.5f, -186.0f, 475.5f}};
        const FogPlan::Shape& s = plan.shapes[static_cast<u32>(F::Kind::Floor)];
        for (u32 i = 0; on && i < 3; ++i) {
            bool known = false;
            for (const Patches::Patch& p : patches) {
                known = known || (p.map == map && std::abs(p.x - places[i][0]) < 0.1f && std::abs(p.z - places[i][2]) < 0.1f);
            }
            if (!known) {
                patches.push_back(Patches::Patch{map, places[i][0], places[i][1], places[i][2], s.rx, s.ry, s.rz, s.density,
                                                 s.fall, s.edge, false, s.noise, s.glow * light, 0.0f, s.clear});
                ++added;
            }
        }
    }
    what += std::to_string(silenced) + " of " + std::to_string(found.size()) +
            (silence ? " lingering fog effects not started, " : " lingering fog effects to take over, ") +
            std::to_string(added) + " new places for the fog grid";
    return silence && silenced != 0;
}

// A map file's fog layer: the file under it (the game's own, or the woods shadows') with the plan's
// kinds of lingering fog not started; empty when it has none of them. An exception (a file the
// parser does not understand among them: the 2026-11-21 review, it counted as one with nothing
// to take over, our fog drawn over the game's): not built, the game's own served.
Bytes FogLayerFile(const std::string& path, u16 map) {
    auto& mnt = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::optional<Bytes> file;
    {
        const Core::FileSys::FileOverrideLayerBypass under;
        file = mnt.ReadFile(path);
    }
    if (!file) {
        throw std::runtime_error("not readable");
    }
    VR::TpfPatch::Dcx dcx{};
    auto data = Unpack(*file, dcx);
    if (!data) {
        throw std::runtime_error("not a DCX that unpacks");
    }
    std::string what;
    bool understood = true;
    if (!FogChange(*data, map, what, true, false, &understood)) {
        if (!understood) {
            throw std::runtime_error("not a map file it understands");
        }
        return {};
    }
    Bytes packed = Pack(*file, dcx, *data);
    if (packed.empty()) {
        throw std::runtime_error("not packed again");
    }
    return packed;
}

// Every fog layer queued (the first time the fog is asked for: file_override.h
// QueueFileOverrideLayer, one worker, one file after another, some 0.7 s of one core, long before
// the next load of an area; an open that wants one not started builds it itself). Two callers at
// once both queue: a layer queued, building or built is left as it is.
void QueueFogLayers() {
    if (g_fog_layers_queued.load(std::memory_order_acquire)) {
        return;
    }
    try {
        for (const auto& [path, map] : g_fog_layers) {
            Core::FileSys::QueueFileOverrideLayer(path, [path, map] { return FogLayerFile(path, map); });
        }
    } catch (...) {
        return; // (out of memory: tried again at the next call)
    }
    g_fog_layers_queued.store(true, std::memory_order_release);
}

// The fog layers' switch, asked at each open of a map file (any thread, outside the overrides'
// lock). On, the layers are queued if they were not yet: the row turned on at the title screen, in
// a session started without the fog, before the frame's tick saw it (the 2026-11-21 review: that
// first load came with the game's fog); the open builds its own file if it was not started.
bool FogFilesOn() {
    const bool on = FogWanted(Fog());
    if (on) {
        QueueFogLayers();
    }
    return on;
}

// A map file's fog layer registered (the start, before the game opens a map), built at once while
// the fog is on.
void FogLayer(const std::string& path, u16 map, bool build) {
    Core::FileSys::SetFileOverrideLayer(path, FogFilesOn, FogFileOpened, std::string{Tag} + ": fog banks");
    g_fog_layers.emplace_back(path, map);
    if (build) {
        Core::FileSys::BuildFileOverrideLayer(path, [path, map] { return FogLayerFile(path, map); });
    }
}

// Once a frame (the loop thread): the menu's row into the files' layers, and the fog the hunter's
// map was loaded with into the fog grid's switch (FogGrid::FogTaken: its patches, their shapes, the
// grid's reach) and the moon glow's (VR::MoonGlowOn: ours need it until the area loads again).
void FogStateTick(u16 map) {
    const FogPlan& plan = Fog();
    if (plan.kinds == 0) {
        return;
    }
    const bool wanted = FogWanted(plan);
    if (g_fog_files_on.exchange(wanted, std::memory_order_acq_rel) != wanted) {
        if (wanted) {
            QueueFogLayers();
        }
        LOG_INFO(Core, "VR fog: {} from the next load of an area (the one loaded now keeps its own)",
                 wanted ? "our fog banks" : "the game's fog");
    }
    if (map == 0) {
        // (No map known this frame, a loading screen or a field that is no map id: the state as it
        // was. A real map outside the table, a chalice dungeon's, follows the row below.)
        return;
    }
    const int slot = MapSlot(map >> 8, map & 0xFFu);
    const u8 loaded = slot >= 0 ? g_map_fog[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed) : 0;
    const bool ours = loaded != 0 ? loaded == 2 : wanted;
    Shader::Optimization::FogGrid::FogTakenNow().store(plan.ours && ours, std::memory_order_relaxed);
    VR::FogBanksLoaded().store(ours, std::memory_order_relaxed);
}

// The game's lingering fog taken over by the fog grid (core/vr/fog_emitters.h). A test stage,
// behind VR_FOG_EMITTERS=<kinds: floor,far,indoor,boss,plain> (unset: the game's own fog, nothing
// changed): every map's file (not the chalice dungeons') is served with those kinds' effects not
// started, and their places go to the fog grid as patches. VR_FOG_EMITTERS_OURS=0 (tests: the
// cost of the game's own): not started, and none of ours in their place.
// A kind's patch: VR_FOG_EMIT_FLOOR / _FAR / _INDOOR / _BOSS / _PLAIN =
// "rx,ry,rz,density,fall,edge,noise,glow". `woods_too`: the Forbidden Woods' two files as well
// (false: the woods shadows serve those, with this change in them).
// The maps' files (not the chalice dungeons'): mAA_BB_00_SS, area AA, block BB, state SS.
constexpr u8 MapFiles[][3] = {
    {21, 0, 0}, {21, 1, 0}, {22, 0, 0}, {23, 0, 0}, {23, 0, 1}, {24, 0, 0}, {24, 0, 1}, {24, 1, 0}, {24, 1, 1},
    {24, 1, 11}, {24, 2, 0}, {24, 2, 1}, {25, 0, 0}, {26, 0, 0}, {27, 0, 0}, {27, 0, 1}, {28, 0, 0}, {28, 0, 1},
    {32, 0, 0}, {32, 0, 1}, {33, 0, 0}, {34, 0, 0}, {35, 0, 0}, {36, 0, 0},
};

// The two verified hanging lantern models occur only in the Woods. Read its two map states
// once for the lamp memory's fixture lookup. And the lights that get no halo (map_objects.h
// NoHaloModels, NoHaloSpots): the files of NoHaloMaps (the Fishing Hamlet's, 77 KB, and the
// Hunter's Nightmare's for the Corpse Pile's heaps, 140 KB).
// VR_FOG_NOHALO=0 (tests): none.
void MapObjectsEbootLoaded() {
    namespace O = VR::MapObjects;
    const bool lanterns = Shader::Optimization::FogLight::Fixtures();
    const bool no_halo = [] {
        const char* const env = std::getenv("VR_FOG_NOHALO");
        return env == nullptr || env[0] != '0';
    }();
    if (!lanterns && !no_halo) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto& mnt = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::vector<std::pair<u16, std::vector<O::Place>>> maps;
    std::vector<std::pair<u16, std::vector<O::NoHaloAnchor>>> anchors;
    u32 files_read = 0;
    for (const auto& f : MapFiles) {
        const bool woods = lanterns && f[0] == 27 && f[1] == 0;
        const bool listed = no_halo && std::ranges::any_of(O::NoHaloMaps, [&](const auto& m) {
                                return m[0] == f[0] && m[1] == f[1];
                            });
        if (!woods && !listed) {
            continue;
        }
        char path[96];
        std::snprintf(path, sizeof(path), "/app0/dvdroot_ps4/map/MapStudio/m%02u_%02u_00_%02u.msb.dcx", f[0], f[1], f[2]);
        const u16 map = static_cast<u16>((f[0] << 8) | f[1]);
        try {
            if (auto file = mnt.ReadFile(path)) {
                VR::TpfPatch::Dcx dcx{};
                if (const auto data = Unpack(*file, dcx)) {
                    if (woods) {
                        auto it = std::ranges::find(maps, map, [](const auto& m) { return m.first; });
                        if (it == maps.end()) {
                            it = maps.insert(maps.end(), {map, {}});
                        }
                        files_read += O::Read(*data, it->second) ? 1 : 0;
                    }
                    if (listed) {
                        auto it = std::ranges::find(anchors, map, [](const auto& m) { return m.first; });
                        if (it == anchors.end()) {
                            it = anchors.insert(anchors.end(), {map, {}});
                        }
                        files_read += O::ReadNoHalo(*data, it->second) ? 1 : 0;
                    }
                }
            }
        } catch (const std::exception&) {
            // (out of memory, an unreadable file: see above)
        }
    }
    std::size_t objects = 0, no_halos = 0;
    try {
        auto& all = O::All();
        all.reserve(maps.size());
        for (auto& [map, places] : maps) {
            if (!places.empty()) {
                objects += places.size();
                all.emplace_back(map, O::Table{std::move(places)});
            }
        }
        // (The spots by hand join their map's anchors.)
        if (no_halo) {
            for (const O::NoHaloSpot& s : O::NoHaloSpots) {
                auto it = std::ranges::find(anchors, s.map, [](const auto& m) { return m.first; });
                if (it == anchors.end()) {
                    it = anchors.insert(anchors.end(), {s.map, {}});
                }
                it->second.push_back(O::AnchorOf(s));
            }
        }
        auto& none = O::NoHaloAll();
        for (auto& [map, list] : anchors) {
            if (!list.empty()) {
                no_halos += list.size();
                none.emplace_back(map, std::move(list));
            }
        }
    } catch (const std::exception&) {
        O::All().clear();
        O::NoHaloAll().clear();
        objects = no_halos = 0;
    }
    O::Publish();
    LOG_INFO(Core, "VR map lanterns: {} placements of {} maps; lights without a halo: {} in {} maps; {} files ({:.1f} ms)",
             objects, O::All().size(), no_halos, O::NoHaloAll().size(), files_read,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void FogEmittersEbootLoaded(bool woods_too) {
    const FogPlan& plan = Fog();
    if (plan.kinds == 0) {
        return;
    }
    // (The fog as at the start, before the first load of an area: FogStateTick from then on.)
    const bool wanted = FogWanted(plan);
    g_fog_files_on.store(wanted, std::memory_order_release);
    Shader::Optimization::FogGrid::FogTakenNow().store(plan.ours && wanted, std::memory_order_relaxed);
    VR::FogBanksLoaded().store(wanted, std::memory_order_relaxed);
    const auto& files = MapFiles;
    const auto started = std::chrono::steady_clock::now();
    auto& mnt = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::string report;
    u32 served = 0;
    u32 lamps_read = 0;
    for (const auto& f : files) {
        // The map's own lamps for the fog they light (core/vr/map_lights.h): its light file, once.
        if (plan.ours && f[2] == 0) {
            char lights[112];
            std::snprintf(lights, sizeof(lights), "/app0/dvdroot_ps4/map/m%02u_%02u_00_00/m%02u_%02u_00_00_0000.btl.dcx",
                          f[0], f[1], f[0], f[1]);
            try {
                if (auto file = mnt.ReadFile(lights)) {
                    VR::TpfPatch::Dcx dcx{};
                    std::vector<VR::MapLights::Light> all;
                    if (const auto data = Unpack(*file, dcx); data && VR::MapLights::Read(*data, all)) {
                        for (const VR::MapLights::Light& l : all) {
                            if (VR::MapLights::IsLamp(l)) {
                                VR::MapLights::Lamps().push_back({static_cast<u16>((f[0] << 8) | f[1]), l});
                                ++lamps_read;
                            }
                        }
                    }
                }
            } catch (const std::exception&) {
                // (out of memory, an unreadable file: that map's fog has the frame's lights only)
            }
        }
        if (f[0] == 27 && !woods_too) {
            continue; // (WoodsShadowsEbootLoaded's: under the woods shadows' own change)
        }
        char path[96];
        std::snprintf(path, sizeof(path), "/app0/dvdroot_ps4/map/MapStudio/m%02u_%02u_00_%02u.msb.dcx", f[0], f[1], f[2]);
        const u16 map = static_cast<u16>((f[0] << 8) | f[1]);
        // Its places for the fog grid, from the file as the game has it; then its fog layer.
        std::string line;
        try {
            if (auto file = mnt.ReadFile(path)) {
                VR::TpfPatch::Dcx dcx{};
                if (auto data = Unpack(*file, dcx)) {
                    FogChange(*data, map, line, false, true);
                } else {
                    line = "not a DCX that unpacks";
                }
            } else {
                line = "not readable";
            }
        } catch (const std::exception& e) {
            line = std::string{"failed ("} + e.what() + ")";
        }
        FogLayer(path, map, wanted);
        ++served;
        if constexpr (Common::Log::Enabled) {
            report += std::string{report.empty() ? "" : "; "} + (path + 32) + ": " + line;
        }
    }
    {
        std::scoped_lock lock{VR::MapLights::ReachMutex()};
        VR::MapLights::Reaches().assign(VR::MapLights::Lamps().size(), {});
    }
    {
        namespace P = Shader::Optimization::FogPatches;
        std::scoped_lock lock{P::FitMutex()};
        P::Fits().assign(P::EmitterPatches().size(), {});
    }
    LOG_INFO(Core,
             "VR fog emitters ({}{}): {} map files with a fog layer ({}), {} places and {} lamps in all ({:.1f} ms): {}",
             plan.asked, plan.ours ? "" : ", none of ours", served,
             wanted ? "on: served from the first load" : "off until the menu's Fog row turns it on",
             Shader::Optimization::FogPatches::EmitterPatches().size(), lamps_read,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), report);
}

} // Anonymous namespace

namespace {
// One taken-over floor bank of the hunter's map near him fitted to its floor (fog_patches.h Fit).
// True: one was measured this frame.
bool FogPatchFitOne(float x, float y, float z, u16 map) {
    namespace P = Shader::Optimization::FogPatches;
    if (!P::FitOn() && !P::SlopeOn()) {
        return false;
    }
    static std::size_t next = 0; // (the loop thread's own)
    const auto& patches = P::EmitterPatches();
    if (patches.empty()) {
        return false;
    }
    constexpr float MeasureM = 45.0f;
    constexpr std::size_t Looked = 96;
    for (std::size_t looked = 0; looked < std::min(Looked, patches.size()); ++looked) {
        const std::size_t i = next;
        next = (next + 1) % patches.size();
        const P::Patch& p = patches[i];
        const float dx = p.x - x, dy = p.y - y, dz = p.z - z;
        if (p.map != map || !(p.fall > 0.01f) || dx * dx + dy * dy + dz * dz > MeasureM * MeasureM) {
            continue;
        }
        {
            // (The Forbidden Woods' files are served after the others: the list may have grown
            // since the fits were laid out.)
            std::scoped_lock lock{P::FitMutex()};
            if (P::Fits().size() != patches.size()) {
                P::Fits().resize(patches.size());
            }
            if (P::Fits()[i].measured) {
                continue;
            }
        }
        // The ground under the emitter (from 1 m over it to 2 m under it).
        Body::Vec3 normal{};
        float fraction = 0.0f;
        bool known = true;
        P::Fit fit{0.0f, 0.0f, 0.0f, p.rx, p.rz, true}; // (as the map file has it, unless the floor says otherwise)
        const auto ground_at = [&](float gx, float gy, float gz, float& ground) {
            switch (SweepMapSphere({gx, gy + 1.0f, gz}, {gx, gy - 2.0f, gz}, 0.1f, normal, fraction)) {
            case MapSweep::Hit:
                ground = gy + 1.0f - 3.0f * fraction;
                return fraction > 0.02f; // (a start inside the scenery tells nothing)
            case MapSweep::Clear:
                return false;
            default:
                known = false;
                return false;
            }
        };
        float ground = p.y;
        if (ground_at(p.x, p.y, p.z, ground) && known) {
            // How far the floor goes each way: step by step, each step free of walls (a sweep at
            // knee height) and with ground under its end (which the next step starts from: slopes
            // and stairs are followed, a drop of more than 2 m ends it).
            std::array<float, 4> free{};
            static constexpr float dirs[4][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
            for (u32 d = 0; d < 4 && known; ++d) {
                const float limit = d < 2 ? p.rx : p.rz;
                float at = 0.0f, floor_y = ground;
                while (at < limit && known) {
                    const float step = std::min(P::FitStepM, limit - at);
                    const Body::Vec3 from{p.x + dirs[d][0] * at, floor_y + 0.6f, p.z + dirs[d][1] * at};
                    const Body::Vec3 to{p.x + dirs[d][0] * (at + step), floor_y + 0.6f, p.z + dirs[d][1] * (at + step)};
                    const MapSweep wall = SweepMapSphere(from, to, 0.15f, normal, fraction);
                    if (wall == MapSweep::Unavailable) {
                        known = false;
                        break;
                    }
                    if (wall == MapSweep::Hit) {
                        at += step * fraction;
                        break;
                    }
                    float next_floor = floor_y;
                    if (!ground_at(to.x, floor_y, to.z, next_floor)) {
                        break; // (a ledge, or nothing known there: the bank ends where the last step did)
                    }
                    floor_y = next_floor;
                    at += step;
                }
                free[d] = at;
            }
            if (known && P::FitOn()) {
                fit = P::FitFrom(free, p.rx, p.rz, ground - p.y);
            }
            // Its ground's slope (fog_patches.h Slope): the ground every SlopeStepM each way, each
            // asked from over the last one found (1.6 m: a stair's rise in that step and more),
            // as far as the bank or SlopeReachM. A step more than 1.2 m up or down is no floor of
            // this bank's (a ledge, a wall's top, the next room's roof): that way ends there.
            if (known && P::SlopeOn()) {
                std::array<std::array<float, 2>, 16> along[2];
                u32 count[2] = {0, 0};
                for (u32 d = 0; d < 4 && known; ++d) {
                    const float limit = std::min(d < 2 ? p.rx : p.rz, P::SlopeReachM);
                    float floor_y = ground;
                    for (float at = P::SlopeStepM; at <= limit + 1e-3f && known; at += P::SlopeStepM) {
                        const float gx = p.x + dirs[d][0] * at, gz = p.z + dirs[d][1] * at;
                        float here = floor_y;
                        bool found = false;
                        switch (SweepMapSphere({gx, floor_y + 1.6f, gz}, {gx, floor_y - 2.0f, gz}, 0.1f, normal, fraction)) {
                        case MapSweep::Hit:
                            here = floor_y + 1.6f - 3.6f * fraction;
                            found = fraction > 0.02f;
                            break;
                        case MapSweep::Clear:
                            break;
                        default:
                            known = false;
                            break;
                        }
                        if (!found || std::abs(here - floor_y) > 1.2f) {
                            break;
                        }
                        floor_y = here;
                        u32& n = count[d / 2];
                        if (n < 16) {
                            along[d / 2][n++] = {(dirs[d][0] + dirs[d][1]) * at, here - ground};
                        }
                    }
                }
                if (known) {
                    fit.sx = P::SlopeFrom(std::span<const std::array<float, 2>>{along[0].data(), count[0]});
                    fit.sz = P::SlopeFrom(std::span<const std::array<float, 2>>{along[1].data(), count[1]});
                }
            }
        }
        if (!known) {
            return false; // (no world yet: another frame)
        }
        {
            std::scoped_lock lock{P::FitMutex()};
            P::Fits()[i] = fit;
        }
        if constexpr (Common::Log::Enabled) {
            static u32 logged = 0;
            if (logged++ < 60) {
                LOG_INFO(Core,
                         "VR fog fit: bank {} at ({:.1f} {:.1f} {:.1f}): moved ({:.1f} {:.1f} {:.1f}), radii {:.1f} x {:.1f} (of "
                         "{:.1f} x {:.1f}), ground {:+.2f}, slope x {:+.3f} z {:+.3f}",
                         i, p.x, p.y, p.z, fit.dx, fit.dy, fit.dz, fit.rx, fit.rz, p.rx, p.rz, ground - p.y, fit.sx, fit.sz);
            }
        }
        return true;
    }
    return false;
}
} // namespace

// The ground under the air's mist (core/vr/mist_ground.h): the lattice grown round the hunter by
// the game's own sphere cast against its static map, a few casts a tick while a cell near him is
// not known yet, none after.
static void MistGroundTick(float x, float y, float z) {
    namespace G = VR::MistGround;
    // (Only where the area has the mist at all, air_light.h Mist::AreaShare: elsewhere no cast.)
    if (!G::On() || !(Shader::Optimization::AirLight::Mist::AreaShare(VR::GameArea::Area()) > 0.0f)) {
        return;
    }
    static G::Lattice lattice; // (the loop thread's own)
    static std::chrono::steady_clock::time_point last{};
    static int shown_x = 1 << 30, shown_z = 1 << 30;
    static constexpr float Radius = 0.1f;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.25f);
    last = now;
    const auto cast = [](float cx, float y_from, float y_to, float cz, float& hit) {
        Body::Vec3 normal{};
        float fraction = 0.0f;
        switch (SweepMapSphere({cx, y_from, cz}, {cx, y_to, cz}, Radius, normal, fraction)) {
        case MapSweep::Hit:
            if (!(fraction > 0.02f)) {
                return G::Cast::Inside; // (a start inside the scenery tells nothing)
            }
            hit = y_from + (y_to - y_from) * fraction - Radius;
            return G::Cast::Hit;
        case MapSweep::Clear:
            return G::Cast::Clear;
        default:
            return G::Cast::Unavailable;
        }
    };
    // (Whether he stands on a floor: asked only where the lattice's ground is not his own.)
    bool on_floor = false;
    if (float ground = 0.0f; lattice.Ground(x, z, ground) && std::abs(ground - y) > G::LevelM) {
        float hit = y;
        on_floor = cast(x, y + 0.5f, y - 0.6f, z, hit) == G::Cast::Hit;
    }
    const bool changed = G::Grow(lattice, x, y, z, on_floor, dt, cast);
    const int cx = G::CellOf(x), cz = G::CellOf(z);
    if constexpr (Common::Log::Enabled) {
        // (Once each time the lattice round him has come to rest: how much of it is known.)
        static bool growing = false;
        if (growing && !changed) {
            int known = 0;
            float lo = 1e9f, hi = -1e9f;
            for (int iz = cz - G::Cells / 2; iz < cz + G::Cells / 2; ++iz) {
                for (int ix = cx - G::Cells / 2; ix < cx + G::Cells / 2; ++ix) {
                    if (lattice.Known(ix, iz)) {
                        ++known;
                        lo = std::min(lo, lattice.At(ix, iz));
                        hi = std::max(hi, lattice.At(ix, iz));
                    }
                }
            }
            static u32 logged = 0;
            if (logged < 12) {
                ++logged;
                LOG_INFO(Core, "VR mist ground: {} of {} cells known round ({:.1f} {:.1f} {:.1f}), the ground from {:.1f} to {:.1f}",
                         known, G::Cells * G::Cells, x, y, z, lo, hi);
            }
        }
        growing = changed;
    }
    if (changed || cx != shown_x || cz != shown_z) {
        shown_x = cx;
        shown_z = cz;
        G::Publish(lattice, x, z);
    }
}

void FogStateFrame() {
    FogStateTick(static_cast<u16>(VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16));
}

void FogLampReachTick(float x, float y, float z) {
    namespace M = VR::MapLights;
    const u16 map_now = static_cast<u16>(VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16);
    // (The menu's Fog row, and the fog this map was loaded with.)
    FogStateTick(map_now);
    MistGroundTick(x, y, z);
    // (One thing a frame: a floor bank's fit first, then a lamp's walls. Only where the fog
    // banks are ours.)
    if (Shader::Optimization::FogGrid::FogTaken() && FogPatchFitOne(x, y, z, map_now)) {
        return;
    }
    if (!Shader::Optimization::FogGrid::Lamps::MapLamps()) {
        return;
    }
    static std::size_t next = 0; // (the loop thread's own)
    const auto& lamps = M::Lamps();
    if (lamps.empty() || M::Reaches().size() != lamps.size()) {
        return;
    }
    const u16 map = static_cast<u16>(VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16);
    // The next unmeasured lamp of his map within MeasureM of him (a share of the list a frame).
    constexpr float MeasureM = 35.0f;
    constexpr std::size_t Looked = 64;
    for (std::size_t looked = 0; looked < std::min(Looked, lamps.size()); ++looked) {
        const std::size_t i = next;
        next = (next + 1) % lamps.size();
        const M::Light& l = lamps[i].light;
        const float dx = l.x - x, dy = l.y - y, dz = l.z - z;
        if (lamps[i].map != map || dx * dx + dy * dy + dz * dz > MeasureM * MeasureM) {
            continue;
        }
        {
            std::scoped_lock lock{M::ReachMutex()};
            if (M::Reaches()[i].measured) {
                continue;
            }
        }
        std::array<float, M::Rays> free{};
        bool known = true;
        for (u32 r = 0; r < M::Rays && known; ++r) {
            const auto& d = M::RayDirections[r];
            const Body::Vec3 from{l.x + d[0] * M::RayStartM, l.y + d[1] * M::RayStartM, l.z + d[2] * M::RayStartM};
            const Body::Vec3 to{l.x + d[0] * l.radius, l.y + d[1] * l.radius, l.z + d[2] * l.radius};
            Body::Vec3 normal{};
            float fraction = 0.0f;
            switch (SweepMapSphere(from, to, 0.05f, normal, fraction)) {
            case MapSweep::Clear:
                free[r] = l.radius;
                break;
            case MapSweep::Hit:
                free[r] = M::RayStartM + fraction * (l.radius - M::RayStartM);
                break;
            default:
                known = false; // (no world yet: another frame)
                break;
            }
        }
        if (known) {
            const std::array<float, 6> axes = M::AxisReach(free, l.radius);
            {
                std::scoped_lock lock{M::ReachMutex()};
                M::Reaches()[i] = {axes, true};
            }
            if constexpr (Common::Log::Enabled) {
                static u32 logged = 0;
                if (logged++ < 40) {
                    LOG_INFO(Core,
                             "VR fog lamps: lamp {} at ({:.1f} {:.1f} {:.1f}) r {:.1f}: free +x {:.1f} -x {:.1f} +y {:.1f} "
                             "-y {:.1f} +z {:.1f} -z {:.1f}",
                             i, l.x, l.y, l.z, l.radius, axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
                }
            }
        }
        return; // (one lamp a frame)
    }
}

void WoodsShadowsEbootLoaded() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const char* const env = std::getenv("VR_WOODS_SHADOWS");
    const bool woods = VR::IsEnabled() && !(env != nullptr && env[0] == '0');
    if (VR::IsEnabled()) {
        MapObjectsEbootLoaded();
    }
    FogEmittersEbootLoaded(!woods);
    if (!woods) {
        return;
    }
    namespace Woods = VR::WoodsShadows;
    const auto started = std::chrono::steady_clock::now();
    auto& mnt = *Common::Singleton<Core::FileSys::MntPoints>::Instance();
    std::string report;
    const auto run = [&](const char* path, auto&& change) {
        std::string line;
        try {
            line = Serve(mnt, path, change);
        } catch (const std::exception& e) { // out of memory, an unreadable file: the game's own
            line = std::string{"failed ("} + e.what() + "): the game's own";
        }
        report += std::string{report.empty() ? "" : "; "} + path + ": " + line;
    };
    for (const char* path : Woods::MsbPaths) {
        run(path, [](Bytes& msb, std::string& what) {
            const Woods::Result r = Woods::PatchMsb(msb);
            what = r.understood ? std::to_string(r.changed) + " of " + std::to_string(r.parts) +
                                      " map pieces now cast"
                                : "not a map file it understands";
            // (The woods' lingering fog: its places here, and the fog layer over this file below.)
            std::string fog;
            FogChange(msb, 0x1B00, fog, false, true);
            if (!fog.empty()) {
                what += "; " + fog;
            }
            return r.understood && r.changed != 0;
        });
    }
    // The woods' fog layers, over the files just served (FogLayerFile reads them as they are
    // served without it).
    if (Fog().kinds != 0) {
        for (const char* path : Woods::MsbPaths) {
            FogLayer(path, 0x1B00, FogWanted(Fog()));
        }
    }
    run(Woods::GparamPath, [](Bytes& data, std::string& what) {
        const bool ok = Woods::PatchGparam(data, 0);
        what = ok ? "shadows fade from 28 m" : "not the game's numbers";
        return ok;
    });
    run(Woods::GparamBinderPath, [](Bytes& data, std::string& what) {
        const bool ok = Woods::PatchGparamBinder(data);
        what = ok ? "shadows fade from 28 m" : "not the game's numbers";
        return ok;
    });
    if constexpr (Common::Log::Enabled) {
        LOG_INFO(Core, "VR woods shadows: {} ({:.1f} ms)", report,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    }
}

} // namespace VR::Bloodborne
