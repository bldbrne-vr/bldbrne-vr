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

// In-frame draw lists: small things of ours (the quick-item arc, its cards and labels, a held
// item the game has no model for, debug markers) drawn into the game's own frame by
// renderer_vulkan/vr_inframe.h, with the game's camera and depth, right after the scene pass and
// before the DIBR capture and the HUD.
//
// The render-camera hook (loop thread) builds one list per camera update, already in that
// camera's view space (from the render camera's own rows and position, never reconstructed), and
// submits it under the update's camera record id; the GPU thread takes the list of the record the
// frame was drawn with. Pure data (no Vulkan), a small mutex ring: the GPU thread copies one list
// per frame.
namespace VR::InFrame {

// Meshes of the in-frame library (renderer_vulkan/vr_inframe.cpp builds them; metres).
enum class MeshId : std::uint16_t {
    Plate,   // the card: a dark rounded disc facing +Z, radius 0.5, front face at z = +0.06
    Ring,    // a thin ring in the z = 0 plane, outer radius 0.5 (rim / gold ring)
    Quad,    // a unit square in the z = 0 plane facing +Z, uv (0,0) top-left
    Plaque,  // a rounded dark box 1 x 1 x 0.1 centred at the origin (label backing)
    Bottle,  // our held item meshes (MeshKind order from core/vr/quick_items.h)
    Knife,
    Pebble,
    Flask,
    Pellet,
    Crystal,
    Paper,
    Ash,
    Token,
    Axis,    // a 1 m arrow along +Y (debug markers)
    Model,   // a game model (VR::Assets::GetModel(tex_key)), uploaded on first use
    // The Blood Gauge (core/vr/bloodborne_vitals.cpp): its plate, a gently curved shell (u along
    // mesh X, v round the arm along mesh Z, both -0.5 .. 0.5; its sag in metres along -Y, as a
    // 45 mm radius cylinder 34.5 mm across bends: scale X and Z, never Y), aux = (u, v) + 0.5;
    // and a strap, a ring segment of a unit cylinder about mesh X (y = cos a, z = sin a,
    // a = -75 .. 75 degrees; x -0.5 .. 0.5): scale Y by its height on top, Z by its reach round
    // the arm (an ellipse; one item per side, see Shade::Gauge's uv).
    GaugePlate,
    GaugeStrap,
    // The Blood Vial's needle (core/vr/vial_needle.h), unit meshes along -Y: the needle, a wire of
    // radius 0.5 from y 0 to its point at y -1 (Shade::Steel with uv.x > 0: a ribbon the vertex
    // shader turns to the eye; its vertices on the axis, aux = (side -1 / +1, its radius there: 1
    // the shaft, tapering to 0 at the point)); its hub, a collar of radius 0.5 at y 0 narrowing to
    // a quarter of that at y -1.
    Needle,
    NeedleHub,
    Syringe, // antique syringe, 176 mm nominal; fitted to the game vial
    VictorianMolotov, // same bounds and neck grip as game model s10010
    ApothecaryCase,
    QuickHolster, // fitted open leather holder for an equipped quick item
    StoredSyringe, // the same syringe with fewer radial segments in the case
    // Held goods the game has no model of (vr_goods_meshes.h; their GoodsLook in
    // bloodborne_quickitems.cpp): Shade::Goods unless noted.
    IosefkaBottle,    // Iosefka's Blood Vial, open, all glass (Shade::Syringe, a potion)
    IosefkaLabel,     // its paper label
    BloodTube,        // Arianna's / Adella's blood: the vial made a sealed tube (Shade::Syringe)
    BloodTubeRing,    // Adeline's, a steel pull stopper (Shade::Syringe)
    BloodClothVelvet, // the cloth tied over Arianna's tube
    BloodClothLinen,  // over Adella's
    ResonantBell,     // the Small / Sinister Resonant Bell
    Skull,            // Madman's Knowledge, Great One's Wisdom (Sangiran 17, embedded data)
    Antidote,         // pills on a square of paper
    Notebook,
    Coldblood,
    RubPaperFire,  // Fire Paper: a scrap of charred paper, held by its left end
    RubPaperBolt,  // Bolt Paper: the same scrap, its own material
    PhantasmShell, // the Empty Phantasm Shell
    HunterMark, RegisterBook, LeagueBook, BeckoningBell, ChoirBell, BeastPellet, AshUrn, UmbilicalCord, // reused goods geometry with item-specific materials
    Count,
};

enum class Shade : std::uint8_t {
    Lit,     // lit by the key light and the ambient, plus emissive
    Unlit,   // color as is (cards, plaques: readable in the dark)
    Icon,    // the icon texture over color (premultiplied)
    Text,    // the label texture: fill = color, halo = emissive.rgb at emissive.a
    Model,   // a game model's diffuse texture x color, lit like the scene around it (the scene
             // light behind it sets the exposure), emissive as a soft rim glow
    Card,    // the arc's card: a translucent dark disc (color, a = opacity) with a hairline rim
             // (emissive.rgb) and a soft inner glow (uv.x = its strength), all by radius
    Gauge,   // the Blood Gauge (vr_inframe.frag ShadeGauge): its plate's brass, glass tubes and
             // liquids from the mesh's (u, v); color = the levels (hp, rally, stamina, bullets),
             // emissive = (glow, pulse, fog, bubbles), uv = (alpha, quicksilver tick step,
             // part: 0 plate / 1 strap, shake mm); a strap's uv = (alpha, the side drawn: +1 the
             // ring's a >= 0, -1 a <= 0, 0 both, 1, where it fades out: its a in radians, 0 = 0.95)
    HoldRing, // quick_hold_use's progress ring on a Quad facing the eye (vr_inframe.frag
              // ShadeHoldRing): a thin band of radius uv.z, half width uv.w (of the quad), a dim
              // track (color, a = opacity) filled clockwise from the top with emissive.rgb (at
              // emissive.a) as uv.x goes 0..1; uv.y the finish's flash (1 .. 0)
    VialDrain, // vial_inject_drain (core/vr/vial_drain.h): a game model shaded as Model, with a
               // blood level along its mesh y (vr_inframe.frag ShadeVialDrain): uv = (level, the
               // column's bottom and top, fill), color = the blood (rgb, a = its opacity),
               // emissive = (the axis' mesh x and z, the film's strength, the grip's mesh y)
    Steel, // the vial's needle and hub (vr_inframe.frag ShadeSteelFn): metal (color.rgb, a =
           // opacity) under the light of the hand holding it, a faint sheen and a highlight;
           // emissive.xyz = the fist in this mesh's coordinates (the hand's light is read there),
           // emissive.w = the highlight's strength; uv.x > 0: a wire (MeshId::Needle, anti-
           // aliased from its distance to the axis), at least uv.x pixels in half width (thinner
           // is drawn that wide and as much fainter); uv.x = 0: a mesh (the hub, its silhouette
           // softened over a pixel); uv.y = 1 / the frame's height (the renderer sets it), uv.z =
           // how much of the colour of the hand's light it takes (0 .. 1; its brightness always),
           // uv.w = the highlight's sharpness (the Blinn exponent / 100)
    Syringe, // syringe visual: color = local liquid plane (normal xyz, distance w); uv =
             // (chamber bottom, top, inner radius, drain fill); emissive = (axis x, z, film, grip
             // y)
    Molotov, // Victorian bottle: aux.x >= 2 linen, otherwise glass; same data as Model
    Apothecary, // worn leather, brass, suede and stored blood; material id in aux.x
    Goods, // our held goods (vr_inframe.frag ShadeGoodsFn): material id in aux.x (GoodsMat in
           // vr_goods_meshes.h), baked cavity in aux.y; color.rgb = a tint, a = opacity;
           // emissive.rgb = the grab's glow; uv.x = 1 selects Queenly Blood on a coldblood mesh
    NativeLiquid, // native bottle + texture: Syringe chamber/plane data; emissive.z = profile
                  // (1 blue elixir, 2 lead elixir, 3 blood cocktail), w = grip y
};

// Texture of an item.
enum class Tex : std::uint8_t { None, Icon, Label, Model };

struct Item {
    // View <- mesh, 3 rows of 4 (row-major [R | t]): view space of the camera record (game units;
    // x right, y up, z forward along the view), mesh in metres (the scale is in R).
    std::array<float, 12> mv{};
    MeshId mesh{MeshId::Quad};
    Shade shade{Shade::Unlit};
    Tex tex{Tex::None};
    std::uint32_t tex_key{0}; // Icon: the EquipParamGoods iconId; Label: index into labels;
                              // Model (mesh and texture): the model id
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f}; // linear rgb, a = opacity
    std::array<float, 4> emissive{};                    // rgb added (Text: the outline colour)
    std::array<float, 4> uv{0.0f, 0.0f, 1.0f, 1.0f};    // u0 v0 u1 v1 of the texture
    // Sized to a label (the builder does not know how wide a text renders): the renderer
    // multiplies the X column of `mv` by (that label's width / height + fit_pad). -1 = as is.
    std::int8_t fit_label{-1};
    float fit_pad{0.0f};
    // Drawn only where the scene is IN FRONT of it (the game's depth test reversed, no depth
    // write), after the other items: the part of the hovered card the hunter's own fist hides,
    // shown through it (a copy of the card's items, faded in with the hover).
    bool xray{false};
    // Depth-tested as if this much nearer to the camera (game units; its pixels stay where they
    // are): the gauge on the wrist draws over the sleeve it rests on (a cuff, a gauntlet that
    // bulges a little past it) and is still hidden by what is really in front of it (the right
    // hand, a weapon, an enemy). Never pulled nearer than the near plane allows.
    float depth_pull{0.0f};
    // Placed from the hunter's own pose (his palm, wrist or forearm dummies): with
    // inframe_hand_sync it is carried with your play space from the camera update his pose was
    // placed for to this one (Carry), so it stays on the hand the frame shows. False: placed in
    // your play space or the world (the arc's cards, a ring where an item was used).
    bool hand{false};
    // Optional conservative view-space bounding sphere (xyz, radius in game units).
    // Radius <= 0 keeps legacy behavior. Holsters use this to skip wholly off-screen groups
    // before texture/probe/depth work; visible geometry and materials are unchanged.
    std::array<float, 4> cull_sphere{};
};

