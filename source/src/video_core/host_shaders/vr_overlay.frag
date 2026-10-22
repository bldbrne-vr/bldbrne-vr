// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#version 450

// VR overlay meshes (video_core/renderer_vulkan/vr_overlay.cpp). Output is premultiplied alpha in
// linear light (the swapchain is sRGB); the OpenXR layer is submitted with
// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT over the game.

layout(location = 0) in vec3 in_world;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_aux;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform FrameData {
    mat4 view_proj[2];
    vec4 eye_pos[2];
    vec4 key_dir; // toward the light
    vec4 key_color;
    vec4 fill_dir;
    vec4 fill_color;
    vec4 sky; // w: exposure
    vec4 ground;
    vec4 rim_color;
    vec4 field; // what the headset shows: degrees left, right, up, down (0: unknown)
} fd;

layout(push_constant, std430) uniform DrawData {
    mat4 model;
    vec4 base;     // linear rgb, a = opacity
    vec4 emissive; // rgb, w = rim strength
    vec4 params;   // x roughness, y metallic, z rim / falloff power, w liquid level (mesh y)
    vec4 extra;    // x eye, y shade mode, z fade, w aux fade
} dd;

const int ShadeLit = 0;
const int ShadeGlass = 1;
const int ShadeLiquid = 2;
const int ShadeGhost = 3;
const int ShadeGlow = 4;
const int ShadeBeam = 5;
const int ShadeUnlit = 6;
const int ShadeVignette = 7;

const float PI = 3.14159265;
// Reflections see a brighter environment than the diffuse ambient (metal would read as black).
const float EnvBoost = 3.0;

