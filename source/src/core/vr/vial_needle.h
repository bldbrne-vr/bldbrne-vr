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
#include <cstddef>

// VR body: the needle on the Blood Vial in the hunter's hand (vr.ini vial_needle=1). The tester,
// 2026-10-09: "can we add a small needle tip on the syringe model?", then "small but will make it
// look nice and more visceral". The game's vial (model s10002, FFX 173) has none: a thin steel
// needle and a small collar (the hub) come out of its stabbing end, along its axis.
// The needle is visual only: injection uses a point VialContactBackM behind the hub end,
// toward the syringe body, independent of needle length, visibility and mesh style.
//
// How it stays on the vial: it is two more in-frame items (renderer_vulkan/vr_inframe.h) built in
// the same list, from the same rows, as the vial they sit on (core/vr/bloodborne_quickitems.cpp
// BuildVialDrain: our model s10002 with its blood level, vial_inject_drain=1, the default): the
// hunter's pose of that very camera update, carried with the hand like the vial
// (inframe_hand_sync). So it is drawn in the game's frame, depth-tested against the scene (his
// fingers, his thigh as it goes in), with the vial's own disparity in both eyes: never a frame
// behind it, never swimming against it. Only on OUR vial: the game's own (FFX 173, the drain off)
// does not sit where s10002 does on the palm dummy (the eye dumps: its end some 6 cm higher, at
// another angle), so a needle placed from our model would float beside it: none there.
//
// Where: the vial model's own frame (metres, +Y along the vial as authored; its bottom, min y, is
// the end that goes in: the hunter holds it like a dagger, the cap under his thumb). The root is
// derived at runtime from the player's own model (NeedleRootOf: the axis, the bottom face's height
// and radius), so no game geometry is in the source.
//
// Pure (no emulator state): the quick-items render tick builds with it, tests/test_vial_needle.cpp
// drives it.
namespace VR::Held {

// The tester, 2026-10-12: "a little longer still, but without changing ... its collision/use
// check" (42 -> 52 mm): contact is based on the hub, never the needle (test_vial_needle.cpp).
inline constexpr float AntiqueNeedleLength = 0.052f;

// The needle's size (metres). A 19-gauge hypodermic, a little heavy so it reads at arm's length in
// the headset (1 mm is about 1.3 pixels of the game's frame at 45 cm over Link; drawn at least
// NeedleMinHalfPx wide), long enough to read as a needle and not a nail, short enough to stay a
// small tip on the 17.6 cm vial (2.2 cm until the finish: a little long at the forearm).
struct NeedleLook {
    float length_m{0.020f};       // from the hub's tip to the needle's point
    float diameter_m{0.0010f};    // the shaft
    float hub_length_m{0.0050f};   // the collar between the vial's end and the needle
    float hub_diameter_m{0.0045f}; // at its root (it narrows to about the needle's width)
    // The hub's root sits where the vial's end is as wide as it (NeedleRoot::InsetFor: flush on a
    // tapered end, on the face of a flat one), this much further in (no seam).
    float inset_m{0.0005f};
    // Which end goes in: -1 the model's bottom (min y, the default), +1 its top.
    float end{-1.0f};
};

// The stabbing end of a vial model, in its own frame (metres).
struct NeedleRoot {
    bool valid{false};
    float x{0.0f}; // the axis (the bounds' centre in x, z)
    float z{0.0f};
    float y{0.0f};        // the end's face (the lowest vertex, or the highest with end = +1)
    float face_r{0.0f};   // the end face's radius: its widest vertex within `band` of it
    float length{0.0f};   // the model's height
    // The end's profile: how wide the model is (its widest vertex, radius) at heights from the
    // end (ProfileStep apart, up to ProfileSpan), never narrowing going in (a running maximum).
    static constexpr float ProfileStep = 0.0005f;
    static constexpr std::size_t ProfileCount = 60; // 3 cm
    std::array<float, ProfileCount> profile{};