// Side planes of the actual render projection, with a small guard band. Unknown projection
// or bounds always keep the item. Testing a sphere (not just its centre) prevents edge popping.
struct Visibility {
    float tx{}, ty{}, nx{}, ny{}, eye_extent{};
    bool valid{false};
    Visibility(float vfov, float aspect, float eye_half_width = 0.0f,
               float horizontal_extent = 1.0f, float vertical_extent = 1.0f) {
        valid = std::isfinite(vfov) && std::isfinite(aspect) && vfov > 0.0f &&
                vfov < 3.13f && aspect > 0.0f &&
                std::isfinite(eye_half_width) && eye_half_width >= 0.0f &&
                std::isfinite(horizontal_extent) && horizontal_extent > 0.0f &&
                std::isfinite(vertical_extent) && vertical_extent > 0.0f;
        if (!valid) return;
        const float tangent = std::tan(vfov * 0.5f) * 1.02f;
        ty = tangent * vertical_extent;
        tx = tangent * aspect * horizontal_extent;
        eye_extent = eye_half_width;
        nx = std::sqrt(1.0f + tx * tx);
        ny = std::sqrt(1.0f + ty * ty);
    }
    bool MaySee(const std::array<float, 4>& sphere) const {
        const auto [x, y, z, r] = sphere;
        if (!valid || r <= 0.0f || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(z) || !std::isfinite(r)) return true;
        return z + r >= 0.0f && std::abs(x) <= z * tx + r * nx + eye_extent &&
               std::abs(y) <= z * ty + r * ny;
    }
};