float DistributionGGX(float NoH, float a) {
    const float a2 = a * a;
    const float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float VisibilitySmithGGX(float NoV, float NoL, float a) {
    const float a2 = a * a;
    const float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    const float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Karis' analytic fit of the split-sum environment BRDF.
vec3 EnvBRDFApprox(vec3 f0, float roughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    const vec4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    const vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

vec3 Hemisphere(vec3 dir) {
    return mix(fd.ground.rgb, fd.sky.rgb, 0.5 + 0.5 * dir.y);
}

// A tall strip light at the key light's azimuth: gives cylinders (glass, fittings) the vertical
// highlight streak that makes them read as glass and metal.
vec3 Panel(vec3 R, float roughness) {
    const vec2 p = normalize(fd.key_dir.xz + vec2(1e-4, 0.0));
    const float h = length(R.xz);
    const float facing = h > 1e-4 ? max(dot(R.xz / h, p), 0.0) : 0.0;
    const float alpha = max(roughness * roughness, 0.002);
    const float n = clamp(0.5 / (alpha * alpha), 2.0, 300.0);
    const float window = smoothstep(-0.5, -0.1, R.y) * (1.0 - smoothstep(0.55, 0.9, R.y));
    return fd.key_color.rgb * (0.3 * sqrt(n) * pow(facing, n) * window);
}

// Reflected environment: the hemisphere (brighter than the diffuse ambient) plus the panel.
vec3 Environment(vec3 R, float roughness) {
    return Hemisphere(R) * EnvBoost + Panel(R, roughness);
}

// GGX specular of one light (radiance, already times N.L); F returns the Fresnel term.
vec3 Specular(vec3 N, vec3 V, vec3 L, float NoV, float a, vec3 f0, out vec3 F) {
    const vec3 H = normalize(V + L);
    const float NoL = max(dot(N, L), 0.0);
    const float NoH = max(dot(N, H), 0.0);
    const float VoH = max(dot(V, H), 0.0);
    F = f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
    return DistributionGGX(NoH, a) * VisibilitySmithGGX(NoV, NoL, a) * F * NoL;
}

vec3 Direct(vec3 N, vec3 V, vec3 L, vec3 color, vec3 base, float roughness, float metallic,
            float NoV) {
    const float NoL = dot(N, L);
    if (NoL <= 0.0) {
        return vec3(0.0);
    }
    const vec3 f0 = mix(vec3(0.04), base, metallic);
    const float a = max(roughness * roughness, 0.002);
    vec3 F;
    const vec3 spec = Specular(N, V, L, NoV, a, f0, F);
    const vec3 diffuse = (1.0 - F) * (1.0 - metallic) * base / PI * NoL;
    return (diffuse + spec) * color;
}

vec4 Lit(vec3 N, vec3 V) {
    const vec3 base = dd.base.rgb;
    const float roughness = clamp(dd.params.x, 0.03, 1.0);
    const float metallic = clamp(dd.params.y, 0.0, 1.0);
    const float NoV = max(dot(N, V), 1e-3);
    const vec3 f0 = mix(vec3(0.04), base, metallic);
    vec3 rgb = Direct(N, V, fd.key_dir.xyz, fd.key_color.rgb, base, roughness, metallic, NoV) +
               Direct(N, V, fd.fill_dir.xyz, fd.fill_color.rgb, base, roughness, metallic, NoV);
    const vec3 R = reflect(-V, N);
    rgb += Hemisphere(N) * base * (1.0 - metallic);
    rgb += Environment(R, roughness) * EnvBRDFApprox(f0, roughness, NoV);
    rgb += dd.emissive.w * pow(1.0 - NoV, dd.params.z) * fd.rim_color.rgb;
    rgb += dd.emissive.rgb;
    const float a = dd.base.a;
    return vec4(rgb * a, a);
}

vec4 Glass(vec3 N, vec3 V) {
    const float NoV = max(dot(N, V), 1e-3);
    const float F = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);
    const float a = 0.06 * 0.06;
    vec3 Fs;
    vec3 spec = vec3(0.0);
    if (dot(N, fd.key_dir.xyz) > 0.0) {
        spec += Specular(N, V, fd.key_dir.xyz, NoV, a, vec3(0.04), Fs) * fd.key_color.rgb;
    }
    if (dot(N, fd.fill_dir.xyz) > 0.0) {
        spec += Specular(N, V, fd.fill_dir.xyz, NoV, a, vec3(0.04), Fs) * fd.fill_color.rgb;
    }
    const vec3 env = (Hemisphere(reflect(-V, N)) * 4.0 + Panel(reflect(-V, N), 0.05)) * F;
    const float rim = dd.emissive.w * pow(1.0 - NoV, dd.params.z);
    const float alpha = clamp(dd.base.a + 0.55 * rim + 0.3 * F, 0.0, 0.85);
    // Premultiplied: the specular and the reflection are added on top of the coverage.
    const vec3 rgb = dd.base.rgb * alpha * Hemisphere(N) * 0.5 + spec + env + rim * fd.rim_color.rgb;
    return vec4(rgb, alpha);
}

vec4 Liquid(vec3 N, vec3 V, bool front) {
    // Cut by a horizontal plane through the level point, so the surface stays level when the
    // vial tilts; the back faces seen through the cut are the free surface.
    const vec3 level = (dd.model * vec4(0.0, dd.params.w, 0.0, 1.0)).xyz;
    if (in_world.y > level.y) {
        discard;
    }
    vec3 base = dd.base.rgb;
    if (!front) {
        N = vec3(0.0, 1.0, 0.0);
        base *= 1.4;
    }
    const float roughness = clamp(dd.params.x, 0.03, 1.0);
    const float a = max(roughness * roughness, 0.002);
    const float NoV = max(dot(N, V), 1e-3);
    // Thin toward the silhouette: light passing through glows there, the core stays deep.
    const float thin = 1.0 - NoV;
    vec3 rgb = vec3(0.0);
    for (int i = 0; i < 2; ++i) {
        const vec3 L = i == 0 ? fd.key_dir.xyz : fd.fill_dir.xyz;
        const vec3 color = i == 0 ? fd.key_color.rgb : fd.fill_color.rgb;
        const float wrap = max((dot(N, L) + 0.5) / 1.5, 0.0);
        rgb += base / PI * wrap * color;
        if (dot(N, L) > 0.0) {
            vec3 F;
            rgb += Specular(N, V, L, NoV, a, vec3(0.02), F) * color;
        }
        // Light shining through toward the viewer.
        rgb += color * base * (5.0 * (0.1 + 0.9 * thin)) * pow(clamp(dot(-V, L), 0.0, 1.0), 3.0);
    }
    rgb += Hemisphere(N) * base;
    rgb += Environment(reflect(-V, N), roughness) * EnvBRDFApprox(vec3(0.02), roughness, NoV);
    rgb += dd.emissive.rgb * (0.4 + 1.6 * thin);
    // Meniscus: a bright rim just under the surface.
    rgb += base * fd.key_color.rgb * 0.6 * (1.0 - smoothstep(0.0, 0.0012, level.y - in_world.y));
    return vec4(rgb * dd.base.a, dd.base.a);
}

vec4 Ghost(vec3 N, vec3 V) {
    const float rim = pow(1.0 - abs(dot(N, V)), dd.params.z);
    // aux fade: the forearm stub dissolves toward its far end.
    const float keep = 1.0 - smoothstep(0.1, 0.8, in_aux.y) * dd.extra.w;
    const float a = dd.base.a * (0.15 + 0.85 * rim) * keep;
    const vec3 rgb = (dd.base.rgb * (0.15 + 0.85 * rim) + dd.emissive.rgb) * a;
    return vec4(rgb, a);
}

vec4 Glow(vec3 N, vec3 V) {
    float g = pow(abs(dot(N, V)), dd.params.z);
    if (dd.params.w > -100.0) {
        // A halo around the blood: fades out above the blood level (params.w, mesh y).
        const float level = (dd.model * vec4(0.0, dd.params.w, 0.0, 1.0)).y;
        g *= 1.0 - smoothstep(level - 0.004, level + 0.004, in_world.y);
    }
    // Its alpha: the light's own brightness. Meta's runtime adds the light (the additive pass
    // keeps the layer's alpha there), SteamVR drops alpha-0 pixels (the grip guide, the laser's
    // dot and the vial's halo never showed there, 2026-10-15): with it they show.
    const vec3 rgb = dd.emissive.rgb * g;
    return vec4(rgb, clamp(max(rgb.r, max(rgb.g, rgb.b)), 0.0, 1.0));
}

vec4 Beam(vec3 N, vec3 V) {
    const float fall = pow(1.0 - clamp(in_aux.y, 0.0, 1.0), 1.5);
    const float core = pow(abs(dot(N, V)), 2.0);
    const float a = dd.base.a * fall * (0.3 + 0.7 * core);
    return vec4(dd.base.rgb * a + dd.emissive.rgb * core * fall, a);
}

void main() {
    const int mode = int(dd.extra.y + 0.5);
    const int eye = int(dd.extra.x + 0.5);
    if (mode == ShadeVignette) {
        // Heal and hit vignettes: aux.y is the glow's profile (0 inside, 1 toward the edges of
        // the view), to the power params.z (1: the heal vignette; 3: the hit vignette, only the
        // very edges).
        float profile = clamp(in_aux.y, 0.0, 1.0);
        const int vmode = int(dd.params.w + 0.5);
        if (vmode != 0) {
            // Its own profile by the angle off the view axis (aux.x = degrees / 90): the low-HP
            // dark (1: clear inside params.x, full from params.y, squared smoothstep) or the
            // rally's rim (2: a band between them, soft at both ends).
            const float angle = in_aux.x * 90.0;
            if (vmode == 3) {
                // Fitted to what the headset shows (fd.field, the eyes' union): per-axis angles
                // off the view axis over that side's half-angle, r = 1 at the displayed edge in
                // every direction; clear inside params.x, full from params.y. The ring's own
                // plane is the model's xy at 1 (its scale is the distance).
                const vec3 rel = transpose(mat3(dd.model)) * (in_world - dd.model[3].xyz);
                const vec2 t = rel.xy / max(dot(dd.model[0].xyz, dd.model[0].xyz), 1e-8);
                const vec4 f = fd.field.x > 1.0 ? fd.field : vec4(54.0, 54.0, 44.0, 55.0);
                const vec2 ang = degrees(atan(t));
                const vec2 q = vec2(ang.x / max(ang.x < 0.0 ? f.x : f.y, 1.0),
                                    ang.y / max(ang.y > 0.0 ? f.z : f.w, 1.0));
                profile = smoothstep(dd.params.x, dd.params.y, length(q));
            } else if (vmode == 1) {
                const float t = smoothstep(dd.params.x, dd.params.y, angle);
                profile = t * t;
            } else {
                profile = smoothstep(dd.params.x - 3.0, dd.params.x + 1.0, angle) *
                          (1.0 - smoothstep(dd.params.y - 1.0, dd.params.y + 3.0, angle));
            }
        }
        if (dd.params.z != 1.0) {
            profile = pow(profile, max(dd.params.z, 0.01));
        }
        if (dd.emissive.w > 0.0) {
            // The directional hit flash (core/vr/hit_direction.h RingWeight): weighted toward
            // emissive.xy around the view axis (x right, y up), a wedge of power emissive.z and
            // focus emissive.w (0 = the plain ring). The ring's own plane is the model's xy.
            const vec3 rel = transpose(mat3(dd.model)) * (in_world - dd.model[3].xyz);
            const float len = length(rel.xy);
            const float c = len > 1e-6 ? dot(rel.xy / len, dd.emissive.xy) : 0.0;
            const float wedge = pow(max(0.5 + 0.5 * c, 0.0), max(dd.emissive.z, 0.01));
            profile *= (1.0 - dd.emissive.w) + dd.emissive.w * wedge;
        }
        const float a = dd.base.a * profile * clamp(dd.extra.z, 0.0, 1.0);
        out_color = vec4(dd.base.rgb * a, a);
        return;
    }
    const vec3 V = normalize(fd.eye_pos[eye].xyz - in_world);
    vec3 N = normalize(in_normal);
    if (!gl_FrontFacing && mode != ShadeLiquid) {
        N = -N;
    }
    vec4 color;
    if (mode == ShadeGlass) {
        color = Glass(N, V);
    } else if (mode == ShadeLiquid) {
        color = Liquid(N, V, gl_FrontFacing);
    } else if (mode == ShadeGhost) {
        color = Ghost(N, V);
    } else if (mode == ShadeGlow) {
        color = Glow(N, V);
    } else if (mode == ShadeBeam) {
        color = Beam(N, V);
    } else if (mode == ShadeUnlit) {
        color = vec4(dd.base.rgb, 1.0);
    } else {
        color = Lit(N, V);
    }
    color *= clamp(dd.extra.z, 0.0, 1.0);
    color.rgb *= fd.sky.w;
    out_color = color;
}