    // How far in from the end the model is `radius` wide (metres; 0 when its face already is,
    // the profile's span when it never is): a hub of that radius sits flush there.
    float InsetFor(float radius) const {
        if (face_r >= radius) {
            return 0.0f;
        }
        float prev_d = 0.0f;
        float prev_r = face_r;
        for (std::size_t i = 0; i < ProfileCount; ++i) {
            const float d = static_cast<float>(i + 1) * ProfileStep;
            const float r = profile[i];
            if (r >= radius) {
                return r > prev_r ? prev_d + (radius - prev_r) * (d - prev_d) / (r - prev_r) : d;
            }
            if (r > prev_r) {
                prev_d = d;
                prev_r = r;
            }
        }
        return static_cast<float>(ProfileCount) * ProfileStep;
    }
};

// From the model's vertex positions (`count` of them, `stride` floats apart, x y z first) and its
// bounds: the end face of `end` (-1 bottom, +1 top). `band`: how far from the end a vertex still
// counts as the face's (metres).
inline NeedleRoot NeedleRootOf(const float* positions, std::size_t count, std::size_t stride,
                               const std::array<float, 3>& min, const std::array<float, 3>& max,
                               float end = -1.0f, float band = 0.002f) {
    NeedleRoot r{};
    if (positions == nullptr || count == 0 || stride < 3 || !(max[1] > min[1])) {
        return r;
    }
    r.x = 0.5f * (min[0] + max[0]);
    r.z = 0.5f * (min[2] + max[2]);
    r.y = end < 0.0f ? min[1] : max[1];
    r.length = max[1] - min[1];
    float face = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float* p = positions + i * stride;
        const float d = std::abs(p[1] - r.y);
        const float radius = std::hypot(p[0] - r.x, p[2] - r.z);
        if (d <= band) {
            face = std::max(face, radius);
        }
        // Into the profile's step at or above it (the vertex is that wide by then).
        const auto k =
            static_cast<std::size_t>(std::ceil(d / NeedleRoot::ProfileStep - 1e-3f));
        if (k >= 1 && k <= NeedleRoot::ProfileCount) {
            r.profile[k - 1] = std::max(r.profile[k - 1], radius);
        }
    }
    r.face_r = face;
    float widest = face;
    for (float& w : r.profile) {
        widest = std::max(widest, w);
        w = widest;
    }
    r.valid = true;
    return r;
}

// Rows [R | t] (3 x 4, row-major) of an in-frame item: view <- mesh.
using Rows = std::array<float, 12>;