// A camera's view: the render camera's rows (right, up, forward: 3 rows of 4, the 4th column
// unused) and its position (game units). View space = rows * (world - position).
struct ViewOf {
    std::array<float, 12> rows{};
    std::array<float, 3> position{};
};

// The rigid move from `from`'s view space to `to`'s (the same world point): v' = A v + b, with
// A = R_to R_from^T and b = R_to (p_from - p_to). Pure (unit tests).
struct ReView {
    std::array<float, 9> a{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> b{};

    static ReView Between(const ViewOf& from, const ViewOf& to) {
        ReView r{};
        const auto& f = from.rows;
        const auto& t = to.rows;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                // (R_to R_from^T)_ij = sum_k to[i][k] * from[j][k]
                r.a[i * 3 + j] = t[i * 4 + 0] * f[j * 4 + 0] + t[i * 4 + 1] * f[j * 4 + 1] +
                                 t[i * 4 + 2] * f[j * 4 + 2];
            }
        }
        const float d[3] = {from.position[0] - to.position[0], from.position[1] - to.position[1],
                            from.position[2] - to.position[2]};
        for (int i = 0; i < 3; ++i) {
            r.b[i] = t[i * 4 + 0] * d[0] + t[i * 4 + 1] * d[1] + t[i * 4 + 2] * d[2];
        }
        return r;
    }
    std::array<float, 3> Point(const std::array<float, 3>& v) const {
        return {a[0] * v[0] + a[1] * v[1] + a[2] * v[2] + b[0],
                a[3] * v[0] + a[4] * v[1] + a[5] * v[2] + b[1],
                a[6] * v[0] + a[7] * v[1] + a[8] * v[2] + b[2]};
    }
    // An item's view <- mesh rows [M | t] -> [A M | A t + b].
    std::array<float, 12> Mv(const std::array<float, 12>& mv) const {
        std::array<float, 12> out{};
        for (int c = 0; c < 4; ++c) {
            const float x = mv[0 * 4 + c], y = mv[1 * 4 + c], z = mv[2 * 4 + c];
            for (int i = 0; i < 3; ++i) {
                out[i * 4 + c] = a[i * 3 + 0] * x + a[i * 3 + 1] * y + a[i * 3 + 2] * z +
                                 (c == 3 ? b[i] : 0.0f);
            }
        }
        return out;
    }
};

