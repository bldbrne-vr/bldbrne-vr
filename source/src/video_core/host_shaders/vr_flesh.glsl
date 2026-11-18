// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The flesh of a Beast Claw cut. Not yet included by a
// pass: the cut corpse's own draw (the live render step) will. No texture: two value noises and a
// handful of ALU ops a pixel; caps cover a few hundred pixels.
//
// Its texture space (core/vr/sever_mesh.cpp Emit): `uv` is the cap's own plane in units of its
// ring's mean radius, the cut's bone at (0, 0). A neck's or a limb's stump shows the bone where the
// bone is, its marrow, and the meat round it; a ring far from the bone (a strand, a finger) is
// meat only. The same surface is the cut meshes' back faces (gl_FrontFacing false, given their own
// uv times a few, so far from any bone): wherever a hole was left open (a sheet, a branched edge),
// the inside reads as meat, never as the world behind it.
//
// FleshSurface: linear albedo; wet (0 .. 1) for the pass's specular, its strength and tightness;
// bend, the normal's tilt in the cap's plane (along uv's axes): the meat swells out of the cut,
// lumpy, and the bone stands proud. `fresh` (1 at the cut, easing to 0 over a few seconds): the
// newly cut meat is brighter and wetter, then darkens as it clots.

// Multiplies only (Hoskins' hash12).
float FleshHash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

// Smooth value noise (four hashes).
float FleshNoise(vec2 p) {
    const vec2 i = floor(p);
    const vec2 f = p - i;
    const vec2 s = f * f * (3.0 - 2.0 * f);
    return mix(mix(FleshHash(i), FleshHash(i + vec2(1.0, 0.0)), s.x),
               mix(FleshHash(i + vec2(0.0, 1.0)), FleshHash(i + vec2(1.0, 1.0)), s.x), s.y);
}

struct Flesh {
    vec3 albedo;
    float wet;
    vec2 bend;
};

Flesh FleshSurface(vec2 uv, float fresh) {
    const float r = length(uv);
    // Muscle: dark arterial red; its fibres (noise stretched across one axis) and clots.
    const float fibres = FleshNoise(uv * vec2(18.0, 5.0));
    const float clots = FleshNoise(uv * 6.0 + 17.0);
    const float meat = 0.65 * fibres + 0.35 * clots;
    // x0.35: the user's darker red (2026-11-13; core/vr/sever_flesh.h MeatShade).
    vec3 albedo = mix(vec3(0.13, 0.008, 0.007), vec3(0.40, 0.030, 0.024), meat) * 0.35;
    albedo *= 1.0 + 0.6 * fresh;
    // Bone: an off-white disc round (0, 0); the marrow dark red in its middle.
    const float bone = 1.0 - smoothstep(0.16, 0.20, r);
    const float marrow = 1.0 - smoothstep(0.05, 0.08, r);
    albedo = mix(albedo, vec3(0.70, 0.60, 0.46) * (0.8 + 0.2 * clots), bone);
    albedo = mix(albedo, vec3(0.18, 0.025, 0.015), marrow);
    // Far from the bone (a cut strand of hair, a finger, a back face): dark, clotted.
    albedo *= mix(1.0, 0.3, smoothstep(1.5, 3.0, r));
    Flesh f;
    f.wet = mix(mix(0.55, 0.95, fresh), 0.3, bone - marrow);
    // The swell: tilted away from the bone toward the rim, lumped by the clots; the bone's rim
    // a ridge.
    const vec2 out_dir = r > 1e-4 ? uv / r : vec2(0.0);
    const float swell = 0.3 * smoothstep(0.2, 1.0, r) - 0.5 * (bone - marrow) * smoothstep(0.1, 0.2, r);
    f.bend = out_dir * swell + 0.25 * (vec2(fibres, clots) - 0.5);
    f.albedo = albedo;
    return f;
}