// The point `p` of the vial's mesh in view (its rows `vial`).
inline std::array<float, 3> RowsPoint(const Rows& m, const std::array<float, 3>& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
            m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

// The parts drawn on the vial. Both are unit meshes along -Y (renderer_vulkan/vr_inframe.cpp):
// the needle (MeshId::Needle) a shaft of radius 0.5 from y 0 to its bevelled point at y -1; the hub
// (MeshId::NeedleHub) radius 0.5 at y 0 narrowing toward y -1.
enum class NeedlePart { Hub, Needle };

// The needle's shaft starts this fraction of the hub's length inside the hub (no seam at the hub's
// tip, whatever its taper).
constexpr float NeedleIntoHub = 0.5f;

// The rows of a part on the vial whose rows are `vial` (view <- its model mesh, metres; its scale
// is the vial's, which the part takes too): mesh X and Z scaled by the part's diameter, Y by its
// length and pointing out of the stabbing end (-Y of the model for end -1, +Y for +1), its origin
// on the axis: the hub's root where the end is as wide as it, `inset_m` further in (InsetFor), the
// needle's half way into the hub, so
// `length_m` of it shows past the hub's tip.
inline Rows NeedlePartRows(const Rows& vial, const NeedleRoot& root, const NeedleLook& look,
                           NeedlePart part) {
    const float dir = look.end < 0.0f ? -1.0f : 1.0f; // out of the end, along the model's y
    const float hub_y =
        root.y - dir * (root.InsetFor(0.5f * look.hub_diameter_m) + look.inset_m);
    const float into = NeedleIntoHub * look.hub_length_m;
    const float start =
        part == NeedlePart::Hub ? hub_y : hub_y + dir * (look.hub_length_m - into);
    const float width = part == NeedlePart::Hub ? look.hub_diameter_m : look.diameter_m;
    const float len = part == NeedlePart::Hub ? look.hub_length_m : look.length_m + into;
    const float out = -dir; // the unit mesh's -Y goes out of the end
    const std::array<float, 3> origin = RowsPoint(vial, {root.x, start, root.z});
    Rows r{};
    for (int i = 0; i < 3; ++i) {
        r[i * 4 + 0] = vial[i * 4 + 0] * width;
        r[i * 4 + 1] = vial[i * 4 + 1] * len * out;
        r[i * 4 + 2] = vial[i * 4 + 2] * width;
        r[i * 4 + 3] = origin[static_cast<std::size_t>(i)];
    }
    return r;
}

// Contact only: 10 mm behind the hub end (previously 5 mm). In the hub's unit mesh,
// -Y points OUT toward the needle, so adding to y moves IN toward the syringe body.
// Metres in the vial's model frame; its transform applies pose and scale once.
inline constexpr float VialContactBackM = 0.010f;
inline std::array<float, 3> VialContactPoint(const Rows& hub, float hub_length_m,
                                          float back_m = VialContactBackM) {
    return RowsPoint(hub, {0.0f, -1.0f + back_m / std::max(hub_length_m, 1e-4f), 0.0f});
}

// The point of the needle (view): the unit mesh's (0, -1, 0).
inline std::array<float, 3> NeedlePoint(const Rows& needle_rows) {
    return RowsPoint(needle_rows, {0.0f, -1.0f, 0.0f});
}

// A point of the vial mesh (`p`, metres) in a part's unit mesh (rows from NeedlePartRows on
// `vial`): the shader reads the hand's light at the fist (the vial's grip, its mesh origin) through
// the part's own mesh coordinates. Exact inverse of the part's affine map.
inline std::array<float, 3> VialPointInPart(const Rows& vial, const Rows& part,
                                            const std::array<float, 3>& p) {
    const std::array<float, 3> v = RowsPoint(vial, p);
    const float d[3] = {v[0] - part[3], v[1] - part[7], v[2] - part[11]};
    // Solve R x = d (R = the part's 3 x 3) by Cramer's rule.
    const float a = part[0], b = part[1], c = part[2];
    const float e = part[4], f = part[5], g = part[6];
    const float h = part[8], i = part[9], j = part[10];
    const float det = a * (f * j - g * i) - b * (e * j - g * h) + c * (e * i - f * h);
    if (std::abs(det) < 1e-24f) {
        return {0.0f, 0.0f, 0.0f};
    }
    const float x = (d[0] * (f * j - g * i) - b * (d[1] * j - g * d[2]) + c * (d[1] * i - f * d[2]));
    const float y = (a * (d[1] * j - g * d[2]) - d[0] * (e * j - g * h) + c * (e * d[2] - d[1] * h));
    const float z = (a * (f * d[2] - d[1] * i) - b * (e * d[2] - d[1] * h) + d[0] * (e * i - f * h));
    return {x / det, y / det, z / det};
}

// When the needle is drawn: exactly while our vial is drawn in the hunter's hand. `ours`: our
// model s10002 this camera update (vial_inject_drain: armed, drawn, draining, the empty hold);
// false when it is put away, hidden for the gun or a two-handed attack, in a menu, a cinematic, an
// event or an item's own animation (core/vr/held_item.h decides those for the vial itself), and
// while the game's own vial is in his hand (the drain off: see above). `model`: the vial model is
// loaded (its root is known).
struct NeedleInputs {
    bool setting{true}; // vial_needle
    bool tuned_off{false}; // VR_TUNE_FILE needle.on=0 (tests)
    bool ours{false};
    bool model{false};
    bool menu{false}; // the camera record is a menu's
};
constexpr bool NeedleVisible(const NeedleInputs& in) {
    return in.setting && !in.tuned_off && in.model && !in.menu && in.ours;
}

// The needle's least half width on screen (pixels of the game's frame). The eye images resample
// the game's frame (about 1.16 x up over Link, then the late warp and the eye TAA), and a line
// under about 2 pixels wide comes out of that beaded (a dotted line at arm's length in the eye
// dumps): 2 pixels wide (plus the edge's pixel each side) at as much less opacity reads as one even,
// fine line.
constexpr float NeedleMinHalfPx = 1.0f;

// How round the wire is shaded (vr_inframe.frag ShadeSteelFn; 0 .. 1) at a drawn half width of
// `half_px` pixels: a highlight running down one side of a needle only a pixel or two wide is a
// streak under a pixel wide, which beads the same way. So up to 1 pixel of half width the needle is
// one even line (its normal toward the eye, the glint it would catch anywhere round it spread
// over its width), from 3 pixels it is fully round, in between a blend.
constexpr float NeedleRoundness(float half_px) {
    const float k = (half_px - 1.0f) * 0.5f;
    return k < 0.0f ? 0.0f : k > 1.0f ? 1.0f : k;
}

// The needle on screen (vr_inframe.vert / .frag, Shade::Steel as a wire): a ribbon facing the eye
// along its axis, one pixel wider on each side than the needle, whose coverage the fragment works
// out from its distance to the axis (a needle edge anti-aliased over one pixel, its normal
// rebuilt round the axis for the light): no stair steps, no crawl as it moves. Narrower than
// `min_half_px` pixels round its axis it is drawn that wide, its opacity scaled down by as much
// (the coverage it really has). `radius`: its radius (view units), `depth` its view depth, `p11`
// the projection's y scale, `height` the frame's height (pixels). Returns the widening factor
// (>= 1; opacity = 1 / it).
constexpr float NeedleWiden(float radius, float depth, float p11, float height,
                            float min_half_px) {
    if (radius <= 0.0f || depth <= 0.0f || p11 <= 0.0f || height <= 0.0f) {
        return 1.0f;
    }
    const float pixel = 2.0f * depth / (p11 * height); // one pixel at that depth (view units)
    const float want = min_half_px * pixel;
    return want > radius ? want / radius : 1.0f;
}

} // namespace VR::Held