struct Label {
    std::array<char, 64> utf8{};
    std::uint8_t px{48}; // glyph size the text is rasterised at
};

// Room for three stocked holsters plus the arc, its occlusion copies, and held props.
constexpr std::uint32_t MaxItems = 96;
constexpr std::uint32_t MaxLabels = 12;

// Who built a list (each camera record can have one of each; Take merges them, the vitals first:
// the gauge under any card of the arc).
enum class Producer : std::uint8_t {
    Quick = 0,  // the quick-item arc, the held items, markers (bloodborne_quickitems.cpp)
    Vitals = 1, // the Blood Gauge (bloodborne_vitals.cpp)
    Count,
};

// A depth probe (the gauge's fit to the arm, core/vr/vitals.h): points in the list's view space
// whose scene depth the renderer reads from the game's own depth (before ours), returned a frame
// or two later through LatestProbe.
constexpr std::uint32_t MaxProbe = 136; // the gauge's 9 lines x 15 points
struct Probe {
    std::uint32_t id{0}; // the requester's serial (0: none)
    std::uint32_t count{0};
    std::array<std::array<float, 3>, MaxProbe> points{};
};
struct ProbeResult {
    std::uint32_t id{0};
    std::uint32_t count{0};
    std::uint64_t record{0};
    std::array<float, MaxProbe> point_z{}; // each point's own view depth (game units)
    std::array<float, MaxProbe> scene_z{}; // the scene's view depth at its pixel (<= 0: none)
};

struct List {
    std::uint64_t record{0};
    std::int64_t steady_ns{0};
    std::uint32_t count{0};
    std::array<Item, MaxItems> items{};
    std::uint32_t label_count{0};
    std::array<Label, MaxLabels> labels{};
    float exposure{1.0f}; // the items' brightness (inframe_exposure); the key light is head-relative
    Producer producer{Producer::Quick};
    Probe probe{};

    // Appends (false when full).
    bool Add(const Item& item) {
        if (count >= MaxItems) {
            return false;
        }
        items[count++] = item;
        return true;
    }
    // A label's index (or -1 when the table is full).
    int AddLabel(const char* utf8, std::uint8_t px = 48) {
        if (label_count >= MaxLabels) {
            return -1;
        }
        Label& l = labels[label_count];
        std::size_t i = 0;
        for (; utf8[i] != 0 && i + 1 < l.utf8.size(); ++i) {
            l.utf8[i] = utf8[i];
        }
        l.utf8[i] = 0;
        l.px = px;
        return static_cast<int>(label_count++);
    }
};

