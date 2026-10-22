// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "core/vr/physical_lever.h"

// The lever models pulled by hand (physical_levers), as data: header-only and pure
// (tests/test_physical_lever.cpp). Each rig is measured offline from a copy of
// the model (objbnd: the Havok skeleton and pull clip, the rigid FLVER), never shipped:
//  - rest_t / rest_q / rest_s: the lever bone's local pose at rest (the clip's first frame, which
//    is also the FLVER bind pose). Some lever bones carry their own constant, non-uniform scale
//    (Cainhurst's and the Fishing Hamlet's, about (0.81, 0.46, 0.81)), in the reference pose and
//    as a constant track of every clip: the game draws the bone's rigid vertices scaled by it
//    (its matrix is T * R(q) * diag(s): 0x43D4D0), so the rig does too (BindRig);
//  - axis / sweep: the clip's largest rotation from rest, as an axis in the bone's own rest
//    frame and its angle (the clip's mechanical stop);
//  - handle / shaft_start: rigid FLVER vertices, as the mesh stores them in the bone's local
//    space (before the bone's own scale); the grip bulb's centre and a point on the moving stem
//    above the hinge housing;
//  - end_deg: where the game's own clip leaves the lever after use (0: back at rest);
//  - chain_rest: the rest locals of the lever bone's ancestors (the root unrotated at the origin
//    for every lever stick; the Research Hall capstan turns under a turned root and a column);
//  - motion: a lever stick pulled (Pull), or a capstan's bar walked round its hub (Capstan).
// The live adapter (bloodborne_levers.cpp) binds an object only when its skeleton's names,
// parents, mesh mapping and rest pose all match one of these, and only for the ObjActParam
// rows listed with it. A row id alone never hides a prompt: rows such as 100 (the generic
// lever) are shared by models without a rig, so the normal prompt is replaced only for the
// exact lever instance whose rig binds (core/vr/lever_prompt.h).
// The rigs added on 2026-10-14 take their rest from the Havok reference pose (the FLVER bind
// pose), which is what a lever shows before any clip has played on it: o260041's pull clip starts
// and ends 5 degrees off it (it binds there too: rest_alt_deg). Where the clip's own first frame
// differs, the difference is noted.
// Main game only: the Chalice Dungeon levers (o290010 / 12 / 14, o290020 / 22 / 24, o293110 /
// 12 / 14 and the static o299902; rows 9902, 320, 2902000, 2931100) are deliberately not listed and
// keep the game's own prompt (o290020's skeleton shares o250010's bone names: it is told apart by
// its four-bone mesh table, its turned root and its hinge height).
namespace VR::Lever {

// Havok bones of a lever skeleton (the live reader's bound), and FLVER bones: the entries of the
// matrix provider's FLVER -> Havok table. MaxChain: the most ancestors a lever bone may have.
inline constexpr std::size_t MaxBones = 4;
inline constexpr std::size_t MaxMeshBones = 4;
inline constexpr std::size_t MaxChain = 2;

// A bone's local rest (unit scale): its translation and rotation in its parent's frame.
struct Rest {
    Vec3 t{};
    Body::Quat q{};
};

struct Model {
    const char* name{}; // the model (logs)
    std::uint32_t count{};                         // Havok bones, 3..MaxBones
    std::array<const char*, MaxBones> bones{};     // Havok order
    std::array<std::int16_t, MaxBones> parents{};  // Havok parents
    // The matrix provider's table, read FLVER -> Havok (1D34460 takes a FLVER bone index, checks
    // it against the table's count and reads the Havok bone it draws from): one entry per FLVER
    // bone, so its count is the FLVER bone count, which need not be the Havok count (o260040's
    // mesh has 2 bones over a 4-bone skeleton, o321110's 4 over 3). -1: a FLVER bone with no Havok
    // bone (the game reads any negative entry so; it is drawn from another provider).
    std::uint32_t mesh_count{};
    std::array<std::int16_t, MaxMeshBones> mesh_map{};
    std::uint8_t lever{1}; // Havok index of the lever bone
    // The rest locals (unit scale) of the lever bone's ancestors, root first (Chain: parents up
    // from the lever). One, the root unrotated at the origin, for every lever stick; two for the
    // Research Hall capstan (o353010), whose bar turns under a root turned -90 degrees about X and
    // a column bone turned back +90 (the column the game's own clip lifts once the bar is turned:
    // lifted, it no longer binds). A live ancestor binds within 1 mm, 0.0001 of the quaternion's
    // dot product (about 1.6 degrees) and 0.001 of unit scale.
    std::array<Rest, MaxChain> chain_rest{};
    // The root bone's translation in the model's own reference pose, where it is not the origin
    // (o280001: 0.153 m up). Its pull clip carries the root at the origin, so a lever the game
    // has pulled itself rests there instead: either binds. Zero (the origin) for the others.
    Vec3 root_t{};
    Vec3 rest_t{};
    Body::Quat rest_q{};
    Vec3 rest_s{1.0f, 1.0f, 1.0f}; // the lever bone's own scale (live, constant), drawn
    // A second rest the lever may stand at, degrees along the pull from rest_q about `axis`
    // (o260041: -4.99, its pull clip's first and last frame: where the game's own pull leaves it,
    // while its idle pose is rest_q). Bound there, its stop is still the clip's: the sweep from it
    // is sweep_deg - rest_alt_deg (BoundSweepDeg). 0: none.
    float rest_alt_deg{};
    Vec3 axis{};
    float sweep_deg{};
    Vec3 handle{};      // raw FLVER vertices, bone-local, before rest_s
    Vec3 shaft_start{};
    // Where the model's own clip leaves the lever after use, degrees from rest along the pull. 0:
    // back at rest (a reusable lever: a completed hand pull settles back to rest). sweep_deg: it
    // stays at its stop (a one-shot mechanism: the game shows it pulled from then on, and its
    // script puts it there again after a reload; a completed hand pull leaves it there). Between
    // (o321110: 61.4 of 80.8 degrees): the hand pull settles there from the stop and stays.
    float end_deg{};
    // A capstan (the hand walks its bar round its hub, physical_lever.h Motion) stays where it is
    // let go and, completed, at its stop (end_deg = sweep_deg: its own clip ends turned).
    Motion motion{Motion::Pull};
    std::array<std::int32_t, 3> actions{}; // the ObjActParam rows placed with it (0: none)
};

// The lever bone's ancestors, root first: Havok indices, and how many (0: not a chain of 1 to
// MaxChain bones up to the root).
struct ChainIdx {
    std::array<std::uint8_t, MaxChain> idx{};
    std::size_t n{};
};
constexpr ChainIdx Chain(const Model& m) {
    std::array<std::uint8_t, MaxChain> up{};
    std::size_t n = 0;
    for (int b = m.lever < MaxBones ? m.parents[m.lever] : -1; b >= 0;
         b = b < static_cast<int>(MaxBones) ? m.parents[static_cast<std::size_t>(b)] : -1) {
        if (n == MaxChain || b >= static_cast<int>(MaxBones))
            return {};
        up[n++] = static_cast<std::uint8_t>(b);
        if (b == 0)
            break;
    }
    ChainIdx out{};
    if (n == 0 || up[n - 1] != 0)
        return out; // the walk must end at the root
    for (std::size_t i = 0; i < n; ++i)
        out.idx[i] = up[n - 1 - i];
    out.n = n;
    return out;
}

// What any entry must be: the live checks (Match, the reader's bounds) rely on it.
constexpr bool WellFormed(const Model& m) {
    if (!m.name || m.count < 3 || m.count > MaxBones || m.mesh_count < 1 ||
        m.mesh_count > MaxMeshBones || m.parents[0] != -1 || m.lever < 1 || m.lever >= m.count)
        return false;
    for (std::size_t i = 0; i < MaxBones; ++i) {
        if ((i < m.count) != (m.bones[i] != nullptr))
            return false;
        if (i >= 1 && i < m.count && (m.parents[i] < 0 || m.parents[i] >= static_cast<int>(i)))
            return false;
    }
    // The lever's ancestors run up to the root; their rests are unit quaternions, and the entries
    // past the chain are left at their defaults.
    const auto chain = Chain(m);
    if (chain.n == 0)
        return false;
    for (std::size_t i = 0; i < MaxChain; ++i) {
        const auto& r = m.chain_rest[i];
        const float rq2 = r.q.x * r.q.x + r.q.y * r.q.y + r.q.z * r.q.z + r.q.w * r.q.w;
        if (i < chain.n ? rq2 < 0.998f || rq2 > 1.002f
                        : r.t.x != 0 || r.t.y != 0 || r.t.z != 0 || r.q.x != 0 || r.q.y != 0 ||
                              r.q.z != 0 || r.q.w != 1)
            return false;
    }
    // A capstan ends turned (its own clip stays at the stop), like o253010; a lever stick may end
    // anywhere its clip leaves it.
    if (m.motion != Motion::Pull && (m.motion != Motion::Capstan || m.end_deg != m.sweep_deg))
        return false;
    // A second rest: a lever stick's, a few degrees off its rest (beyond the 1.6-degree match
    // tolerance, so the two never overlap), its stop still less than half a turn from it.
    if (m.rest_alt_deg != 0 &&
        (m.motion != Motion::Pull || m.rest_alt_deg < -10.0f || m.rest_alt_deg > 10.0f ||
         (m.rest_alt_deg > -2.0f && m.rest_alt_deg < 2.0f) || m.sweep_deg - m.rest_alt_deg >= 180))
        return false;
    bool drawn = false; // the lever bone has mesh (some FLVER bone draws from it)
    for (std::size_t f = 0; f < MaxMeshBones; ++f) {
        const int h = m.mesh_map[f];
        if (f >= m.mesh_count ? h != 0 : h < -1 || h >= static_cast<int>(m.count))
            return false;
        drawn = drawn || (f < m.mesh_count && h == m.lever);
    }
    const float axis2 = m.axis.x * m.axis.x + m.axis.y * m.axis.y + m.axis.z * m.axis.z;
    const float q2 = m.rest_q.x * m.rest_q.x + m.rest_q.y * m.rest_q.y +
                     m.rest_q.z * m.rest_q.z + m.rest_q.w * m.rest_q.w;
    return drawn && axis2 > 0.998f && axis2 < 1.002f && q2 > 0.998f && q2 < 1.002f &&
           m.rest_s.x > 0.05f && m.rest_s.y > 0.05f && m.rest_s.z > 0.05f && m.sweep_deg > 0 &&
           m.sweep_deg < 180 && m.end_deg >= 0 && m.end_deg <= m.sweep_deg && m.actions[0] != 0;
}

inline constexpr std::array<Model, 15> Models{{
    // Yharnam (m24_00 / 01 / 02): the first physical lever, measured 2026-10-12.
    {.name = "o240001",
     .count = 4,
     .bones = {"o240001", "o240001_00", "o240001_01", "o240001_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 3, 2},
     .rest_t = {0.0f, 0.214419335f, 0.048484571f},
     .rest_q = {0, 0, 0, 1},
     .axis = {1, 0, 0},
     .sweep_deg = 80.7877f,
     .handle = {0.000043f, 1.001864f, -0.753844f},
     .shaft_start = {-0.0002f, 0.46f, -0.34f},
     .actions = {2400000, 2410000, 2420000}},
    // Forbidden Woods (m27_00): the elevator and shortcut levers, row 2700000. The same
    // skeleton (it keeps o240001's bone names) on a taller housing and a shorter, longer-
    // travelled stick: its hinge sits at 0.422 m and the clip pulls it 97.4 degrees.
    {.name = "o270003",
     .count = 4,
     .bones = {"o240001", "o240001_00", "o240001_01", "o240001_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 3, 2},
     .rest_t = {0.0f, 0.421969f, 0.0533446f},
     .rest_q = {0, 0, 0, 1},
     .axis = {1, 0, 0},
     .sweep_deg = 97.4333f,
     .handle = {0.0f, 0.5854f, -0.6904f},
     .shaft_start = {0.0f, 0.266f, -0.314f},
     .actions = {2700000, 0, 0}},
    // Forsaken Castle Cainhurst (m25_00; earlier notes called it Byrgenwerth, which is m32_00):
    // the shortcut elevator lever, row 2500010. A hub with a thin rod and a knob along the bone's
    // local +Z; the bone rests rotated and carries its own constant scale (0.81, 0.46, 0.81) in
    // its reference pose and its clip, which the game draws its rigid mesh at: the knob, 1.43 m
    // out in the mesh, is drawn 1.15 m from the hinge (without the scale the rig cannot bind: the
    // live bone's scale is not 1). The rod swings over the top about local -Y (the object's +X),
    // 82.1 degrees. Its skeleton (lever, lever_01, lever_02) is shared with the library lever
    // o253010 (row 2500000, listed below) and with the Chalice door levers o290020 / 22 / 24 (not
    // listed: four FLVER bones, a root turned -90 degrees and the hinge 0.41 m up).
    {.name = "o250010",
     .count = 3,
     .bones = {"lever", "lever_01", "lever_02", nullptr},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 3,
     .mesh_map = {0, 1, 2, 0},
     .rest_t = {0.0f, 0.214419f, 0.0484846f},
     .rest_q = {0.643953f, -0.643202f, -0.29293f, -0.29293f},
     .rest_s = {0.8077924f, 0.4565846f, 0.8077924f},
     .axis = {0, -1, 0},
     .sweep_deg = 82.1077f,
     .handle = {0.0f, 0.0f, 1.4275f},
     .shaft_start = {0.0f, 0.0f, 0.741f},
     .actions = {2500010, 0, 0}},
    // Central Yharnam (m24_01): the elevator call levers at the top and bottom of the lift, row
    // 100 (the game's generic lever, also placed with o220005, o280001, o321110, o352000, o352001
    // and o360010, each listed below with its own rig). Its own four-bone skeleton, FLVER order
    // equal to Havok's. The lever bone rests tilted -12 degrees about X with its hinge 0.363 m up;
    // the clip pulls it 92.5 degrees about +X (gears o243005_01 / _02 turn with it in the game's
    // clip and stay where they are under the hand pull).
    {.name = "o243005",
     .count = 4,
     .bones = {"o243005", "o243005_00", "o243005_01", "o243005_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 2, 3},
     .rest_t = {0.0014918f, 0.3626277f, 0.0622286f},
     .rest_q = {-0.1043217f, 0.0f, 0.0f, 0.9945436f},
     .axis = {1, 0, 0},
     .sweep_deg = 92.5042f,
     .handle = {-0.0002f, 0.8465f, -0.6036f},
     .shaft_start = {0.0f, 0.54f, -0.39f},
     .actions = {100, 0, 0}},
    // Geometric twins, re-verified from the game dump (2026-10-14): the same skeleton
    // (parents, reference pose), FLVER bind pose, mesh mapping, lever-bone mesh (every vertex of
    // each has an exact counterpart in the other) and pull clip (every track within 0.01 degree,
    // every frame) as the model whose numbers they carry, under their own names and rows. A row
    // binds only the models listed with it, so the twin and its original never stand in for
    // each other.
    // Hunter's Nightmare (m34_00): the shortcut door lever, row 3400020. o240001's model.
    {.name = "o342202",
     .count = 4,
     .bones = {"o240001", "o240001_00", "o240001_01", "o240001_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 3, 2},
     .rest_t = {0.0f, 0.214419335f, 0.048484571f},
     .rest_q = {0, 0, 0, 1},
     .axis = {1, 0, 0},
     .sweep_deg = 80.7877f,
     .handle = {0.000043f, 1.001864f, -0.753844f},
     .shaft_start = {-0.0002f, 0.46f, -0.34f},
     .actions = {3400020, 0, 0}},
    // Research Hall (m35_00): the altar lift's levers (1F, 2F, B1), the generic row 100.
    // o240001's model under o240001's bone names: only the row tells it from o240001.
    {.name = "o352001",
     .count = 4,
     .bones = {"o240001", "o240001_00", "o240001_01", "o240001_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 3, 2},
     .rest_t = {0.0f, 0.214419335f, 0.048484571f},
     .rest_q = {0, 0, 0, 1},
     .axis = {1, 0, 0},
     .sweep_deg = 80.7877f,
     .handle = {0.000043f, 1.001864f, -0.753844f},
     .shaft_start = {-0.0002f, 0.46f, -0.34f},
     .actions = {100, 0, 0}},
    // Research Hall (m35_00): the two lifts' top and bottom levers, row 100. o243005's model
    // under its own bone names (o350142, o350142_00 / _01 / _02).
    {.name = "o352000",
     .count = 4,
     .bones = {"o350142", "o350142_00", "o350142_01", "o350142_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 2, 3},
     .rest_t = {0.0014918f, 0.3626277f, 0.0622286f},
     .rest_q = {-0.1043217f, 0.0f, 0.0f, 0.9945436f},
     .axis = {1, 0, 0},
     .sweep_deg = 92.5042f,
     .handle = {-0.0002f, 0.8465f, -0.6036f},
     .shaft_start = {0.0f, 0.54f, -0.39f},
     .actions = {100, 0, 0}},
    // Forsaken Castle Cainhurst (m25_00): the library's hidden-passage lever, row 2500000.
    // o250010's model (and its bone scale) and pull up to the stop (frame 72); its clip then stays
    // there instead of returning (the passage stays open, and the map script puts the lever at the
    // clip's end after a reload), so a completed hand pull leaves it pulled. Its second hub
    // (lever_02), turned by the clip, stays where it is under the hand pull, as on o250010.
    {.name = "o253010",
     .count = 3,
     .bones = {"lever", "lever_01", "lever_02", nullptr},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 3,
     .mesh_map = {0, 1, 2, 0},
     .rest_t = {0.0f, 0.214419f, 0.0484846f},
     .rest_q = {0.643953f, -0.643202f, -0.29293f, -0.29293f},
     .rest_s = {0.8077924f, 0.4565846f, 0.8077924f},
     .axis = {0, -1, 0},
     .sweep_deg = 82.1077f,
     .handle = {0.0f, 0.0f, 1.4275f},
     .shaft_start = {0.0f, 0.0f, 0.741f},
     .end_deg = 82.1077f,
     .actions = {2500000, 0, 0}},
    // Measured from the game dump (2026-10-14) as o243005 was: rest = the reference pose, axis
    // and sweep = the pull clip's largest rotation from it, handle = the grip bulb, shaft_start =
    // the first thin slice of the stem above the hub.
    // Hemwick Charnel Lane (m22_00): a one-way lever and an elevator pair, row 100. Own names,
    // o240001's hinge (0.214 m up) and FLVER order; the clip pulls 86.4 degrees about +X. (Its
    // short "used" clip tilts the lever 5 degrees: a lever left so keeps the game's prompt.)
    {.name = "o220005",
     .count = 4,
     .bones = {"o220005", "o220005_00", "o220005_01", "o220005_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 3, 2},
     .rest_t = {0.00029f, 0.2144193f, 0.0484846f},
     .rest_q = {0, 0, 0, 1},
     .axis = {1, 0, 0},
     .sweep_deg = 86.4168f,
     .handle = {-0.0006f, 1.0539f, -0.721f},
     .shaft_start = {0.0f, 0.54f, -0.37f},
     .actions = {100, 0, 0}},
    // Nightmare of Mensis (m26_00): the lift lever, row 2600000 (shared with the twelve o260040
    // levers, listed below with their own two-bone mesh). It rests unrotated (its idle pose); its
    // pull clip starts tilted -4.99 degrees, stops 75.8 degrees past the rest, about +X, and ends
    // tilted -4.99 again, where the game's own pull leaves it: it binds there too (rest_alt_deg),
    // its stop the same.
    {.name = "o260041",
     .count = 4,
     .bones = {"o260041", "o260041_00", "o260041_01", "o260041_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 3, 2},
     .rest_t = {0.00029f, 0.2144193f, 0.0484846f},
     .rest_q = {0, 0, 0, 1},
     .rest_alt_deg = -4.9893f,
     .axis = {1, 0, 0},
     .sweep_deg = 75.7855f,
     .handle = {0.0039f, 0.9314f, -0.643f},
     .shaft_start = {0.0f, 0.35f, -0.23f},
     .actions = {2600000, 0, 0}},
    // Yahar'gul (m28_00): the elevator levers, row 100. Three bones (FLVER order o280001,
    // o280001_01, o280001_00); the root sits 0.153 m up in the reference pose (the clip carries it
    // at the origin), so the hinge is 0.214 m up as on o240001. The lever rests tilted -5 degrees
    // and the clip pulls it 80.8 degrees about +X. The grip is a crook, not a bulb: the handle is
    // its middle, on the stem's line (the bulb would be the crook's tip, 1.19 m out).
    {.name = "o280001",
     .count = 3,
     .bones = {"o280001", "o280001_00", "o280001_01", nullptr},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 3,
     .mesh_map = {0, 2, 1, 0},
     .root_t = {0.0f, 0.152681f, 0.01f},
     .rest_t = {0.0f, 0.0613187f, 0.038f},
     .rest_q = {-0.0436194f, 0.0f, 0.0f, 0.9990482f},
     .axis = {1, 0, 0},
     .sweep_deg = 80.7855f,
     .handle = {0.004f, 1.03f, -0.67f},
     .shaft_start = {0.0f, 0.48f, -0.33f},
     .actions = {100, 0, 0}},
    // Fishing Hamlet (m36_00): the elevator levers, row 100. o250010's layout (a hub, a rod and a
    // knob 1.41 m out in the mesh along the bone's local +Z, a second hub) under its own names
    // (o360010, _01, _02), with its own bone scale (0.84, 0.80, 0.84): the knob is drawn 1.18 m
    // from the hinge. The rod swings over the top about local -Y, 82.1 degrees.
    {.name = "o360010",
     .count = 3,
     .bones = {"o360010", "_01", "_02", nullptr},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 3,
     .mesh_map = {0, 1, 2, 0},
     .rest_t = {0.0f, 0.21442f, 0.04848f},
     .rest_q = {0.6434398f, -0.6434397f, -0.2932324f, -0.2932325f},
     .rest_s = {0.84004f, 0.797562f, 0.84004f},
     .axis = {0, -1, 0},
     .sweep_deg = 82.0539f,
     .handle = {0.0052f, 0.0011f, 1.4085f},
     .shaft_start = {0.0f, 0.0f, 0.62f},
     .actions = {100, 0, 0}},
    // Measured from the game dump (2026-10-14, wave 2): the levers whose mesh has a different
    // bone count than their skeleton (the matrix provider's table is read FLVER -> Havok).
    // Nightmare of Mensis (m26_00): the twelve lift and cage levers, row 2600000 (shared with
    // o260041 above). o260041's skeleton and clip under its own names, but its FLVER has two bones
    // (the housing and the lever) over the four Havok bones: the two gears turned by the clip have
    // no mesh. Rests unrotated, 0.214 m up; the clip pulls 75.8 degrees about +X and returns.
    {.name = "o260040",
     .count = 4,
     .bones = {"o260040", "o260040_00", "o260040_01", "o260040_02"},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 2,
     .mesh_map = {0, 1, 0, 0},
     .rest_t = {0.0f, 0.2144193f, 0.0484846f},
     .rest_q = {0, 0, 0, 1},
     .axis = {1, 0, 0},
     .sweep_deg = 75.7855f,
     .handle = {0.0009f, 0.885f, -0.6893f},
     .shaft_start = {0.0f, 0.42f, -0.33f},
     .actions = {2600000, 0, 0}},
    // Byrgenwerth (m32_00): the Lecture Building courtyard lever, the generic row 100 (a one-shot:
    // its script disables the action once used). Three Havok bones under four FLVER bones (the
    // fourth, the mechanism block behind it, has no Havok bone). The lever rests tilted -5 degrees,
    // 0.214 m up; the clip pulls it 80.8 degrees about +X and leaves it at 61.4 degrees (its
    // last frame), where a completed hand pull leaves it too.
    {.name = "o321110",
     .count = 3,
     .bones = {"o321110", "o321110_00", "o321110_01", nullptr},
     .parents = {-1, 0, 0, 0},
     .mesh_count = 4,
     .mesh_map = {0, 1, 2, -1},
     .rest_t = {0.0f, 0.2144193f, 0.0484846f},
     .rest_q = {-0.0435249f, 0.0f, 0.0f, 0.9990523f},
     .axis = {1, 0, 0},
     .sweep_deg = 80.7746f,
     .handle = {0.0025f, 0.876f, -0.6243f},
     .shaft_start = {0.0f, 0.40f, -0.28f},
     .end_deg = 61.39f,
     .actions = {100, 0, 0}},
    // Research Hall (m35_00): the Astral Clocktower's turning capstan, row 3500110 (motion 101230,
    // a capstan push; who may turn it is the game's own permission check). Three bones:
    // the root Dummy001 (turned -90 degrees about X), the column o353010_00 under it (turned back
    // +90: the two compose to no rotation) and the capstan o353010_01 under the column, resting
    // turned 180 degrees about Y. FLVER order o353010_00, o353010_01, Dummy001. Its mesh: a wheel
    // 1.66 m in radius and one straight bar along the bone's local +X at 1.182 m up (about 0.2 m
    // thick), out to 3.0 m: the handle is its outer end inside the end collar, the shaft starts
    // just outside the wheel's rim. The clip turns it 90.3 degrees about +Y (the object's +Y: the
    // bar swings from the object's -X to its +Z, pushed by a hunter walking round behind it) and
    // stays turned; the map script then lifts the column with its next clip, which starts there.
    {.name = "o353010",
     .count = 3,
     .bones = {"Dummy001", "o353010_00", "o353010_01", nullptr},
     .parents = {-1, 0, 1, 0},
     .mesh_count = 3,
     .mesh_map = {1, 2, 0, 0},
     .lever = 2,
     .chain_rest = {{{{0.0f, 0.0f, 0.0f}, {-0.70710678f, 0.0f, 0.0f, 0.70710678f}},
                     {{0.0f, 0.0f, 0.0f}, {0.70710678f, 0.0f, 0.0f, 0.70710678f}}}},
     .rest_t = {0.0f, 0.0f, 0.0f},
     .rest_q = {0.0f, 1.0f, 0.0f, 0.0f},
     .axis = {0, 1, 0},
     .sweep_deg = 90.3353f,
     .handle = {2.90f, 1.182f, -0.0025f},
     .shaft_start = {1.80f, 1.182f, -0.0025f},
     .end_deg = 90.3353f,
     .motion = Motion::Capstan,
     .actions = {3500110, 0, 0}},
}};
static_assert(std::ranges::all_of(Models, WellFormed), "every lever model must be well formed");

// Every listed ObjActParam row once, in the order the models first list it (several models share
// the generic row 100).
inline constexpr auto UniqueActions = [] {
    struct List {
        std::array<std::int32_t, 3 * Models.size()> rows{};
        std::size_t count{};
    } out{};
    for (const auto& m : Models)
        for (const auto a : m.actions) {
            bool seen = a == 0;
            for (std::size_t i = 0; i < out.count; ++i)
                seen = seen || out.rows[i] == a;
            if (!seen)
                out.rows[out.count++] = a;
        }
    return out;
}();
inline constexpr std::size_t ActionCount = UniqueActions.count;
// Every listed ObjActParam row: the allowlist for row validation and for the native gate's
// cheap first test. Being listed is necessary, never sufficient, to hide a prompt.
inline constexpr auto Actions = [] {
    std::array<std::int32_t, ActionCount> out{};
    for (std::size_t i = 0; i < ActionCount; ++i)
        out[i] = UniqueActions.rows[i];
    return out;
}();
static_assert(Actions.back() != 0, "Actions must list every model's rows exactly");

inline bool SupportedAction(std::int32_t id) {
    for (const auto a : Actions)
        if (a == id)
            return true;
    return false;
}

// The skeleton as read from the game: its bone count, names (Havok order), parents and the
// matrix provider's FLVER -> Havok table (its own count, the FLVER bone count).
struct Skeleton {
    std::uint32_t count{};
    std::array<std::array<char, 16>, MaxBones> names{};
    std::array<std::int16_t, MaxBones> parents{};
    std::uint32_t mesh_count{};
    std::array<std::int16_t, MaxMeshBones> mesh_map{};
};

// The same Havok bones (count, names, parents), whatever the mesh table.
inline bool SameBones(const Model& m, const Skeleton& s) {
    if (s.count != m.count)
        return false;
    for (std::uint32_t i = 0; i < m.count; ++i) {
        if (std::strncmp(s.names[i].data(), m.bones[i], s.names[i].size()) != 0 ||
            s.parents[i] != m.parents[i])
            return false;
    }
    return true;
}

// The same bones and the same mesh table: as many FLVER bones, each drawn from the same Havok bone
// (an unmapped one negative, any value).
inline bool SameLayout(const Model& m, const Skeleton& s) {
    if (!SameBones(m, s) || s.mesh_count != m.mesh_count)
        return false;
    for (std::uint32_t f = 0; f < m.mesh_count; ++f) {
        const auto want = m.mesh_map[f];
        if (want >= 0 ? s.mesh_map[f] != want : s.mesh_map[f] >= 0)
            return false;
    }
    return true;
}

inline bool ListedWith(const Model& m, std::int32_t action) {
    for (const auto a : m.actions)
        if (a != 0 && a == action)
            return true;
    return false;
}

// A live local at a measured rest: within 1 mm and 1.6 degrees (0.0001 of the quaternions' dot
// product), as the first check had.
inline bool AtRest(const Body::QsTransform& live, Vec3 t, const Body::Quat& q) {
    return Body::Length(live.t - t) <= 0.001f &&
           std::abs(Body::Dot(Body::Normalize(live.q), Body::Normalize(q))) >= 0.9999f;
}
// The lever bone's ancestors at their rests, unscaled (the root also at the model's reference
// offset, root_t): every one a lever's hinge turns under.
inline bool ChainAtRest(const Model& m, std::span<const Body::QsTransform> locals) {
    const auto chain = Chain(m);
    if (chain.n == 0)
        return false;
    for (std::size_t i = 0; i < chain.n; ++i) {
        if (chain.idx[i] >= locals.size())
            return false;
        const auto& live = locals[chain.idx[i]];
        const auto& want = m.chain_rest[i];
        if (Body::Length(live.s - Vec3{1, 1, 1}) > 0.001f ||
            !(AtRest(live, want.t, want.q) ||
              (i == 0 && AtRest(live, want.t + m.root_t, want.q))))
            return false;
    }
    return true;
}
// The lever bone itself at its measured rest and at its own scale within 0.001.
inline bool LeverAtRest(const Model& m, const Body::QsTransform& lever) {
    return AtRest(lever, m.rest_t, m.rest_q) && Body::Length(lever.s - m.rest_s) <= 0.001f;
}
// The lever bone at its second rest (rest_alt_deg along the pull), at its own scale.
inline bool LeverAtAltRest(const Model& m, const Body::QsTransform& lever) {
    return m.rest_alt_deg != 0 &&
           AtRest(lever, m.rest_t,
                  Body::Normalize(m.rest_q * Body::AxisAngle(m.axis, m.rest_alt_deg * Body::Deg))) &&
           Body::Length(lever.s - m.rest_s) <= 0.001f;
}
// Degrees from where the lever bone stands at rest (`lever`) to the clip's stop: the measured
// sweep from rest_q, or from the second rest when it stands there (the stop does not move).
inline float BoundSweepDeg(const Model& m, const Body::QsTransform& lever) {
    return !LeverAtRest(m, lever) && LeverAtAltRest(m, lever) ? m.sweep_deg - m.rest_alt_deg
                                                              : m.sweep_deg;
}

// The model whose layout and rest pose this object shows, from its live bone locals (Havok order):
// the lever's ancestors at their rests (ChainAtRest: the root unrotated and unscaled, at the origin
// or at the model's own reference offset, for every lever stick); the lever bone at its measured
// rest (or its second rest, rest_alt_deg) within 1 mm and 1.6 degrees, and at its own scale within
// 0.001. Else nullptr. Models share
// skeletons: o240001 / o270003 are told apart by their rest poses; twins with the same names and
// rest (o240001 / o342202 / o352001, o250010 / o253010) only by their rows. With `action` (the
// object's ObjActParam row), only a model listed with that row: a model sharing a measured
// skeleton on a row nobody measured it for keeps its prompt. Without one, the first model with
// that layout and rest.
inline const Model* Match(const Skeleton& s, std::span<const Body::QsTransform> locals,
                          std::int32_t action = 0) {
    for (const auto& m : Models) {
        if (!SameLayout(m, s) || (action != 0 && !ListedWith(m, action)) ||
            locals.size() < m.count)
            continue;
        if (ChainAtRest(m, locals) &&
            (LeverAtRest(m, locals[m.lever]) || LeverAtAltRest(m, locals[m.lever])))
            return &m;
    }
    return nullptr;
}
// The same from the root and the lever bone only (it stands in for every other bone): tests, for
// the lever sticks (the lever bone 1 under the root; not o353010's chain).
inline const Model* Match(const Skeleton& s, const Body::QsTransform& root,
                          const Body::QsTransform& lever, std::int32_t action = 0) {
    const std::array<Body::QsTransform, MaxBones> locals{root, lever, lever, lever};
    return Match(s, std::span<const Body::QsTransform>{locals}, action);
}

// The lever bone's parent in the world: `object` (the map object's frame) composed with the live
// locals of the lever's ancestors, root first.
inline Body::QsTransform ParentWorld(const Model& m, std::span<const Body::QsTransform> locals,
                                     const Body::QsTransform& object) {
    auto world = object;
    const auto chain = Chain(m);
    for (std::size_t i = 0; i < chain.n && chain.idx[i] < locals.size(); ++i)
        world = Body::Compose(world, locals[chain.idx[i]]);
    return world;
}

// The hinge of `m`'s lever bone under `parent_world` (the object's frame composed with the lever's
// ancestors: ParentWorld), at the bone's live rest pose (its own scale included), up to the clip's
// stop from there (BoundSweepDeg).
inline BoneBinding Binding(const Model& m, const Body::QsTransform& parent_world,
                           const Body::QsTransform& rest_local) {
    BoneBinding b{};
    b.parent_world = parent_world;
    b.rest_local = rest_local;
    b.axis_local = m.axis;
    b.handle_local = m.handle;
    b.shaft_start_local = m.shaft_start;
    b.shaft = true;
    b.sweep = BoundSweepDeg(m, rest_local) * Body::Deg;
    b.motion = m.motion;
    return b;
}

// A capstan the hand let go before its stop (Motion::Capstan): its bar stays where it was left,
// nothing turns it back, and a new squeeze carries on from there. The game adapter keeps one such
// record (a new one replaces it): which object, its scheduler entry and its handle, the model,
// the lever bone's locals at its true rest (`original`) and as the hand left them (`last`, the
// game's bytes), the angle (radians from rest toward the stop), and the ObjActParam row it stands
// on (a restore binds it again with it).
struct Parked {
    std::uint64_t object{}, entry{};
    std::uint32_t handle{};
    const Model* model{};
    Body::HkQsTransform original{}, last{};
    float angle{};
    std::int32_t action{};
};
// The model a parked capstan still is, or nullptr: the very object (address, entry and handle),
// its layout, a row listed with it, its ancestors at rest, the record's rest the model's own, an
// angle within its stops, and the lever bone still exactly, byte for byte, as the hand left it
// (the game animating it, or anything else writing it, ends the record: the lever then binds at
// rest again, or keeps its prompt).
inline const Model* StillParked(const Parked& p, const Skeleton& s,
                                std::span<const Body::HkQsTransform> raw, std::uint64_t object,
                                std::uint64_t entry, std::uint32_t handle, std::int32_t action) {
    const Model* m = p.model;
    if (!m || m->motion != Motion::Capstan || p.object == 0 || p.object != object ||
        p.entry != entry || p.handle != handle || !SameLayout(*m, s) || !ListedWith(*m, action) ||
        raw.size() < m->count || !std::isfinite(p.angle) || !(p.angle > 0.0f) ||
        p.angle > m->sweep_deg * Body::Deg + 1e-5f)
        return nullptr;
    std::array<Body::QsTransform, MaxBones> locals{};
    for (std::size_t i = 0; i < m->count; ++i)
        locals[i] = Body::FromHavok(raw[i]);
    if (!ChainAtRest(*m, std::span<const Body::QsTransform>{locals.data(), m->count}) ||
        !LeverAtRest(*m, Body::FromHavok(p.original)) ||
        std::memcmp(&raw[m->lever], &p.last, sizeof(p.last)) != 0)
        return nullptr;
    return m;
}

// Where a completed pull leaves the lever (radians from rest, toward the stop: end_deg).
inline float EndAngle(const Model& m) {
    return std::min(m.end_deg, m.sweep_deg) * Body::Deg;
}
// Its own clip leaves it at the stop: a completed hand pull ends there at once.
inline bool StaysAtStop(const Model& m) {
    return m.end_deg >= m.sweep_deg;
}

} // namespace VR::Lever