// Stable compaction preserves draw order and label indices. The renderer calls this before
// ending the game render pass or copying scene color/depth for a wholly invisible list.
inline std::uint32_t CullInvisible(List& list, float vfov, float aspect,
                                   float eye_half_width = 0.0f,
                                   float horizontal_extent = 1.0f, float vertical_extent = 1.0f) {
    // Enclose both native eyes: their separation and off-axis frusta. Keep one shared
    // list so an item visible to either eye is recorded for both. Mono keeps the old bounds.
    const Visibility view(vfov, aspect, eye_half_width, horizontal_extent, vertical_extent);
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < list.count; ++i) {
        if (view.MaySee(list.items[i].cull_sphere)) {
            if (kept != i) list.items[kept] = list.items[i];
            ++kept;
        }
    }
    list.count = kept;
    return kept;
}


// Render-camera hook: the list for camera record `list.record` (replaces an older one of it from
// the same producer).
void Submit(const List& list);
// Your play space in the game world as a camera update placed it (core/vr/body_frame.h's
// mapping: world = eye_world + TrackingToWorld(t - head, base_yaw, scale)).
struct PlayFrame {
    std::array<float, 3> eye_world{}; // the mono eye (the render position minus the stereo eye)
    std::array<float, 3> head{};      // the tracking head it was placed for
    float base_yaw{0.0f};
    float scale{1.0f};

    std::array<float, 3> ToTracking(const std::array<float, 3>& w) const {
        const float cb = std::cos(base_yaw), sb = std::sin(base_yaw);
        const float inv = std::abs(scale) > 1e-6f ? 1.0f / scale : 1.0f;
        const float x = w[0] - eye_world[0], y = w[1] - eye_world[1], z = w[2] - eye_world[2];
        return {head[0] + (cb * x - sb * z) * inv, head[1] + y * inv,
                head[2] - (sb * x + cb * z) * inv};
    }
    std::array<float, 3> ToWorld(const std::array<float, 3>& t) const {
        const float cb = std::cos(base_yaw), sb = std::sin(base_yaw);
        const float x = t[0] - head[0], y = t[1] - head[1], z = t[2] - head[2];
        return {eye_world[0] + (cb * x - sb * z) * scale, eye_world[1] + y * scale,
                eye_world[2] + (-sb * x - cb * z) * scale};
    }
};

// inframe_hand_sync: what is on the hunter's hand, placed in world from his pose, carried with
// your play space from the update his pose was placed for (`from`) to this one (`to`): world
// x -> to.ToWorld(from.ToTracking(x)), as a move in `view`'s space. Walking, strafing and smooth
// turns move it with you; your head's own motion moves nothing (it is in both frames alike).
inline ReView CarryBetween(const PlayFrame& from, const PlayFrame& to, const ViewOf& view) {
    // TrackingToWorld = s M(b) d and WorldToTracking = M(b) w / s with the involution
    // M(b) = [cb 0 -sb; 0 1 0; -sb 0 -cb], so T(x) = Q x + c with Q = (s_to / s_from) M_to M_from.
    // Built from small differences only (game positions are hundreds of units: float).
    const auto M = [](float b) {
        const float cb = std::cos(b), sb = std::sin(b);
        return std::array<float, 9>{cb, 0.0f, -sb, 0.0f, 1.0f, 0.0f, -sb, 0.0f, -cb};
    };
    const auto mul = [](const std::array<float, 9>& x, const std::array<float, 9>& y) {
        std::array<float, 9> o{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                o[i * 3 + j] = x[i * 3 + 0] * y[0 * 3 + j] + x[i * 3 + 1] * y[1 * 3 + j] +
                               x[i * 3 + 2] * y[2 * 3 + j];
            }
        }
        return o;
    };
    const auto apply = [](const std::array<float, 9>& x, const std::array<float, 3>& v) {
        return std::array<float, 3>{x[0] * v[0] + x[1] * v[1] + x[2] * v[2],
                                    x[3] * v[0] + x[4] * v[1] + x[5] * v[2],
                                    x[6] * v[0] + x[7] * v[1] + x[8] * v[2]};
    };
    const std::array<float, 9> m_to = M(to.base_yaw);
    std::array<float, 9> q = mul(m_to, M(from.base_yaw));
    const float k = std::abs(from.scale) > 1e-6f ? to.scale / from.scale : 1.0f;
    for (float& v : q) {
        v *= k;
    }
    const auto& R = view.rows;
    const auto& p = view.position;
    // T(p) - p = (e_to - p) + s_to M_to (h_from - h_to) + Q (p - e_from)
    const std::array<float, 3> dh = apply(
        m_to, {from.head[0] - to.head[0], from.head[1] - to.head[1], from.head[2] - to.head[2]});
    const std::array<float, 3> qp =
        apply(q, {p[0] - from.eye_world[0], p[1] - from.eye_world[1], p[2] - from.eye_world[2]});
    std::array<float, 3> d{};
    for (int i = 0; i < 3; ++i) {
        d[i] = (to.eye_world[i] - p[i]) + to.scale * dh[i] + qp[i];
    }
    ReView rv{};
    // a = R Q R^T, b = R (T(p) - p)
    std::array<float, 9> rm{R[0], R[1], R[2], R[4], R[5], R[6], R[8], R[9], R[10]};
    std::array<float, 9> rt{R[0], R[4], R[8], R[1], R[5], R[9], R[2], R[6], R[10]};
    rv.a = mul(rm, mul(q, rt));
    rv.b = apply(rm, d);
    return rv;
}

// inframe_hand_sync: a world point placed from the hunter's pose for `from`, carried to `to`:
// to.ToWorld(from.ToTracking(x)) (what CarryBetween does in a view's space), built from small
// differences. Pure (unit tests).
inline std::array<float, 3> CarryPoint(const PlayFrame& from, const PlayFrame& to,
                                       const std::array<float, 3>& x) {
    // M(b) v = (cb vx - sb vz, vy, -sb vx - cb vz); T(x) = M_to (s_to (h_from - h_to) +
    // (s_to / s_from) M_from (x - e_from)) + e_to.
    const auto M = [](float b, const std::array<float, 3>& v) {
        const float cb = std::cos(b), sb = std::sin(b);
        return std::array<float, 3>{cb * v[0] - sb * v[2], v[1], -sb * v[0] - cb * v[2]};
    };
    const float k = std::abs(from.scale) > 1e-6f ? to.scale / from.scale : 1.0f;
    const auto r = M(from.base_yaw, {x[0] - from.eye_world[0], x[1] - from.eye_world[1],
                                     x[2] - from.eye_world[2]});
    std::array<float, 3> t{};
    for (int i = 0; i < 3; ++i) {
        t[i] = k * r[i] + to.scale * (from.head[i] - to.head[i]);
    }
    const auto w = M(to.base_yaw, t);
    return {to.eye_world[0] + w[0], to.eye_world[1] + w[1], to.eye_world[2] + w[2]};
}

// inframe_hand_sync: whether two updates in a row carry (`apply`): not a teleport, a snap turn or
// a scale change between them (your play space moved more than 1 m or turned more than 20
// degrees). `still`: nothing moved (it applies and moves nothing). Pure (unit tests).
struct CarryStep {
    bool apply{false};
    bool still{false};
    float moved_m{0.0f};  // the play space's move (metres)
    float turn_rad{0.0f}; // and its turn
};
inline CarryStep CarryStepOf(const PlayFrame& from, const PlayFrame& to) {
    CarryStep s{};
    const auto o_from = from.ToWorld({0.0f, 0.0f, 0.0f});
    const auto o_to = to.ToWorld({0.0f, 0.0f, 0.0f});
    const float dx = o_to[0] - o_from[0], dy = o_to[1] - o_from[1], dz = o_to[2] - o_from[2];
    s.moved_m = std::sqrt(dx * dx + dy * dy + dz * dz) / (to.scale > 0.0f ? to.scale : 1.0f);
    const float turn = std::abs(to.base_yaw - from.base_yaw);
    s.turn_rad = std::min(turn, std::abs(turn - 6.2831853f));
    s.apply = s.moved_m <= 1.0f && s.turn_rad <= 0.35f && std::abs(to.scale - from.scale) <= 1e-4f;
    s.still = s.apply && s.moved_m < 1e-5f && s.turn_rad < 1e-6f;
    return s;
}

// inframe_hand_sync: the carry of the camera update being placed (the render-camera hook sets it
// before the producers run, `record` 0 = none for it): what they keep in world or in your play
// space from his pose and do not draw as a hand item (the vial's DIBR depth proxy, the arc's
// hover point, the place of the use ring, the throw's start) is moved by it, so it is where the
// carried hand items are drawn. Render-camera hook thread.
struct HandCarry {
    std::uint64_t record{0};
    PlayFrame from{};
    PlayFrame to{};
    ReView rv{}; // the same move in the record's view space (CarryBetween)
};
void SetHandCarry(const HandCarry& carry);
// The carry of `record` (false: none: not first person, inframe_hand_sync=0, standing still, a
// teleport or a snap turn, or another record's).
bool GetHandCarry(std::uint64_t record, HandCarry& out);

// inframe_hand_sync (render-camera hook, after the producers submitted `record`'s lists): the
// items of its lists placed on the hunter's hand (Item::hand) are moved by `rv` (in that
// record's view space), with the depth probe of a list that is all hand items (the wrist
// gauge). Returns the number of items moved.
std::uint32_t Carry(std::uint64_t record, const ReView& rv);
// Pure (unit tests): `from`'s hand items moved by `rv` as `to_record`'s list (Carry: `to` is
// `from` itself). A list that is all hand items is moved whole, with its labels and depth probe;
// a mixed one (the arc folding as an item is taken) keeps `to`'s other items and labels and gets
// `from`'s unlabelled hand items. False: nothing to do (no hand items, or a mixed list with no
// `to`).
inline Item ReViewed(const Item& item, const ReView& rv) {
    Item out = item;
    out.mv = rv.Mv(item.mv);
    if (item.cull_sphere[3] > 0.0f) {
        const auto p = rv.Point({item.cull_sphere[0], item.cull_sphere[1], item.cull_sphere[2]});
        out.cull_sphere = {p[0], p[1], p[2], item.cull_sphere[3]};
    }
    return out;
}
inline bool RebaseList(const List& from, const List* to, const ReView& rv,
                       std::uint64_t to_record, List& out) {
    std::uint32_t hand = 0;
    for (std::uint32_t i = 0; i < from.count; ++i) {
        hand += from.items[i].hand ? 1u : 0u;
    }
    if (hand == 0) {
        return false;
    }
    if (hand == from.count) {
        // All on his hand (the gauge, an item in his fist): the whole list, labels and probe.
        out = from;
        out.record = to_record;
        for (std::uint32_t i = 0; i < out.count; ++i) {
            out.items[i] = ReViewed(from.items[i], rv);
        }
        for (std::uint32_t i = 0; i < out.probe.count && i < MaxProbe; ++i) {
            out.probe.points[i] = rv.Point(from.probe.points[i]);
        }
        return true;
    }
    if (to == nullptr) {
        return false;
    }
    // Mixed: `to`'s own items except its unlabelled hand items, then `from`'s unlabelled ones.
    out = *to;
    out.count = 0;
    const auto labelled = [](const Item& item) {
        return item.tex == Tex::Label || item.fit_label >= 0;
    };
    for (std::uint32_t i = 0; i < to->count; ++i) {
        if (!(to->items[i].hand && !labelled(to->items[i]))) {
            out.items[out.count++] = to->items[i];
        }
    }
    for (std::uint32_t i = 0; i < from.count && out.count < MaxItems; ++i) {
        if (from.items[i].hand && !labelled(from.items[i])) {
            out.items[out.count++] = ReViewed(from.items[i], rv);
        }
    }
    return true;
}
// GPU thread: the lists of `record` merged (false: none, nothing to draw for that frame).
bool Take(std::uint64_t record, List& out);
// GPU thread: a depth probe's answer; any thread: the newest one.
void PublishProbe(const ProbeResult& result);
bool LatestProbe(ProbeResult& out);
// GPU thread: a list of `record` is waiting (cheap; a frame without one skips the target tracking).
bool Has(std::uint64_t record);
// The newest record a list is waiting for (0: none); logs-on diagnostics.
std::uint64_t NewestRecord();

struct Stats {
    std::uint64_t submitted{0};
    std::uint64_t taken{0};
    std::uint64_t missed{0}; // frames with a list submitted for other records only
};
Stats GetStats();
void NoteMiss();

// GPU thread: a submitted list was drawn into its frame / the renderer turned itself off.
void NoteDrawn();
void NoteDisabled();
// Presenter: what is submitted is shown. False once the renderer turned itself off, or after 30
// lists in a row were submitted and none drawn (the game's frame is not one it can draw into);
// tried again 20 s later.
bool Available();

} // namespace VR::InFrame
