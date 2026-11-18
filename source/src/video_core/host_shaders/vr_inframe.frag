// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#version 450

// In-frame items (video_core/renderer_vulkan/vr_inframe.cpp). The target is an sRGB view of the
// game's scene colour, so this writes linear light (premultiplied alpha, ONE / ONE_MINUS_SRC_ALPHA)
// and the hardware encodes it like the scene. Texels with almost no coverage are discarded, so
// they write no depth either (DIBR then shows what is behind them at its own depth). The second
// output is the coverage (blended MAX): the DIBR warp keeps the HUD's edges from cutting our items.
// The pass is single-sampled after the game's AA: the cards' round edges are anti-aliased here
// (analytic, from the mesh-space radius), and the icons and labels, which have no mips, are
// box-filtered over each pixel's footprint.

layout(set=0,binding=3) uniform sampler2D model_surface;
layout(location=5) in vec4 in_vertex_color;
layout(location = 0) in vec3 in_view;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_local;
layout(location = 4) flat in vec2 in_anchor; // Shade::Model: the holding hand (frame uv), -1 none

layout(location = 0) out vec4 out_color;
layout(location=6) flat in int in_gray;
layout(location = 1) out vec4 out_cover;

layout(set = 0, binding = 0) uniform sampler2D tex;
// The scene's light: the scene colour box-filtered into a mip chain (VrInFrame::ProbeScenes); its
// texels hold the scene's sRGB-encoded values (the game's own encoding), maybe B G R.
layout(set = 0, binding = 1) uniform sampler2D probe;

layout(push_constant, std430) uniform DrawData {
    vec4 mv[3];
    vec4 color;    // linear rgb, a
    vec4 emissive; // rgb added (Text: the halo colour, a = its opacity; Card: the frame)
    vec4 uv;
    vec4 proj;
    vec4 extra;    // x shade (+ Edge* flags), y exposure, z / w 1 / the frame's size (z < 0: the
                   // probe is B G R; both 0: no probe)
} dd;

// The scene's light around this pixel, linear (a 64 x 64 pixel average), and its brightness.
vec3 SceneLight(out float luma) {
    if (dd.extra.w == 0.0) {
        luma = 0.25;
        return vec3(0.25);
    }
    const vec2 uv = gl_FragCoord.xy * vec2(abs(dd.extra.z), dd.extra.w);
    vec3 c = textureLod(probe, uv, 5.0).rgb;
    if (dd.extra.z < 0.0) {
        c = c.bgr;
    }
    c = pow(c, vec3(2.2));
    luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return c;
}

const int ShadeLit = 0;
const int ShadeUnlit = 1;
const int ShadeIcon = 2;
const int ShadeText = 3;
const int ShadeModel = 4;
const int ShadeCard = 5;
const int ShadeGauge = 6;
const int ShadeHoldRing = 7;
const int ShadeVialDrain = 8;
const int ShadeSteel = 9;
const int ShadeSyringe = 10;
const int ShadeMolotov = 11;
const int ShadeApothecary = 12;
const int ShadeGoods = 13;
const int ShadeNativeLiquid = 14;
const int ShadeFan = 15;
// Silhouettes in the mesh's xy plane (the card faces the eye): a disc of radius 0.5 (the plate),
// a ring between radii 0.47 and 0.53 (the rim).
const int EdgeDisc = 16;
const int EdgeRing = 32;
// ... both (never a card's edge): a candle (Item::candle; CandleItem).
const int Candle = EdgeDisc | EdgeRing;

// The texture box-filtered over this pixel's footprint: four bilinear taps a quarter of it apart
// (a 4 x 4 texel box at 4x minification; one tap when not minified). `premultiply`: averaged as
// premultiplied colour (the icons' transparent texels have no colour to bleed). The textures have
// one level: explicit LOD 0 (no implicit derivatives in the per-pixel branch).
vec4 Footprint(vec2 uv, bool premultiply) {
    const vec2 size = vec2(textureSize(tex, 0));
    const vec2 dx = dFdx(uv);
    const vec2 dy = dFdy(uv);
    const float texels = max(length(dx * size), length(dy * size));
    const float k = clamp(texels - 1.0, 0.0, 1.0) * 0.25;
    if (k <= 0.0) {
        return textureLod(tex, uv, 0.0);
    }
    vec4 acc = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        const vec2 o = (float(i & 1) * 2.0 - 1.0) * dx + (float(i >> 1) * 2.0 - 1.0) * dy;
        const vec4 t = textureLod(tex, uv + o * k, 0.0);
        acc += premultiply ? vec4(t.rgb * t.a, t.a) : t;
    }
    acc *= 0.25;
    if (premultiply) {
        acc.rgb = acc.a > 1e-4 ? acc.rgb / acc.a : vec3(0.0);
    }
    return acc;
}

// Coverage of the round silhouette at this pixel (1 inside, a one-pixel ramp at the edge).
float EdgeCoverage(int edge) {
    const float r = length(in_local.xy);
    const float fw = max(fwidth(r), 1e-5);
    if (edge == EdgeDisc) {
        return clamp((0.5 - r) / fw, 0.0, 1.0);
    }
    return clamp(min(0.53 - r, r - 0.47) / fw, 0.0, 1.0);
}

// A game model (or a held item's card icon): its diffuse texture lit like the hand that holds it.
// The game lights the hunter (his own light, the room's): the scene's colour at the hand (the
// probe, a 16 x 16 pixel average there, before our items were drawn) over a skin's albedo (0.5) is
// the light falling on it, and so on the item in his fist; the light behind the fragment over an
// average albedo is the floor of it (a hand in a glove or in the shade). So the item is as bright
// as the hand holding it, in a lit street and in the clinic's dark alike, tinted by the light
// round it, never glowing. A head-relative key from above shapes it; emissive is the grab's
// confirmation (a brighten and a soft warm halo at the silhouette).
// The light on a held model at this fragment (ShadeGameModel's): its exposure `e` (the hand's
// light) and the tint of the light round it.
float HeldLight(out vec3 tint) {
    float scene_luma;
    const vec3 scene = SceneLight(scene_luma);
    float e = scene_luma / 0.2;
    vec3 tint_src = scene;
    float tint_luma = scene_luma;
    if (in_anchor.x >= 0.0 && dd.extra.w != 0.0) {
        vec3 hand = textureLod(probe, clamp(in_anchor, 0.0, 1.0), 3.5).rgb;
        vec3 round = textureLod(probe, clamp(in_anchor, 0.0, 1.0), 5.0).rgb;
        if (dd.extra.z < 0.0) {
            hand = hand.bgr;
            round = round.bgr;
        }
        hand = pow(hand, vec3(2.2));
        round = pow(round, vec3(2.2));
        const float hand_luma = dot(hand, vec3(0.2126, 0.7152, 0.0722));
        e = max(e, hand_luma / 0.50);
        tint_src = round;
        tint_luma = dot(round, vec3(0.2126, 0.7152, 0.0722));
    }
    tint = mix(vec3(1.0), clamp(tint_src / max(tint_luma, 1e-3), 0.6, 1.6), 0.4);
    return clamp(e, 0.15, 1.3);
}

// A candle (Item::candle; core/vr/hand_lantern.h): this item is the Hand Lantern lit.
bool CandleItem() {
    return (int(dd.extra.x + 0.5) & (EdgeDisc | EdgeRing)) == Candle;
}

// ... on its model: the lantern's cage (s10021: bars round an open middle, a base and a cap) lit
// from inside by the flame on its axis (mesh (0, -0.0105, 0)). What faces the flame (the base,
// the cap's underside, the far bars' inner sides) warms, less with distance; what faces away
// (the bars in front of it, their outer sides) stays dark against it. `n`: the normal toward the
// eye; emissive.x: the light's strength (its flicker in it).
vec3 CandleLight(vec3 albedo, vec3 n) {
    const vec4 flame = vec4(0.0, -0.0105, 0.0, 1.0);
    const vec3 to =
        vec3(dot(dd.mv[0], flame), dot(dd.mv[1], flame), dot(dd.mv[2], flame)) - in_view;
    // (Never a NaN into the scene, the upscalers' history, even at the flame itself.)
    const float facing = max(dot(n, to), 0.0) * inversesqrt(max(dot(to, to), 1e-12));
    const vec3 d = in_local - flame.xyz;
    const float falloff = 1.0 / (1.0 + dot(d, d) / (0.018 * 0.018));
    return albedo * vec3(1.0, 0.55, 0.22) * (7.0 * max(dd.emissive.x, 0.0) * facing * falloff);
}

// ... on a card: its flame, a small teardrop of fire in a faint halo, on a quad 1.4 times as tall
// as wide facing the eye, its y up the candle (uv.x: the strength, its flicker in it; uv.y: the
// flame's height, 1 = as drawn). Light added over what is behind, covering little of it (rgb is
// the light over the coverage: main premultiplies). Depth-tested in the cage's middle: the bars
// in front of it hide it, the gaps show it.
vec4 CandleFlame() {
    // In card widths, from the flame's widest point (a little above the wick).
    const vec2 p = vec2(in_local.x, in_local.y * 1.4 + 0.10);
    const float height = max(dd.uv.y, 0.5);
    const float up = clamp(p.y / (0.27 * height), 0.0, 1.0);
    const float w = 0.092 * mix(1.0, 0.40, up);
    const float h = p.y > 0.0 ? 0.27 * height : 0.11;
    const float fire = exp(-p.x * p.x / (w * w) - p.y * p.y / (h * h));
    const float halo = exp(-dot(p, p) / (0.25 * 0.25));
    const vec3 amber = vec3(1.0, 0.50, 0.17);
    const vec3 core = vec3(1.0, 0.88, 0.62);
    const vec3 light = (mix(amber, core, smoothstep(0.35, 0.95, fire)) * (1.6 * fire) +
                        amber * (0.18 * halo)) * max(dd.uv.x, 0.0);
    const float alpha = min(0.5 * sqrt(dot(light, vec3(0.2126, 0.7152, 0.0722))), 0.45);
    return vec4(light / max(alpha, 0.03), alpha);
}

vec4 ShadeGameModel(float exposure, bool liquid_data) {
    float scene_luma;
    const vec3 scene = SceneLight(scene_luma);
    float e = scene_luma / 0.2;
    vec3 tint_src = scene;
    float tint_luma = scene_luma;
    if (in_anchor.x >= 0.0 && dd.extra.w != 0.0) {
        vec3 hand = textureLod(probe, clamp(in_anchor, 0.0, 1.0), 3.5).rgb;
        vec3 round = textureLod(probe, clamp(in_anchor, 0.0, 1.0), 5.0).rgb;
        if (dd.extra.z < 0.0) {
            hand = hand.bgr;
            round = round.bgr;
        }
        hand = pow(hand, vec3(2.2));
        round = pow(round, vec3(2.2));
        const float hand_luma = dot(hand, vec3(0.2126, 0.7152, 0.0722));
        e = max(e, hand_luma / 0.50);
        tint_src = round;
        tint_luma = dot(round, vec3(0.2126, 0.7152, 0.0722));
    }
    e = clamp(e, 0.15, 1.3);
    const bool animated=!liquid_data && dd.uv.z<0.0 && dd.uv.w>0.0;
    vec4 surface=vec4(0.5,0.5,1.0,0.8);
    vec4 t;
    if(animated) {
        const int material=clamp(int(floor(in_uv.x/16.0)),0,3);
        const vec2 base_uv=vec2(in_uv.x-float(material)*16.0,in_uv.y);
        const vec2 tile=vec2(material&1,material>>1)*0.5;
        const bool single_surface=dd.uv.z < -1.5;
        const vec2 uv=single_surface?base_uv:tile+(vec2(16.0)+fract(base_uv)*224.0)/512.0;
        const float uv_scale=single_surface?1.0:(224.0/512.0);
        const vec2 dx=dFdx(base_uv)*uv_scale,dy=dFdy(base_uv)*uv_scale;
        t=textureGrad(tex,uv,dx,dy);
        surface=textureGrad(model_surface,uv,dx,dy);
        t.rgb*=in_vertex_color.rgb;
    } else t=texture(tex,in_uv);
    float alpha = (liquid_data ? 1.0 : dd.color.a) * smoothstep(0.2, 0.55, t.a);
    vec3 n = normalize(in_normal);
    const vec3 v = normalize(-in_view);
    if (dot(n, v) < 0.0) {
        n = -n; // two-sided (no culling: the view space is left-handed)
    }
    if(animated && in_vertex_color.w>0.0) {
        const vec3 dp1=dFdx(in_view),dp2=dFdy(in_view);
        const vec2 duv1=dFdx(in_uv),duv2=dFdy(in_uv);
        const vec3 p2=cross(dp2,n),p1=cross(n,dp1);
        const vec3 tangent=p2*duv1.x+p1*duv2.x,bitangent=p2*duv1.y+p1*duv2.y;
        const float length2=max(dot(tangent,tangent),dot(bitangent,bitangent));
        if(length2>1e-20) {
            vec3 mapped=surface.rgb*2.0-1.0;mapped.xy*=in_vertex_color.w;
            n=normalize((tangent*mapped.x+bitangent*mapped.y)*inversesqrt(length2)+n*mapped.z);
        }
    }
    const vec3 albedo = t.rgb * (liquid_data ? vec3(1.0) : dd.color.rgb);
    const vec3 tint =
        mix(vec3(1.0), clamp(tint_src / max(tint_luma, 1e-3), 0.6, 1.6), 0.4);
    const vec3 l = normalize(vec3(-0.30, 0.80, -0.45));
    const float diffuse = max(dot(n, l), 0.0);
    const vec3 h = normalize(l + v);
    const float rough=clamp(surface.a,0.05,1.0);
    const float specular = pow(max(dot(n, h), 0.0), animated?mix(120.0,12.0,rough):28.0) * (animated?mix(.20,.025,rough):.18);
    const float facing = max(dot(n, v), 0.0);
    vec3 rgb = (albedo * (0.55 + 0.75 * diffuse) + vec3(specular)) * tint * e * exposure;
    // The game grades its frame toward grey-blue; the item's raw texture is a little richer than
    // what it shows: 18% of its colour goes, so it sits in the scene instead of popping out of it.
    rgb = mix(vec3(dot(rgb, vec3(0.2126, 0.7152, 0.0722))), rgb, 0.82);
    // A candle inside it (Item::candle): its emissive is the candle's, not the glow's.
    if (!liquid_data && CandleItem()) {
        return vec4(rgb + CandleLight(albedo, n), alpha);
    }
    // The glow (warm, soft): the item brightens a little and a halo of warm light rises at its
    // silhouette, its own texture always readable through it.
    const float rim = pow(1.0 - facing, 2.0);
    // The glove's XYZ contains motion, not light; its W is the confirmed summon flash (0..1), or
    // while it charges 2 + its charge glow (bloodborne_quickitems.cpp): its own symbols (the
    // sigils and engraved strokes of its texture, the mask in its albedo's alpha)
    // light from within, hotter than linearly as it
    // gathers, then pulse once full, until it is thrown; the leather stays dark, a faint warm
    // edge only (2026-11-04: the whole-glove tint before read as a flat khaki sheet in the mock).
    const bool glove=animated && dd.uv.z < -2.5 && dd.uv.z > -3.5;
    if (glove && dd.emissive.w > 1.5) {
        const float charged = clamp(dd.emissive.w - 2.0, 0.0, 1.0);
        const float hot = charged * clamp((1.0 - t.a) * (1.0 / 0.375), 0.0, 1.0);
        const vec3 ember = vec3(1.0, 0.62, 0.22);
        rgb = rgb * (1.0 + 0.3 * hot) + ember * (0.9 * hot + 2.4 * hot * hot) +
              ember * charged * 0.06 * rim;
        return vec4(rgb, alpha);
    }
    const vec3 emission=glove ? vec3(1.0,.025,.008)*clamp(dd.emissive.w,0.0,1.0)*.35
                              : dd.emissive.rgb;
    const float glow = liquid_data ? 0.0 : max(emission.r, max(emission.g, emission.b));
    rgb = rgb * (1.0 + 0.8 * glow) + (liquid_data ? vec3(0.0) : emission) * (0.10 + 0.90 * rim);
    return vec4(rgb, alpha);
}

// vial_inject_drain (core/vr/vial_drain.h): the Blood Vial's own model (s10002) shaded as
// ShadeGameModel shades it (its texture under the hand's light, the key, the specular: the glass
// keeps its look), with its blood level. Mesh y below uv.x (the level) inside the column uv.y ..
// uv.z is dark blood (color.rgb under the same light, over the glass at opacity color.a), its
// surface a thin darker meniscus; above it, where the blood was, a faint red film on the glass
// with a few streaks round the axis (emissive.xy: the axis' mesh x, z; emissive.z: the film's
// strength), fading upward. uv.w: the fill (1 full .. 0 empty).
vec4 ShadeVialDrainFn(float exposure) {
    vec3 tint;
    const float e = HeldLight(tint);
    const vec4 t = texture(tex, in_uv);
    const float alpha = smoothstep(0.2, 0.55, t.a);
    vec3 n = normalize(in_normal);
    const vec3 v = normalize(-in_view);
    if (dot(n, v) < 0.0) {
        n = -n;
    }
    const vec3 l = normalize(vec3(-0.30, 0.80, -0.45));
    const float diffuse = max(dot(n, l), 0.0);
    const vec3 h = normalize(l + v);
    const float specular = pow(max(dot(n, h), 0.0), 28.0) * 0.18;
    const float light = (0.55 + 0.75 * diffuse) * e * exposure;
    const vec3 spec = vec3(specular) * e * exposure;
    // The game shows this model (FFX 173) as a dark slate glass: its texture graded toward blue
    // (measured against the game's own vial in the same light, eye dumps 2026-10-06).
    const vec3 grade = vec3(0.50, 0.68, 1.0);
    vec3 glass = (t.rgb * grade * light + spec) * tint;
    glass = mix(vec3(dot(glass, vec3(0.2126, 0.7152, 0.0722))), glass, 0.82);

    const float y = in_local.y;
    const float level = dd.uv.x;
    const float lo = dd.uv.y;
    const float hi = dd.uv.z;
    const float span = max(hi - lo, 1e-4);
    const float fw = max(fwidth(y), 1e-5);
    const float column = clamp((y - lo) / fw + 0.5, 0.0, 1.0) * clamp((hi - y) / fw + 0.5, 0.0, 1.0);
    const float below = level - y; // > 0: in the blood
    const float blood = column * clamp(below / fw + 0.5, 0.0, 1.0);
    vec3 blood_rgb = (dd.color.rgb * light + spec) * tint;
    // The meniscus: the blood's top 3% of the column a darker, browner red (foam, the curved
    // surface), a hairline wide at the least.
    const float men_w = max(0.03 * span, 1.5 * fw);
    const float men = blood * (1.0 - smoothstep(0.0, men_w, below));
    blood_rgb *= mix(vec3(1.0), vec3(0.42, 0.30, 0.30), men);
    vec3 rgb = mix(glass, blood_rgb, blood * dd.color.a);
    // The film: above the level, up to where the blood was, a faint red left on the glass, a few
    // streaks round the axis, strongest just above the surface.
    const float above = y - level;
    const float drained = column * clamp(above / fw + 0.5, 0.0, 1.0);
    const float a = atan(in_local.x - dd.emissive.x, in_local.z - dd.emissive.y);
    const float streak = 0.5 + 0.5 * sin(a * 7.0 + 1.7 * sin(a * 3.0 + y * 90.0));
    const float film = drained * dd.emissive.z * (0.45 + 0.55 * streak) *
                       (0.35 + 0.65 * exp(-max(above, 0.0) / (0.25 * span)));
    rgb = mix(rgb, (dd.color.rgb * 1.6 * light + spec) * tint, clamp(film, 0.0, 1.0));
    return vec4(rgb, alpha);
}

// The procedural scratch syringe shades its chamber as glass
// around an analytic blood volume. The transmitted ray is clipped against the cylinder and
// the damped free-surface plane, so the visible top has depth instead of a painted red stripe.
float VialNoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    const vec3 k = vec3(127.1, 311.7, 74.7);
    float a = fract(sin(dot(i, k)) * 43758.5453);
    float b = fract(sin(dot(i + vec3(1,0,0), k)) * 43758.5453);
    float c = fract(sin(dot(i + vec3(0,1,0), k)) * 43758.5453);
    float d = fract(sin(dot(i + vec3(1,1,0), k)) * 43758.5453);
    float e = fract(sin(dot(i + vec3(0,0,1), k)) * 43758.5453);
    float g = fract(sin(dot(i + vec3(1,0,1), k)) * 43758.5453);
    float h = fract(sin(dot(i + vec3(0,1,1), k)) * 43758.5453);
    float j = fract(sin(dot(i + vec3(1,1,1), k)) * 43758.5453);
    return mix(mix(mix(a,b,f.x),mix(c,d,f.x),f.y),
               mix(mix(e,g,f.x),mix(h,j,f.x),f.y),f.z);
}

// The original-size apothecary bottle: dark green glass, a worn paper seal and folded linen.
// One opaque material pass; aux.x >= 2 marks the cloth in the procedural mesh.
float MolotovInkSegment(vec2 p, vec2 a, vec2 b) {
    const vec2 line=b-a;
    return length(p-a-line*clamp(dot(p-a,line)/dot(line,line),0.0,1.0));
}

vec4 ShadeMolotovFn(float exposure) {
    vec3 tint;
    const float e = HeldLight(tint)*exposure;
    vec3 n = normalize(in_normal);
    const vec3 v = normalize(-in_view);
    if(dot(n,v)<0.0) n=-n;
    const vec3 l=normalize(vec3(-0.30,0.80,-0.45));
    const vec3 h=normalize(l+v);
    const float facing=max(dot(n,v),0.0);
    const float diffuse=0.48+0.78*max(dot(n,l),0.0);
    const vec3 p=in_local;
    const float age=VialNoise(p*135.0+vec3(4.2,9.1,2.7));
    const float grain=VialNoise(p*1650.0);
    const float detail=1.0-smoothstep(0.00035,0.0015,length(fwidth(p)));
    const float heel=1.0-smoothstep(-.096,-.065,p.y);
    // Broad cloudy glass under irregular earthen deposits, with finer broken edges.
    // Glass itself does not rust: these are brown iron/mineral stains and caked grime.
    const float cloud=VialNoise(p*48.0+vec3(7.1,2.3,5.4));
    const float crust=VialNoise(p*390.0+vec3(1.9,6.2,8.7));
    const float runs=VialNoise(p*vec3(245.0,36.0,245.0)+vec3(2,5,1));
    const float field=cloud*.54+age*.28+crust*.18;
    const float stains=smoothstep(.43,.61,field+heel*.065);
    const float haze=smoothstep(.30,.68,cloud*.6+age*.4);
    const float grime=clamp(heel*.4+stains*.65,0.0,.90);
    vec3 albedo=mix(vec3(.052,.007,.005),vec3(.180,.030,.016),haze*.72);
    vec3 oxide=mix(vec3(.009,.005,.004),vec3(.070,.023,.012),age*.7+crust*.3);
    oxide*=.65+.45*crust+.14*(grain-.5)*detail;
    albedo=mix(albedo,oxide,stains*.82);
    const float streaks=smoothstep(.58,.76,runs)*(0.3+0.7*stains);
    albedo=mix(albedo,vec3(.012,.010,.006),streaks*.46+heel*.14);
    albedo*=.88+.24*crust;
    // Dark, aged brown glass: the original bottle carries its deepest tones in the lower body.
    const float body_light=mix(.23,.78,smoothstep(-.075,.060,p.y));
    albedo*=body_light*mix(.52,1.0,smoothstep(.28,.62,age));
    const float nh=max(dot(n,h),0.0);
    // Cloudy regions spread the reflection; thick dry deposits almost extinguish it.
    float spec=(pow(nh,mix(85.0,24.0,haze))*.23+pow(nh,12.0)*.018)*
               (1.0-.90*stains)*(1.0-.45*heel)*mix(.55,1.0,body_light);
    vec3 rim=vec3(.065,.014,.009)*pow(1.0-facing,3.0)*(1.0-.7*grime)*body_light;
    // Oval paper apothecary seal on the front; no external texture or unreadable fake lettering.
    const vec2 label=vec2(p.x/.02668,(p.y+.016)/.036);
    const float edge=length(label)+(age-.5)*.11+(grain-.5)*.018*detail;
    const float fw=max(fwidth(edge),.008);
    const float paper=(1.0-smoothstep(1.0-fw,1.0+fw,edge))*step(p.z,-.027);
    const float border=max(1.0-smoothstep(.006,.016,abs(length(label)-.84)),
                           1.0-smoothstep(.004,.011,abs(length(label)-.78)));
    // Alchemical-style fire triangle over a cross, enclosed by a fine circle.
    // Analytic line distances keep the small ink mark readable without a texture allocation.
    const vec2 emblem=vec2(label.x,label.y);
    float mark=abs(length(emblem)-.56);
    mark=min(mark,MolotovInkSegment(emblem,vec2(-.26,.04),vec2(0,.44)));
    mark=min(mark,MolotovInkSegment(emblem,vec2(0,.44),vec2(.26,.04)));
    mark=min(mark,MolotovInkSegment(emblem,vec2(.26,.04),vec2(-.26,.04)));
    mark=min(mark,MolotovInkSegment(emblem,vec2(0,.04),vec2(0,-.41)));
    mark=min(mark,MolotovInkSegment(emblem,vec2(-.18,-.24),vec2(.18,-.24)));
    const float ink_fw=max(fwidth(mark),.005);
    const float crest=1.0-smoothstep(.016-ink_fw,.016+ink_fw,mark);
    const float ink=max(border,crest)*(.7+.3*age);
    vec3 paper_rgb=mix(vec3(.18,.141,.078),vec3(.065,.048,.024),age*.60+heel*.2);
    paper_rgb=mix(paper_rgb,vec3(.036,.032,.019),ink*.85);
    paper_rgb*=.78+.25*grain+.12*age;
    paper_rgb=mix(paper_rgb,vec3(.038,.028,.014),smoothstep(.55,.82,age)*.45);
    albedo=mix(albedo,paper_rgb,paper);
    spec*=1.0-.96*paper;rim*=1.0-paper;
    if(in_uv.x>=1.5) {
        const float threads=sin(p.x*10500.0)*sin(p.z*10500.0);
        const float charred=smoothstep(.026,.039,p.x)*.62;
        albedo=mix(vec3(.26,.223,.158),vec3(.027,.022,.016),charred);
        albedo*=.68+.32*age+.10*threads*detail;
        spec=pow(nh,12.0)*.009;rim=vec3(0);
    }
    vec3 rgb=(albedo*dd.color.rgb*diffuse+vec3(spec)+rim)*tint*e;
    const float glow=max(dd.emissive.r,max(dd.emissive.g,dd.emissive.b));
    rgb=rgb*(1.0+.8*glow)+dd.emissive.rgb*(.10+.90*pow(1.0-facing,2.0));
    return vec4(rgb,dd.color.a);
}

// Case materials remain opaque and share the scene's lighting / exposure.
vec4 ShadeApothecaryFn(float exposure) {
    vec3 tint; const float e=HeldLight(tint)*exposure;
    vec3 n=normalize(in_normal), v=normalize(-in_view);
    if(dot(n,v)<0.0) n=-n;
    const vec3 l=normalize(vec3(-.30,.80,-.45));
    const float nh=max(dot(n,normalize(l+v)),0.0);
    const vec3 p=in_local;
    const float broad=VialNoise(p*55.0+vec3(2,7,9));
    const float grain=VialNoise(p*1700.0);
    const float detail=1.0-smoothstep(.0004,.002,length(fwidth(p)));
    vec3 albedo=mix(vec3(.017,.009,.005),vec3(.087,.037,.014),broad);
    float spec=.015*pow(nh,20.0);
    if(in_uv.x<.5) {
        albedo*=.80+.35*grain*detail;
        // Worn seams along the front lip, binding, and two reinforcing straps.
        const float lip=abs(p.y-.016);
        const float strap=min(abs(abs(p.x)-.031),abs(abs(p.x)-.043));
        const float seam=min(lip,strap);
        const float fw=max(fwidth(seam),.0002);
        const float stitch=(1.0-smoothstep(.0005-fw,.0005+fw,seam))*
             smoothstep(.35,.65,sin((lip<strap?p.x:p.y)*1250.0)) * step(p.z,-.050);
        albedo=mix(albedo,vec3(.21,.138,.068),stitch*.65);
        albedo=mix(albedo,vec3(.006,.004,.003),smoothstep(.57,.77,broad)*.55);
    } else if(in_uv.x<1.5) {
        const float tarnish=smoothstep(.35,.68,broad);
        albedo=mix(vec3(.13,.091,.040),vec3(.020,.019,.009),tarnish);
        spec=pow(nh,42.0)*.19*(1.0-.85*tarnish);
    } else if(in_uv.x<2.5) {
        albedo=vec3(.030,.016,.009)*(.75+.45*grain*detail);
        spec=0.0;
    } else if(in_uv.x<3.5) {
        albedo=mix(vec3(.032,.0018,.002),vec3(.105,.012,.009),broad);
        albedo=mix(albedo,vec3(.048,.034,.015),smoothstep(.57,.75,broad)*.65);
        spec=pow(nh,68.0)*.22*(1.0-.7*broad);
    } else {
        // The quick-item paper bundle: matte fibres, uneven stains and worn fold lines.
        const float stains=VialNoise(p*175.0+vec3(9,2,4));
        const float fold=exp(-abs(p.x+.012)*1200.0);
        const float edge=1.0-smoothstep(.001,.008,min(.035-abs(p.x),.05-abs(p.y)));
        albedo=vec3(.72+.44*grain*detail)*(1.0-.42*smoothstep(.45,.78,stains));
        albedo*=1.0-.30*edge-.17*fold;
        spec=0.0;
    }
    const float diffuse=.40+.80*max(dot(n,l),0.0);
    vec3 rgb=(albedo*diffuse+spec)*tint*e;
    // A restrained highlight on touch, driven by the same target as the grip test.
    rgb+=dd.emissive.rgb*.10;
    return vec4(rgb*dd.color.rgb,dd.color.a);
}

// Two octaves of VialNoise, narrowed to the spread of the noise the approved previews were tuned
// with (std 0.075 round 0.5), so their thresholds cover as much here.
float GoodsNoise(vec3 p) {
    return 0.5+((VialNoise(p)+0.55*VialNoise(p*2.03+7.1))/1.55-0.5)*0.55;
}

// Messenger's Gift only: ivory, worn gilded crown/filigree and a garnet forehead seal.
// Shared with the offline material preview. Object-space masks need no extra texture/draw.
vec4 MessengerGiftSurface(vec3 p, float cav, out float metal, out float height) {
    float footprint = max(length(fwidth(p)), .00005);
    float detail = 1.0 - smoothstep(.0004, .002, footprint);
    float broad = GoodsNoise(p * 55.0 + vec3(2,7,1));
    float grain = GoodsNoise(p * 750.0);
    vec2 radial = vec2(p.x, p.z + .012);
    float angle = dot(radial,radial)>1e-10 ? atan(radial.x,radial.y) : 0.0;
    float crown = p.y - (.045 + .007 * cos(angle * 8.0));
    float band = 1.0 - smoothstep(.0022, .0022 + footprint, abs(crown));
    float loop = length(vec2(sin(angle * 4.0) * .019, p.y - .070)) - .017;
    float lace = (1.0 - smoothstep(.00040, .00040 + footprint,
                                  min(abs(loop),abs(loop+.0022)))) *
                 smoothstep(.040, .050, p.y);
    float edge = 1.0 - smoothstep(.00045, .00045 + footprint, abs(abs(crown) - .0046));
    float gild = max(band, max(lace, edge * .75));
    // A bordered lozenge on the forehead; no emitted light or moving sparkle.
    float seal = abs(p.x) / .010 + abs(p.y - .030) / .017;
    float front = 1.0 - smoothstep(-.067, -.050, p.z);
    float rim = (1.0 - smoothstep(.10, .10 + footprint * 100.0, abs(seal - 1.0))) * front;
    float jewel = (1.0 - smoothstep(.76, .76 + footprint * 100.0, seal)) * front;
    gild = max(gild, rim);
    float wear = smoothstep(.53, .64, broad + (grain - .5) * .20);
    float gold = gild * (1.0 - .45 * wear);
    vec3 ivory = mix(vec3(.43,.345,.225), vec3(.64,.55,.38), smoothstep(.35,.65,broad));
    ivory *= 1.0 - .08 * detail * smoothstep(.52,.65,grain);
    vec3 gilt = mix(vec3(.43,.23,.045), vec3(.74,.49,.135), smoothstep(.35,.65,broad));
    gilt *= 1.0 - .16 * detail * smoothstep(.56,.66,grain);
    vec3 color = mix(ivory, gilt, gold);
    color = mix(color, vec3(.11,.003,.012) * (.65 + .6 * broad), jewel);
    color = mix(color, vec3(.028,.018,.009), smoothstep(.12,.85,cav) * .85);
    metal = gold * (1.0 - jewel) * .92;
    float rough = mix(.59, .29, gold);
    rough = mix(rough, .16, jewel);
    // Shallow raised inlay and fine grain; footprint fading prevents subpixel sparkle.
    height = gold * .00016 + jewel * .00022 + (grain - .5) * .000055 * detail;
    return vec4(color, rough);
}

// Queenly Blood alone: wet wine-dark clot, pale fibrous veins, black dried islands.
// Object-space procedural texture: no bitmap, extra draw, animation or transparency.
vec4 QueenlyBloodSurface(vec3 p) {
    float broad=GoodsNoise(p*100.0+vec3(4,9,2));
    float grain=GoodsNoise(p*410.0+vec3(7,1,5));
    float fold=abs(GoodsNoise(p*vec3(190.0,65.0,190.0)+broad*2.0)-.5);
    float width=max(.006,fwidth(fold)*1.2);
    float veins=(1.0-smoothstep(width,width*2.6,fold))*smoothstep(.43,.58,broad);
    float crust=smoothstep(.51,.63,grain+.12*(broad-.5));
    vec3 colour=mix(vec3(.020,.001,.003),vec3(.15,.006,.012),smoothstep(.36,.64,broad));
    colour=mix(colour,vec3(.009,.004,.008),crust*.87);
    colour=mix(colour,vec3(.18,.055,.045),veins*.65);
    return vec4(colour,mix(.38,.85,crust)*(1.0-.25*veins));
}

// Held quick items (vr_goods_meshes.h): material id in aux.x (GoodsMat), the baked cavity in
// aux.y. Old and a little dirty, a little rust; lit like the apothecary case.
// BEGIN HUNTER RELIC MATERIAL
// Original procedural material. Metres; no sampled textures or particle buffers.
float RelicNoise(vec3 p) {
    return .5+.25*sin(p.x+sin(p.z*1.31))+.25*sin(p.y*1.71+p.z);
}
vec4 RelicSurface(vec3 p, float code, out float metal, out float height) {
    float footprint=max(length(fwidth(p)),.00001);
    float fine=1.-smoothstep(.0002,.0012,footprint);
    float age=RelicNoise(p*165.+vec3(3,1,7));
    float grain=RelicNoise(p*3200.);
    float scuff=smoothstep(.68,.82,RelicNoise(p*vec3(1100,85,750)))*fine;
    vec3 color; float rough;
    if(code<6.5) {
        color=mix(vec3(.08,.095,.11),vec3(.32,.34,.36),age);
        color=mix(color,vec3(.065,.040,.021),smoothstep(.72,.90,age)*.65);
        color=mix(color,vec3(.44,.45,.46),scuff*.36);
        rough=.57-.19*scuff;metal=.8;height=(grain-.5)*.000055*fine;
    } else if(code<7.5) {
        color=mix(vec3(.10,.058,.020),vec3(.49,.31,.092),age);
        rough=.46;metal=.85;height=(grain-.5)*.000025*fine;
    } else if(code<10.) {
        color=mix(vec3(.028,.023,.019),vec3(.083,.055,.033),age);
        float blood=smoothstep(.69,.89,RelicNoise(p*175.+vec3(2,9,4)));
        color=mix(color,vec3(.045,.016,.012),blood*.32);
        float cracks=1.-smoothstep(.025,.075,abs(RelicNoise(p*vec3(640,480,170))-.5));
        color*=1.-cracks*.18*fine;
        color=mix(color,vec3(.19,.13,.075),scuff*.16);
        rough=.86-blood*.10;metal=.0;height=((grain-.5)*.00010-cracks*.00010)*fine;
    } else if(code<19.5) {
        float weave=sin(p.x*7200.)*sin(p.z*7200.);
        color=mix(vec3(.022,.018,.016),vec3(.12,.085,.050),age);
        color*=.86+.18*grain*fine+.06*weave*fine;
        rough=.94;metal=0.;height=((grain-.5)*.0001+weave*.00004)*fine;
    } else {
        color=vec3(.045,.065,.085);rough=.25;metal=.7;height=0.;
    }
    return vec4(color,rough);
}
vec3 RelicEmission(vec3 p,float code,float kind,float time,float charge,float burst) {
    float current=clamp(charge,0.,1.),flash=clamp(burst,0.,1.);
    float pulse=.62+.38*sin(time*24.+p.z*260.);
    float electrode=step(19.5,code);
    float embroidery=step(6.5,code)*(1.-step(7.5,code));
    vec3 tint=kind<.5?vec3(.24,.54,1.):vec3(1.,.025,.008);
    return tint*(electrode*(current*pulse+flash*2.)+
                 (kind>.5?embroidery*(current*.18+flash*.55):0.));
}

// END HUNTER RELIC MATERIAL

// The Blacksky Eye (core/vr/blacksky_eye.h; vr_hunter_relic_meshes.h BlackskyEye): a living
// human eye. Mesh metres, its centre at the origin, its front -Z; uv = (5, time s, the pupil's
// radius in the iris' 0..1, the shot's flash 1..0). 21 the sclera, 22 the iris and its pupil,
// 23 the cornea (clear, drawn after the rest over the recessed iris), 24 the optic nerve.
float EyeHash(vec2 c) { return fract(sin(dot(c, vec2(127.1, 311.7))) * 43758.5453); }
vec4 ShadeEyeFn(vec3 p, vec3 n, vec3 v, vec3 l, vec3 tint, float e, float exposure, int id) {
    const float pupil = clamp(dd.uv.z, 0.10, 0.85);
    const float flash = clamp(dd.uv.w, 0.0, 1.0);
    const float fine = 1.0 - smoothstep(0.00006, 0.0005, length(fwidth(p)));
    const vec3 h = normalize(l + v);
    const float nh = max(dot(n, h), 0.0), nv = max(dot(n, v), 0.0);
    const vec3 grab = dd.emissive.rgb * 0.10;
    if (id == 23) {
        // The cornea: clear and wet. Its Fresnel sheen of the room's light and its highlights
        // (the key, sharp, a broad soft lobe, and a second small window), premultiplied: the iris
        // shows through it unchanged but where it reflects.
        const float fres = 0.025 + 0.975 * pow(1.0 - nv, 5.0);
        const vec3 l2 = normalize(vec3(0.55, 0.35, -0.75));
        const float spec = pow(nh, 900.0) * 4.0 + pow(nh, 90.0) * 0.30 +
                           pow(max(dot(n, normalize(l2 + v)), 0.0), 1400.0) * 1.6;
        const vec3 room = mix(vec3(0.07, 0.075, 0.08), vec3(0.30, 0.31, 0.33),
                              clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
        const vec3 lit = (room * fres + vec3(spec)) * tint * e;
        const float a = clamp(fres * 0.85 + spec * 0.15, 0.035, 1.0);
        return vec4(lit / a, a);
    }
    vec3 albedo;
    vec3 glow = vec3(0.0);
    float shine = 0.0;
    if (id == 22) {
        // The iris, its pupil an opening that dilates (its fibres bunch outward as it does):
        // radial wavy stroma, the collarette's zig-zag ridge between the amber pupillary zone and
        // the grey-blue ciliary one, crypts, a few contraction furrows, the dark limbal ring.
        const float r = length(p.xy) / 0.006;
        const float phi = atan(p.y, p.x);
        const float aa = max(fwidth(r), 1e-4);
        const float open = smoothstep(pupil - aa, pupil + aa, r);
        const float rho = clamp((r - pupil) / (1.0 - pupil), 0.0, 1.0);
        const vec2 c = vec2(cos(phi), sin(phi));
        const float wav = GoodsNoise(vec3(c * 3.0, rho * 4.0));
        const float ang = phi + (wav - 0.5) * 0.35;
        const float fib = 0.5 + 0.5 * sin(ang * 64.0 + GoodsNoise(vec3(c * 9.0, rho * 2.0)) * 9.0);
        const float fib2 = 0.5 + 0.5 * sin(ang * 157.0 + rho * 6.0 + wav * 4.0);
        const float collar = 0.36 + 0.035 * sin(phi * 13.0 + 1.7) + 0.02 * sin(phi * 29.0);
        const float zone = smoothstep(collar - 0.04, collar + 0.04, rho);
        const vec3 inner = vec3(0.34, 0.19, 0.065) * (0.75 + 0.5 * fib);
        const vec3 outer = mix(vec3(0.13, 0.20, 0.26), vec3(0.27, 0.35, 0.42), fib);
        vec3 col = mix(inner, outer, zone) * (0.82 + 0.32 * mix(0.5, fib2, fine));
        col += vec3(0.09, 0.07, 0.045) * (1.0 - smoothstep(0.0, 0.04, abs(rho - collar)));
        const float crypt = smoothstep(0.60, 0.68, GoodsNoise(vec3(c * 6.0, rho * 7.0) + 4.0)) *
                            smoothstep(0.25, 0.45, rho) * (1.0 - smoothstep(0.72, 0.9, rho));
        col *= 1.0 - 0.55 * crypt;
        col *= 1.0 - 0.22 * smoothstep(0.80, 1.0, sin(rho * 34.0 + wav * 3.0)) *
                         smoothstep(0.55, 0.75, rho) * fine;
        col = mix(col, vec3(0.03, 0.04, 0.05), smoothstep(0.80, 0.985, r));
        col = mix(col, vec3(0.09, 0.045, 0.02), (1.0 - smoothstep(0.0, 0.06, rho)) * 0.8);
        // The pupil: black; fired, the black sky inside it and its light pouring out.
        const vec3 hole = vec3(0.005, 0.005, 0.007);
        if (flash > 0.001) {
            const vec2 cell = floor(p.xy / 0.00028);
            const vec2 at = fract(p.xy / 0.00028) - 0.5;
            const vec2 jitter = (vec2(EyeHash(cell + 3.1), EyeHash(cell + 7.7)) - 0.5) * 0.5;
            const float star = step(0.82, EyeHash(cell)) * (1.0 - smoothstep(0.05, 0.22, length(at - jitter)));
            const float well = 1.0 - smoothstep(0.0, pupil, r);
            glow = vec3(0.50, 0.62, 1.0) * flash * (1.0 - open) * (star * 2.2 + well * 0.9);
        }
        albedo = mix(hole, col, open);
    } else if (id == 24) {
        // The optic nerve: pale fibrous sheath, its cut face raw and wet.
        const float fibre = GoodsNoise(vec3(p.x * 2600.0, p.y * 2600.0, p.z * 260.0));
        albedo = mix(vec3(0.52, 0.42, 0.37), vec3(0.70, 0.60, 0.53), fibre);
        const float cut = smoothstep(0.0158, 0.0161, p.z);
        albedo = mix(albedo, mix(vec3(0.22, 0.02, 0.025), vec3(0.40, 0.08, 0.06), fibre), cut);
        albedo = mix(albedo, vec3(0.46, 0.13, 0.11), smoothstep(0.0128, 0.0115, p.z) * 0.6);
        shine = mix(0.05, 0.35, cut);
    } else {
        // The sclera: ivory, pinker behind; the conjunctiva's grey-blue at the limbus; veins as
        // warped ridges, thick and many behind and thinning toward the iris, finer branches
        // between; a blush of blood where the muscles were torn away.
        const vec3 d = normalize(p);
        const float front = -d.z;
        albedo = mix(vec3(0.74, 0.62, 0.57), vec3(0.86, 0.80, 0.73), smoothstep(-0.6, 0.55, front));
        albedo *= 0.94 + 0.10 * GoodsNoise(d * 40.0);
        albedo = mix(albedo, vec3(0.52, 0.55, 0.60), smoothstep(0.74, 0.84, front) * 0.5);
        // Into the limbus: the soft dark ring the iris' own edge continues (no hard line).
        albedo = mix(albedo, vec3(0.06, 0.07, 0.085), smoothstep(0.835, 0.866, front) * 0.85);
        // Vessels: long warped lines running from behind toward the limbus (a lower frequency
        // than the eye is pixels across, so they stay lines), branches in patches between.
        const vec3 warp = vec3(GoodsNoise(d * 3.0 + 3.1), GoodsNoise(d * 3.0 + 7.7),
                               GoodsNoise(d * 3.0 + 1.9)) - 0.5;
        const vec3 q = d * 3.4 + warp * 1.6;
        const float r1 = abs(GoodsNoise(q) - 0.5);
        const float r2 = abs(GoodsNoise(q * 2.2 + 11.0) - 0.5);
        const float back = smoothstep(0.80, -0.4, front);
        const float b1 = 0.020 + 0.012 * back, b2 = 0.012 + 0.006 * back;
        const float w1 = max(b1, fwidth(r1) * 1.5);
        const float w2 = max(b2, fwidth(r2) * 1.5);
        // A vessel thinner than its pixel fades (its contrast spread over it), and edge-on,
        // where it would only alias into specks along the outline.
        const float grazing = smoothstep(0.08, 0.35, nv);
        float vein = (1.0 - smoothstep(w1 * 0.35, w1, r1)) * (b1 / w1) *
                     smoothstep(0.10, 0.45, back + 0.3 * (GoodsNoise(d * 2.0) - 0.5));
        vein = max(vein, (1.0 - smoothstep(w2 * 0.35, w2, r2)) * (b2 / w2) * 0.75 *
                         smoothstep(0.52, 0.62, GoodsNoise(d * 4.0 + 5.0)) *
                         (0.25 + 0.75 * smoothstep(0.05, 0.6, back)) * smoothstep(0.86, 0.62, front));
        vein *= grazing * fine;
        // Behind, the vessels that came in with the nerve: seven trunks leaving it at uneven
        // angles, meandering more as they go, thick at the nerve and thinning toward the equator,
        // each forking once and the fork drifting away (2026-11-04: the even meridians before read
        // as a beach ball). Widths are arcs across the globe (constant on it, not crowding at the
        // nerve); under a pixel a line fades rather than break into specks.
        {
            const float pb = 3.14159265 - acos(clamp(front, -1.0, 1.0)); // 0 at the nerve
            const float turn = atan(d.y, d.x) / 6.2831853;
            const float across = 6.2831853 * max(sin(pb), 0.02); // a turn's arc at this latitude
            float radial = 0.0;
            for (int k = 0; k < 7; ++k) {
                const float h1 = fract(sin(float(k) * 12.9898 + 4.1) * 43758.5453);
                const float h2 = fract(sin(float(k) * 78.233 + 1.7) * 24634.6345);
                const float h3 = fract(sin(float(k) * 39.425 + 9.3) * 11853.1937);
                const float c = (float(k) + 0.6 * (h1 - 0.5)) / 7.0 +
                                (0.02 + 0.045 * pb) * sin(pb * (4.0 + 2.0 * h2) + h1 * 6.2831853) +
                                0.012 * sin(pb * 13.0 + h3 * 6.2831853);
                const float width = mix(0.075, 0.028, smoothstep(0.2, 1.45, pb)) * (0.8 + 0.4 * h2);
                const float end = 1.0 - smoothstep(1.05 + 0.35 * h3, 1.45 + 0.35 * h3, pb);
                const float arc = abs(fract(turn - c + 0.5) - 0.5) * across;
                const float aa = max(width, fwidth(arc) * 1.2);
                radial = max(radial, (1.0 - smoothstep(aa * 0.4, aa, arc)) * (width / aa) * end);
                // The fork: from pf on, a thinner vessel drifting off to one side.
                const float pf = 0.42 + 0.3 * h3;
                const float cb = c + (h2 > 0.5 ? 1.0 : -1.0) * 0.11 * max(pb - pf, 0.0);
                const float wb = width * 0.65;
                const float arcb = abs(fract(turn - cb + 0.5) - 0.5) * across;
                const float ab = max(wb, fwidth(arcb) * 1.2);
                radial = max(radial, (1.0 - smoothstep(ab * 0.4, ab, arcb)) * (wb / ab) *
                                         smoothstep(pf, pf + 0.06, pb) *
                                         (1.0 - smoothstep(1.0 + 0.3 * h1, 1.35 + 0.3 * h1, pb)));
            }
            vein = max(vein, radial * smoothstep(0.13, 0.22, pb) * grazing);
        }
        const vec3 blood = mix(vec3(0.50, 0.04, 0.04), vec3(0.32, 0.02, 0.035),
                               smoothstep(0.3, 0.8, GoodsNoise(q * 1.7)));
        albedo = mix(albedo, blood, vein * 0.85);
        // Round its equator, behind the limbus' line: the four straight muscles' torn stumps, each
        // a short flat band about 10 mm wide along the globe (ragged edged, raw, its fibres along
        // the meridian, darker at the cut); a little pale tissue round the nerve.
        const float az = atan(d.y, d.x);
        const float polar = acos(clamp(front, -1.0, 1.0)); // 0 at the cornea
        float stump = 0.0, cut = 0.0;
        for (int k = 0; k < 4; ++k) {
            const float da = abs(mod(az - (1.5707963 * float(k) + 0.35) + 3.14159265, 6.2831853) - 3.14159265);
            const float ragged = 0.08 * (GoodsNoise(vec3(az * 7.0, polar * 9.0, float(k) * 3.7)) - 0.5);
            const float across = 1.0 - smoothstep(0.30, 0.38, da + ragged);
            const float along = smoothstep(1.18, 1.26, polar + ragged) * (1.0 - smoothstep(1.52, 1.66, polar - ragged));
            stump = max(stump, across * along);
            cut = max(cut, across * smoothstep(1.46, 1.60, polar - ragged) * (1.0 - smoothstep(1.60, 1.66, polar - ragged)));
        }
        const float fibres = GoodsNoise(vec3(az * 55.0, polar * 5.0, 0.0));
        vec3 muscle = mix(vec3(0.44, 0.15, 0.13), vec3(0.30, 0.08, 0.075), smoothstep(0.35, 0.7, fibres));
        muscle = mix(muscle, vec3(0.20, 0.03, 0.035), cut * 0.7);
        albedo = mix(albedo, muscle, stump * 0.75 * fine + stump * 0.45 * (1.0 - fine));
        albedo = mix(albedo, vec3(0.64, 0.50, 0.42), smoothstep(-0.80, -0.96, front) *
                                                     smoothstep(0.45, 0.6, GoodsNoise(d * 18.0)) * 0.6);
        albedo = mix(albedo, vec3(0.55, 0.30, 0.27), smoothstep(-0.55, -0.95, front) * 0.15);
        shine = 0.45;
    }
    // Wet tissue under the hand's light: a wrapped diffuse (light through it), its shadow side
    // warmed by the blood under it, a wet highlight and sheen.
    const float wrap = clamp((dot(n, l) + 0.35) / 1.35, 0.0, 1.0);
    vec3 rgb = albedo * (0.30 + 0.85 * wrap) * tint * e;
    rgb += albedo * vec3(0.22, 0.04, 0.03) * (1.0 - wrap) * e * 0.6;
    rgb += vec3(pow(nh, 140.0) * 0.9 + pow(nh, 30.0) * 0.12) * shine * tint * e;
    rgb += vec3(0.04) * pow(1.0 - nv, 4.0) * shine * tint * e;
    return vec4(rgb + glow * exposure + grab, 1.0);
}

// The packet's "KOS LIGHTS", 5x5 bits a letter. A switch, not a constant array: an array indexed
// at run time is copied into memory, which can cost the whole shader (every in-frame draw)
// scratch space on some GPUs.
uint KosGlyph(int i) {
    switch(i) {
    case 0: return 18128177u; case 1: return 15255086u; case 2: return 16267326u;
    case 4: return 32539681u; case 5: return 32641183u; case 6: return 32044094u;
    case 7: return 18415153u; case 8: return 4329631u;  case 9: return 16267326u;
    default: return 0u;
    }
}

vec4 ShadeGoodsFn(float exposure) {
    vec3 tint; const float e=HeldLight(tint)*exposure;
    vec3 n=normalize(in_normal), v=normalize(-in_view);
    if(dot(n,v)<0.0) n=-n;
    const vec3 l=normalize(vec3(-.30,.80,-.45));
    float nh=max(dot(n,normalize(l+v)),0.0);
    const vec3 p=in_local;
    const int id=int(in_uv.x+.5);
    const float cav=clamp(in_uv.y,0.0,1.0);
    // Sub-millimetre detail fades with the pixel's footprint: no shimmer in the headset.
    const float detail=1.0-smoothstep(.0004,.002,length(fwidth(p)));
    const float fine=mix(.5,1.0,detail);
    if(dd.uv.x>4.5 && dd.uv.x<5.5) return ShadeEyeFn(p,n,v,l,tint,e,exposure,id);
    vec3 albedo; vec3 glow=vec3(0.0);
    float rough=.9, metal=0.0, G=1.3, K=.7;
    vec3 grime=vec3(.020,.014,.008);
    if(dd.uv.x>2.5 && dd.uv.x<4.5) {
        float height;
        vec4 relic=RelicSurface(p,float(id),metal,height);
        albedo=relic.rgb;rough=relic.a;G=0.0;K=0.0;
        vec3 q0=dFdx(in_view),q1=dFdy(in_view),r1=cross(q1,n),r2=cross(n,q0);
        float det=dot(q0,r1);
        if(abs(det)>1e-12) {
            n=normalize(abs(det)*n-sign(det)*(dFdx(height)*r1+dFdy(height)*r2));
            nh=max(dot(n,normalize(l+v)),0.0);
        }
        glow=RelicEmission(p,float(id),dd.uv.x-3.0,dd.uv.y,dd.uv.z,dd.uv.w);
    } else if(id==19) {
        // Hand-rolled, stained parchment. Detail fades below the pixel footprint.
        float grain=GoodsNoise(p*vec3(1800.0,450.0,1800.0));
        float stain=GoodsNoise(p*150.0+vec3(3,7,1));
        albedo=mix(vec3(.24,.125,.045),vec3(.64,.46,.22),smoothstep(.33,.65,stain));
        albedo*=.87+.22*mix(.5,grain,detail);
        float fibre=sin(p.y*4100.0+p.x*400.0)*sin(p.z*3700.0+p.y*140.0);
        albedo*=1.0+.065*fibre*detail;
        float seam=1.0-smoothstep(.00015,.00055+length(fwidth(p)),abs(p.z+.0028));
        albedo*=1.0-.22*seam;
        float mouth=1.0-smoothstep(-.010,.003,p.y);
        albedo=mix(albedo,vec3(.12,.058,.021),mouth*.65);
        float burned=p.y-(.0545+.0025*(grain-.5));
        float ash=smoothstep(0.0,.002,burned);
        albedo=mix(albedo,mix(vec3(.025,.022,.019),vec3(.29,.27,.235),
                             smoothstep(.38,.65,grain)),ash);
        // Broken cinder specks, not a solid neon cap.
        glow=vec3(.65,.075,.006)*ash*smoothstep(.58,.68,grain)*.45;
        rough=.98; G=.12; K=.3;
    } else if(id==20) {
        float roof=step(.014-abs(p.x)*.65,p.y);
        albedo=mix(vec3(.69,.62,.44),vec3(.32,.009,.012),roof);
        float edge=max(abs(p.x)/.027,abs(p.y)/.043);
        albedo*=1.0-.22*smoothstep(.91,1.0,edge);
        albedo*=.90+.10*GoodsNoise(p*850.0);
        albedo*=1.0-.35*(1.0-smoothstep(.0003,.0008,abs(p.y-.025)));
        // Tiny baked bitmap wordmark: no texture allocation or extra draw for the packet.
        vec2 label=vec2((p.x+.024)/.0008,(.003-p.y)/.0011);
        if(abs(p.z)>.0105 && label.x>=0.0 && label.x<60.0 && label.y>=0.0 && label.y<5.0) {
            ivec2 cell=ivec2(floor(label));
            int col=cell.x%6;
            float ink=col<5 ? float((KosGlyph(cell.x/6)>>uint(cell.y*5+col))&1u) : 0.0;
            albedo=mix(albedo,vec3(.035,.022,.012),ink*detail);
        }
        rough=.95;G=.15;K=.25;
    } else if(id==6) {
        // Iron: tarnish, bright wear on the raised parts, orange-brown rust in patches and
        // bled into the crevices; its flaky rims are noise ridges (no cell pattern).
        albedo=mix(vec3(.045,.040,.034),vec3(.15,.14,.12),smoothstep(.3,.75,GoodsNoise(p*180.0)));
        const float wear=smoothstep(.85,.99,1.0-cav)*smoothstep(.64,.69,GoodsNoise(p*520.0+3.1))*.7*fine;
        albedo=mix(albedo,vec3(.30,.28,.25),wear);
        const float rust=smoothstep(.58,.70,cav*.55+GoodsNoise(p*70.0+1.3));
        vec3 rc=mix(vec3(.055,.022,.009),vec3(.19,.068,.022),smoothstep(.35,.7,GoodsNoise(p*650.0)));
        rc*=.55+.45*mix(1.0,smoothstep(.0,.05,abs(GoodsNoise(p*420.0+5.7)-.5)),detail);
        albedo=mix(albedo,rc,rust);
        rough=mix(mix(.45,.28,wear),.92,rust); metal=1.0-rust; K=.55;
    } else if(id==7) {
        // Bronze: dark metal, green-brown patina gathered in the hollows, a few worn highlights.
        albedo=mix(vec3(.05,.033,.015),vec3(.12,.082,.036),smoothstep(.3,.7,GoodsNoise(p*160.0)));
        const float wear=smoothstep(.85,.99,1.0-cav)*smoothstep(.6,.68,GoodsNoise(p*320.0+2.2))*.6*fine;
        albedo=mix(albedo,vec3(.26,.18,.08),wear);
        const float pat=smoothstep(.50,.66,cav*.6+GoodsNoise(p*55.0+4.4));
        albedo=mix(albedo,mix(vec3(.032,.042,.026),vec3(.07,.095,.060),smoothstep(.4,.7,GoodsNoise(p*500.0))),pat*.75);
        rough=mix(mix(.45,.3,wear),.85,pat); metal=1.0-pat; K=.6;
    } else if(id==2||id==3||id==13||id==15) {
        // Cloth and cord: a weave, uneven stains, grime in the folds; velvet keeps its crimson
        // with dust in the folds and a soft sheen.
        const bool velvet=id==3;
        albedo=id==2?vec3(.30,.26,.19):id==15?vec3(.26,.23,.17):velvet?vec3(.050,.004,.008):vec3(.10,.075,.05);
        albedo*=.85+.25*mix(.5,smoothstep(.3,.7,GoodsNoise(p*900.0)),detail);
        const float st=smoothstep(.58,.72,GoodsNoise(p*60.0+vec3(5,2,9)));
        albedo=mix(albedo,velvet?vec3(.018,.005,.004):vec3(.11,.075,.035),st*(velvet?.75:.55));
        if(velvet) {
            albedo=mix(albedo,vec3(.05,.04,.04),smoothstep(.5,.85,cav+.4*GoodsNoise(p*80.0))*.18);
            glow=vec3(.35,.04,.06)*.35*pow(1.0-max(dot(n,v),0.0),3.0)*tint;   // the sheen
            K=.5; grime=vec3(.016,.004,.004);
        } else {
            K=.75; grime=vec3(.035,.025,.014);
        }
        rough=.93;
    } else if(id==4||id==5||id==10||id==14) {
        // Paper, label, cork, pills: fibres, stains; foxing on paper; darker handled edges.
        albedo=id==5?vec3(.12,.075,.036):id==14?vec3(.30,.245,.15):id==4?vec3(.15,.09,.045):vec3(.34,.32,.27);
        albedo*=.88+.2*mix(.5,smoothstep(.3,.7,GoodsNoise(p*700.0)),detail);
        albedo=mix(albedo,vec3(.08,.05,.022),smoothstep(.56,.72,GoodsNoise(p*45.0+vec3(1,7,3)))*.5);
        if(id==5||id==14) {
            const float spots=smoothstep(.62,.68,GoodsNoise(p*260.0))*smoothstep(.5,.62,GoodsNoise(p*35.0+9.0));
            albedo=mix(albedo,vec3(.10,.055,.02),spots*.8*fine);
        }
        if(id==5) {
            // The Antidote's sheet (8 cm): its edges handled dark.
            albedo=mix(albedo,vec3(.04,.024,.011),smoothstep(.026,.040,max(abs(p.x),abs(p.z)))*.8);
        } else if(id==14) {
            // Iosefka's label: wrapped +-1.25 rad round the front, 4.8 cm tall.
            const float s=abs(atan(p.x,-p.z))/1.25, t=abs((p.y+.016)/.024);
            albedo=mix(albedo,vec3(.06,.04,.02),smoothstep(.76,1.0,max(s,t))*.7);
        } else if(id==10) {
            const float specks=smoothstep(.63,.68,GoodsNoise(p*1400.0))*smoothstep(.45,.6,GoodsNoise(p*90.0));
            albedo=mix(albedo,vec3(.08,.06,.03),specks*detail);
        }
        rough=id==14?.9:.85;
    } else if(id==9) {
        // Leather: dark, light scratches along it, grime packed into the strap's folds.
        albedo=mix(vec3(.024,.013,.0065),vec3(.040,.022,.011),smoothstep(.3,.7,GoodsNoise(p*90.0)));
        const float scuff=smoothstep(.66,.74,GoodsNoise(p*vec3(900.0,72.0,900.0)))*smoothstep(.45,.6,GoodsNoise(p*30.0))*fine;
        albedo=mix(albedo,vec3(.09,.065,.045),scuff*.5);
        rough=mix(.55,.8,scuff); G=2.0; K=.85; grime=vec3(.008,.005,.003);
    } else if(id==8 && dd.uv.x>1.5) {
        float height;
        vec4 relic = MessengerGiftSurface(p, cav, metal, height);
        if(int(floor(in_uv.x*100.0+.5))%100>=25) { // tolerate interpolation near integer 8
            relic=vec4(.24,.18,.105,.93); metal=0.0; height=0.0;
        }
        albedo = relic.rgb; rough = relic.a; G = 0.0; K = 0.0;
        // Surface-gradient bump, using existing varyings; no tangent buffer or map fetch.
        vec3 q0 = dFdx(in_view), q1 = dFdy(in_view);
        vec3 r1 = cross(q1,n), r2 = cross(n,q0);
        float det = dot(q0,r1);
        if (abs(det)>1e-12) {
            n = normalize(abs(det)*n-sign(det)*(dFdx(height)*r1+dFdy(height)*r2));
            nh = max(dot(n,normalize(l+v)),0.0);
        }
    } else if(id==8) {
        // Bone (the Sangiran 17 cast): ochre mottled with umber, dark cracks between the plates.
        albedo=mix(vec3(.10,.062,.028),vec3(.42,.30,.14),smoothstep(.36,.66,GoodsNoise(p*18.0)));
        albedo*=.8+.25*mix(.5,smoothstep(.3,.7,GoodsNoise(p*400.0)),detail);
        // Crack lines: a warped noise's mid level, their width uneven along them.
        const float c=abs(GoodsNoise(p*22.0+vec3(3,1,7)+(GoodsNoise(p*55.0)-.5)*3.0)-.5);
        const float cw=max(.010*(.5+GoodsNoise(p*90.0)),fwidth(c)*1.5);
        albedo=mix(albedo,vec3(.012,.008,.004),(1.0-smoothstep(.0,cw,c))*.85);
        rough=.75; K=.6;
    } else if(id==11) {
        if(dd.uv.x>.5) { // Queenly Blood only: a wet, fibrous clot rather than glowing dew.
            vec4 queen=QueenlyBloodSurface(p);
            albedo=queen.rgb; rough=queen.a; G=.6; K=.55;
        } else {
            // Other coldbloods retain their original dark clot and dim inner glow.
            const float crust=smoothstep(.62,.70,GoodsNoise(p*160.0));
            albedo=mix(vec3(.025,.001,.001),vec3(.016,.003,.002),crust);
            glow=vec3(.85,.012,.008)*(.15+.8*smoothstep(.42,.66,GoodsNoise(p*420.0+2.0)))*(1.0-.85*crust)*.12;
            rough=mix(.3,.8,crust); K=.4;
        }
    } else if(id==16||id==17) {
        // Fire / Bolt Paper: a scrap of charred paper (cav here: nearness to its torn edge).
        // Fire smoulders: ember-orange along the edge and in a few specks. Bolt bristles: pale
        // fibres along it and thin blue-white veins of lightning, brightest toward the edge.
        const float edge=cav;
        if(id==16) {
            albedo=mix(vec3(.012,.009,.007),vec3(.075,.052,.034),smoothstep(.35,.7,GoodsNoise(p*90.0)));
            albedo=mix(albedo,vec3(.13,.11,.09),smoothstep(.55,.7,GoodsNoise(p*400.0))*fine);
            const float rim=smoothstep(.78,.97,edge)*smoothstep(.45,.62,GoodsNoise(p*160.0));
            const float speck=smoothstep(.70,.74,GoodsNoise(p*900.0))*smoothstep(.55,.65,GoodsNoise(p*35.0))*fine;
            glow=vec3(1.0,.32,.05)*max(rim,speck)*.35;
        } else {
            const float fib=GoodsNoise(vec3(p.x*60.0,p.y*900.0,p.z*900.0));
            albedo=mix(vec3(.016,.018,.022),vec3(.085,.09,.10),smoothstep(.3,.7,mix(.5,fib,detail)));
            const float vein=(1.0-smoothstep(.0,.016,abs(GoodsNoise(p*38.0)-.5)))*
                             smoothstep(.38,.52,GoodsNoise(p*12.0+vec3(3,1,7)));
            glow=vec3(.55,.78,1.0)*vein*mix(.25,1.0,edge)*.22;
        }
        rough=.92; G=0.0; K=.25;
    } else if(id==18) {
        // The Empty Phantasm Shell: a pale grey-blue husk, dark veins under its skin, a little
        // frost, a cold glow at its rim (the phantasm's light, gone dim).
        albedo=mix(vec3(.05,.065,.08),vec3(.22,.26,.31),smoothstep(.40,.62,GoodsNoise(p*140.0)));
        albedo=mix(albedo,vec3(.62,.68,.74),smoothstep(.74,.80,GoodsNoise(p*900.0))*.6*fine);
        glow=vec3(.45,.6,.9)*pow(1.0-max(dot(n,v),0.0),3.0)*.08;
        rough=.35; K=.35;
    } else {
        // Glass, blood, tonic (the vessels themselves use ShadeSyringeFn): dark glass, dusty.
        albedo=id==1?vec3(.05,.002,.004):id==12?vec3(.22,.13,.03):vec3(.02,.022,.02);
        albedo=mix(albedo,vec3(.07,.064,.052),min(smoothstep(.35,.75,GoodsNoise(p*30.0))*.07+smoothstep(.35,.8,cav)*.28,.30));
        rough=.08; K=.3;
    }
    const float g=clamp(cav*G+(GoodsNoise(p*25.0)-.5)*.7,0.0,1.0);
    albedo=mix(albedo,grime,smoothstep(.15,.85,g)*K);
    const float diffuse=.40+.80*max(dot(n,l),0.0);
    const float shine=pow(nh,mix(70.0,8.0,rough))*mix(.30,.01,rough);
    const vec3 spec=mix(vec3(shine),albedo*shine*6.0,metal);   // metals tint their highlight
    vec3 rgb=(albedo*(1.0-.5*metal)*diffuse+spec)*tint*e+glow*exposure;
    if(!(id==8 && dd.uv.x>1.5) && !(dd.uv.x>3.5 && dd.uv.x<4.5)) rgb+=dd.emissive.rgb*.10;
    return vec4(rgb*dd.color.rgb,dd.color.a);
}

// Atlas masks are in the native models' original UVs. Keep the blue label, the lead
// bottle's engraved collar/base, and every cocktail cloth/cork island opaque.
float NativeBottleGlass(int profile) {
    if (profile == 1) {
        const bool body = in_uv.x < 0.80 && in_uv.y > 0.38;
        const bool label = (in_uv.x > 0.25 && in_uv.x < 0.55 && in_uv.y > 0.55) ||
                           (in_uv.x > 0.13 && in_uv.x < 0.27 && in_uv.y > 0.63 && in_uv.y < 0.83);
        return float(body && !label && in_local.y > -0.120 && in_local.y < 0.060);
    }
    if (profile == 2) {
        // The blank lower panel becomes smoky glass; the native engraved band stays metal.
        return smoothstep(-0.055,-0.051,in_local.y) *
               (1.0-smoothstep(-0.010,-0.005,in_local.y));
    }
    return float(in_uv.x < 0.30 && in_uv.y > 0.22 && in_uv.y < 0.90 &&
                 in_local.y > -0.110 && in_local.y < 0.033);
}

vec4 ShadeSyringeFn(float exposure, int native_profile) {
    vec3 tint;
    const float e = HeldLight(tint) * exposure;
    vec3 n = normalize(in_normal);
    const vec3 v = normalize(-in_view);
    if (dot(n,v) < 0.0) n = -n;
    const vec3 l = normalize(vec3(-0.30,0.80,-0.45));
    const vec3 h = normalize(l+v);
    const float facing = max(dot(n,v),0.0);
    const float diffuse = 0.34 + 0.76 * max(dot(n,l),0.0);
    const float spec = max(dot(n,h),0.0);
    const float lo = dd.uv.x, hi = dd.uv.y;
    const float radius = max(dd.uv.z,0.001);
    const float half_h = max((hi-lo)*0.5,0.001);
    const vec3 p = in_local - vec3(dd.emissive.x,(lo+hi)*0.5,dd.emissive.y);
    const float angle = atan(p.x,p.z);
    const float age = VialNoise(in_local*340.0);
    const float grain = VialNoise(in_local*1600.0);
    const float detail = 0.28;
    const float fw = max(fwidth(in_local.y),0.00003);
    // aux.x >= 2: a potion bottle (Iosefka's Blood Vial, vr_goods_meshes.h): all glass, its own
    // liquid, a few bubbles; emissive.z is then its bubbles' phase (0 .. 1 over 4 s), not a film.
    const bool native_bottle = native_profile != 0;
    const bool potion = native_bottle || in_uv.x >= 1.5;
    const vec4 native_surface = native_bottle ? ShadeGameModel(exposure, true) : vec4(0.0);
    const float native_glass = native_bottle ? NativeBottleGlass(native_profile) : 1.0;
    if (native_bottle && native_glass <= 0.0) return native_surface;
    const float rail = smoothstep(0.01565,0.0160,length(p.xz));
    // Glass, not metal: the vial's barrel between its collars, a potion everywhere (its liquid is
    // bounded by the column's own lo / hi below, not by this mask).
    const float chamber = potion ? 1.0 : (1.0-rail)*smoothstep(lo,lo+max(fw,0.0005),in_local.y) *
                          (1.0-smoothstep(hi-max(fw,0.0005),hi,in_local.y));

    // Tarnished surgical nickel, brown oxidation in recesses, restrained rubbed highlights.
    vec3 metal = mix(vec3(0.030,0.025,0.020),vec3(0.23,0.21,0.17),
                     clamp(detail+age*0.55,0.0,1.0));
    metal *= 0.84 + 0.22*grain;
    const float threads = 0.5+0.5*sin(in_local.y*2600.0);
    const float thread_fade = 1.0-smoothstep(0.7,2.0,fw*2600.0);
    metal *= 1.0 - 0.12*threads*thread_fade;
    // Dry umber/ochre corrosion rather than red stains. A coarse field produces a few
    // broad islands of crust; the smaller existing noise breaks their edges into flakes.
    const float seam_d = min(min(abs(in_local.y-0.032),abs(in_local.y-0.146)),
                             min(abs(in_local.y-0.170),abs(in_local.y-0.014)));
    const float seam = exp(-seam_d/0.0014);
    const float coarse = VialNoise(in_local*95.0+vec3(9.2,3.7,5.1));
    const float crust_field = coarse*0.80+age*0.16+grain*0.04;
    const float crust = smoothstep(0.49,0.57,crust_field);
    const float speckles = smoothstep(0.43,0.71,age*0.80+grain*0.20+seam*0.18);
    const float rust = max(crust,0.48*speckles+0.22*seam);
    const vec3 oxide = mix(vec3(0.026,0.017,0.008),vec3(0.108,0.074,0.033),
                           0.25+0.65*grain);
    metal = mix(metal,oxide,rust*0.94);
    // Dark broken edges suggest layered scale, with rough, matte interiors and small
    // exposed metal islands. All surface shading: no extra geometry or draw calls.
    const float crust_edge = smoothstep(0.46,0.50,crust_field)*
                             (1.0-smoothstep(0.52,0.57,crust_field));
    metal *= (1.0-0.34*crust_edge)*(1.0-0.45*seam)*
             (0.82+0.18*smoothstep(0.25,0.60,grain));
    vec3 metal_rgb = (metal*diffuse +
        vec3(pow(spec,mix(28.0,85.0,age))*0.48*(1.0-rust*0.94)*(1.0-seam*0.5)))*e*tint;

    // Refraction into the thick wall; local rays keep both eyes and all vial orientations valid.
    const mat3 rows = mat3(dd.mv[0].xyz,dd.mv[1].xyz,dd.mv[2].xyz);
    const mat3 inv_model = inverse(transpose(rows));
    const vec3 ray0 = normalize(inv_model*normalize(in_view));
    vec3 local_n = normalize(rows*n);
    if (dot(local_n,ray0)>0.0) local_n = -local_n;
    const vec3 ray = refract(ray0,local_n,1.0/1.47);
    const float a = max(dot(ray.xz,ray.xz),1e-7);
    const float b = dot(p.xz,ray.xz);
    const float c = dot(p.xz,p.xz)-radius*radius;
    const float disc = b*b-a*c;
    const float root = sqrt(max(disc,0.0));
    float enter = max((-b-root)/a,0.0), leave = (-b+root)/a;
    const float safe_y = abs(ray.y)>1e-6 ? ray.y : (ray.y>=0.0 ? 1e-6 : -1e-6);
    const float t0 = (-half_h-p.y)/safe_y, t1 = (half_h-p.y)/safe_y;
    enter = max(enter,min(t0,t1));
    leave = min(leave,max(t0,t1));
    const vec3 liquid_n = normalize(dd.color.xyz);
    const float denom = dot(liquid_n,ray);
    const float below = dd.color.w-dot(liquid_n,p);
    float blood_enter = enter, blood_leave = leave;
    const float surface_t = below/(abs(denom)>1e-6 ? denom : 1e-6);
    if (abs(denom)<1e-6) {
        if (below<0.0) blood_leave = blood_enter;
    } else if (denom>0.0) blood_leave = min(blood_leave,surface_t);
    else blood_enter = max(blood_enter,surface_t);
    const float blood_len = disc>0.0 && dd.uv.w>0.0 ? max(blood_leave-blood_enter,0.0) : 0.0;
    const vec3 absorption = native_profile == 1 ? vec3(105.0,48.0,12.0) :
                            native_profile == 2 ? vec3(95.0,88.0,80.0) :
                            native_profile == 3 ? vec3(60.0,260.0,220.0) :
                            potion ? vec3(16.0,38.0,160.0) : vec3(82.0,420.0,340.0);
    const vec3 trans = exp(-absorption*blood_len);
    // Background is the pre-item scene probe. A small refractive shift, bounded on screen;
    // opaque depth/coverage remains the original vial's for the existing DIBR hand occlusion.
    vec3 behind = vec3(0.11,0.125,0.115)*e;
    if (dd.extra.w!=0.0) {
        const vec2 screen = gl_FragCoord.xy*vec2(abs(dd.extra.z),dd.extra.w);
        behind = textureLod(probe,clamp(screen+n.xy*0.003,0.001,0.999),1.0).rgb;
        if (dd.extra.z<0.0) behind = behind.bgr;
        behind = pow(behind,vec3(2.2));
    }
    const vec3 liquid_tint = native_profile == 1 ? vec3(0.008,0.042,0.14) :
                            native_profile == 2 ? vec3(0.042,0.045,0.049) :
                            native_profile == 3 ? vec3(0.075,0.002,0.004) :
                            potion ? vec3(0.15,0.080,0.011) : vec3(0.042,0.0018,0.0031);
    vec3 blood = liquid_tint*diffuse*e*tint;
    // The empty syringe drawing blood (emissive.z < 0: its inflow, BloodDraw::Inflow): a jet of
    // brighter arterial blood up the axis from the needle end, spreading as it rises, streaks
    // curling round it. The level drives the curl (a gush whirls, a top-up settles): no clock.
    // One sample where the ray is mid-blood; only this item's own pixels.
    const float inflow = potion ? 0.0 : max(-dd.emissive.z, 0.0);
    if (inflow > 0.0 && blood_len > 0.0) {
        const vec3 m = p + ray*(0.5*(blood_enter+blood_leave));
        const float rise = clamp((m.y+half_h)/(2.0*half_h), 0.0, 1.0); // 0 at the needle end
        const float off = length(m.xz)/radius;
        const float spread = 0.22+0.78*rise;
        const float jet = exp(-off*off/(0.2*spread*spread))*(1.0-0.6*rise);
        const float curl = 0.5+0.5*sin(atan(m.x,m.z)*3.0+rise*14.0+off*6.0-dd.uv.w*40.0);
        const float k = inflow*clamp(jet+0.45*curl*(1.0-off)*(1.0-rise), 0.0, 1.0);
        blood = mix(blood, vec3(0.15,0.009,0.011)*(0.55+0.65*curl)*diffuse*e*tint, k);
    }
    vec3 glass = behind*trans*vec3(0.82,0.90,0.85) + blood*(1.0-trans);
    if (potion && !native_bottle && blood_len > 0.0) {
        // A few tiny bubbles rising through the blood: hashed columns, each looping once or
        // twice per phase, gone at the surface; a pale rim where the ray grazes one.
        float bubble = 0.0;
        for (int i = 0; i < 7; ++i) {
            const vec2 h = fract(sin(vec2(float(i)*12.9898+1.7,float(i)*78.233+4.1))*43758.5453);
            const float rise = fract(h.x+dd.emissive.z*float(1+i%2));
            const vec3 c = vec3((h.x-0.5)*1.3*radius,(2.0*rise-1.0)*half_h,(h.y-0.5)*1.3*radius);
            const float r = 0.00035+0.00035*h.y;
            if (dot(liquid_n,c) > dd.color.w-r) continue;
            const float t = dot(c-p,ray);
            if (t < blood_enter || t > blood_leave) continue;
            const float off = length(p+ray*t-c);
            bubble = max(bubble,smoothstep(r,0.6*r,off)*(0.45+0.55*smoothstep(0.3*r,0.9*r,off)));
        }
        glass = mix(glass,vec3(0.32,0.25,0.13)*e*tint,bubble*0.6);
    }
    // A wet surface seen THROUGH the wall. The dark meniscus hugs the wall above the volume.
    const float at_surface = float(disc>0.0 && surface_t>enter && surface_t<leave && dd.uv.w>0.0);
    const vec3 surface_n = normalize(transpose(rows)*liquid_n);
    const float wet = pow(abs(dot(surface_n,h)),64.0)*0.035*at_surface;
    glass += vec3(wet)*e;
    const float meniscus = exp(-abs(below)/max(0.00045,fw));
    glass *= 1.0 - meniscus*0.35*float(dd.uv.w>0.0);
    // A sparse, uneven stain remains on emptied glass; no glow or cartoon bubbles.
    const float film = potion ? 0.0 : max(dd.emissive.z,0.0)*(0.07+0.19*pow(age,3.0))*(1.0-exp(-max(-below,0.0)*110.0));
    glass = mix(glass,vec3(0.038,0.006,0.004)*e,film);
    const float fresnel = 0.04+0.65*pow(1.0-facing,5.0);
    const vec2 side_n = n.xz / max(length(n.xz),1e-5);
    const float strip = pow(max(dot(side_n,normalize(vec2(-0.45,-0.9))),0.0),90.0);
    const float reflection = pow(spec,110.0)*0.46 + pow(spec,22.0)*0.025 + strip*0.14;
    glass = glass*(1.0-fresnel) + (vec3(0.19,0.22,0.21)*fresnel+reflection)*e;
    glass += vec3(0.018,0.020,0.017)*age*(1.0-facing)*e;
    if (native_bottle) {
        // Retain the native surface's patina, with the moving liquid visible underneath.
        glass = mix(glass, native_surface.rgb, 0.18);
        return vec4(mix(native_surface.rgb, glass, native_glass), native_surface.a);
    }
    // Fine etched graduations in one narrow sector, softened with the pixel footprint.
    const float ytick = (in_local.y-lo)/0.007;
    const float line = 1.0-smoothstep(0.00016,0.00016+fw,abs(fract(ytick+0.5)-0.5)*0.007);
    const float major = 1.0-step(0.5,mod(floor(ytick+0.5),5.0));
    const float ticks = line*(1.0-smoothstep(0.07+major*0.1,0.10+major*0.1,abs(angle-0.45)));
    glass = mix(glass,vec3(0.15,0.135,0.10)*e,potion ? 0.0 : ticks*0.48);
    // Dirt caught under the collars and a few dried runs on the outer glass. These are
    // fixed to the vessel, separate from the moving blood, with the central window clear.
    const float edge_d = max(min(in_local.y-lo,hi-in_local.y),0.0);
    const float edge_dirt = exp(-edge_d/0.005)*(0.35+0.65*age);
    const float run = pow(0.5+0.5*sin(angle*11.0+0.9*sin(angle*3.0)),12.0);
    const float dried_run = run*exp(-max(hi-in_local.y,0.0)/0.028);
    const float grime = clamp(0.62*edge_dirt+0.20*dried_run+
                              0.045*smoothstep(0.65,0.85,grain),0.0,0.68);
    glass = mix(glass,vec3(0.022,0.018,0.010)*diffuse*e*tint,grime);
    // A few broad islands of encrusted dirt on the OUTSIDE of the glass. Chord distances
    // around the cylinder avoid a UV seam; all marks stay on the vessel as the blood moves.
    const vec2 round = p.xz/max(length(p.xz),1e-5);
    const vec2 patch_a = (round-vec2(-0.60,-0.80))/0.585;
    const vec2 patch_b = (round-normalize(vec2(0.82,-0.57)))/0.45;
    const vec2 patch_c = (round-normalize(vec2(0.20,0.98)))/0.63;
    const vec3 patch_y = (vec3(in_local.y)-vec3(0.112,0.0635,0.090))/vec3(0.018,0.0135,0.0162);
    const float island_a = 1.0-dot(patch_a,patch_a)-patch_y.x*patch_y.x;
    const float island_b = 1.0-dot(patch_b,patch_b)-patch_y.y*patch_y.y;
    const float island_c = 1.0-dot(patch_c,patch_c)-patch_y.z*patch_y.z;
    const float deposit_field = max(island_a,max(island_b,island_c))+
                                (age-0.5)*0.95+(coarse-0.5)*0.60+(grain-0.5)*0.12;
    const float deposit_fw = max(fwidth(deposit_field),0.035);
    const float deposit = smoothstep(-deposit_fw,0.14+deposit_fw,deposit_field);
    const float haze = smoothstep(-0.48,-0.03,deposit_field)*(1.0-deposit);
    // Cloudy etched glass around the crust, dark broken edges and matte earthen deposits.
    // Neither the crust nor its clouded halo is red; the blood remains the only wet red.
    glass = mix(glass,glass*0.56+vec3(0.058,0.054,0.039)*diffuse*e,haze*0.58);
    vec3 deposit_rgb = mix(vec3(0.021,0.018,0.010),vec3(0.075,0.062,0.034),
                           smoothstep(0.18,0.85,age*0.62+grain*0.38))*diffuse*e*tint;
    const float broken_edge = smoothstep(0.0,0.09,deposit_field)*
                              (1.0-smoothstep(0.16,0.30,deposit_field));
    deposit_rgb *= (1.0-0.32*broken_edge)*(0.72+0.28*smoothstep(0.22,0.40,grain));
    glass = mix(glass,deposit_rgb,deposit*0.94);
    if (potion) {
        // Grimier than the vial: dried blood run down from the open mouth, a dark crust caked
        // round the base, and a dull film on the shoulder.
        const float runs = pow(0.5+0.5*sin(angle*7.0+2.0*sin(angle*2.0)+age*1.5),7.0);
        const float mouth = smoothstep(-0.035,0.060,in_local.y);
        const float drip = runs*mouth*smoothstep(0.42,0.58,coarse+0.35*mouth);
        glass = mix(glass,vec3(0.034,0.004,0.002)*diffuse*e*tint,clamp(drip,0.0,0.9));
        const float base = 1.0-smoothstep(-0.062,-0.040,in_local.y);
        glass = mix(glass,vec3(0.017,0.011,0.006)*diffuse*e*tint,clamp(base*(0.45+0.55*coarse),0.0,0.88));
        const float shoulder = smoothstep(0.012,0.032,in_local.y)*(1.0-smoothstep(0.045,0.060,in_local.y));
        glass = mix(glass,glass*0.55+vec3(0.030,0.026,0.016)*diffuse*e,shoulder*(0.35+0.4*age));
    }
    return vec4(mix(metal_rgb,glass,chamber),1.0);
}

// The Blood Vial's needle and its hub (core/vr/vial_needle.h): steel under the light of the hand
// holding it (HeldLight, as the vial: as bright as his hand, never glowing), but only uv.z of the
// colour of the light round it (that light is mostly his skin, his sleeve and the vial's blood:
// all of it made the needle pink): a diffuse of color.rgb, a faint sheen at grazing angles and a
// highlight of strength emissive.w, sharpness uv.w x 100.
//  - The needle (uv.x > 0) is a wire (vr_inframe.vert): in_local.x is this fragment's distance
//    from its axis in pixels, in_local.y the half width drawn, in_local.z the share of it that is
//    needle. Its coverage falls off over the last pixel of its edge. Its normal is rebuilt round
//    the axis (in_normal: the sideways direction) as far as it is wide enough to show it
//    (NeedleRoundness: from 3 pixels of half width): there the highlight is a thin line running
//    down one side of it, as on a real needle. Narrower, that line would be under a pixel and
//    bead in the eye images: the needle is one even line instead, facing the eye, its glint the
//    one it catches anywhere round it (the best normal in the circle of the sideways direction
//    and the eye's), spread over its width, so it flashes along its whole length as it turns.
//  - The hub (uv.x = 0) is a mesh: its silhouette fades over its last pixel.
vec4 ShadeSteelFn(float exposure) {
    vec3 tint;
    const float e = HeldLight(tint);
    tint = mix(vec3(dot(tint, vec3(0.2126, 0.7152, 0.0722))), tint, clamp(dd.uv.z, 0.0, 1.0));
    const vec3 v = normalize(-in_view);
    const vec3 l = normalize(vec3(-0.30, 0.80, -0.45)); // the vial's key light
    const vec3 h = normalize(l + v);
    const float sharp = max(dd.uv.w * 100.0, 4.0);
    vec3 n;
    float cover = 1.0;
    float glint = -1.0; // the narrow wire's even glint (< 0: none, a round normal)
    float round_k = 1.0;
    if (dd.uv.x > 0.0) {
        const float w = max(in_local.y, 1e-3);
        cover = clamp(w - abs(in_local.x) + 0.5, 0.0, 1.0) * clamp(in_local.z, 0.0, 1.0);
        round_k = clamp((w - 1.0) * 0.5, 0.0, 1.0); // VR::Held::NeedleRoundness
        const vec3 s = normalize(in_normal);
        const float u = clamp(in_local.x / w, -1.0, 1.0) * round_k;
        n = normalize(s * u + v * sqrt(max(1.0 - u * u, 0.0)));
        const float sh = dot(s, h);
        const float vh = max(dot(v, h), 0.0);
        glint = 0.5 * pow(min(sqrt(sh * sh + vh * vh), 1.0), sharp);
    } else {
        n = normalize(in_normal);
        if (dot(n, v) < 0.0) {
            n = -n;
        }
        const float facing = dot(n, v);
        cover = clamp(facing / max(fwidth(facing), 1e-4) + 0.25, 0.0, 1.0);
    }
    const float diffuse = max(dot(n, l), 0.0);
    float spec = pow(max(dot(n, h), 0.0), sharp);
    if (glint >= 0.0) {
        spec = mix(glint, spec, round_k);
    }
    spec *= dd.emissive.w;
    const float sheen = pow(1.0 - max(dot(n, v), 0.0), 4.0) * 0.08;
    const vec3 rgb =
        (dd.color.rgb * (0.35 + 0.80 * diffuse) + vec3(spec + sheen)) * tint * e * exposure;
    return vec4(rgb, dd.color.a * cover);
}

// The arc's card on a unit quad (mesh xy in -0.5 .. 0.5): a rounded square of translucent dark
// slate (color.rgb, opacity color.a), lighter at the top like the game's item boxes, a hairline
// frame of worn metal inset from its edge (emissive.rgb at emissive.a), and a soft warm glow
// from its centre (uv.x: strength 0..1), the game's own menu-selection candlelight. Everything
// anti-aliased from the mesh coordinates; the scene's light dims the slate and the frame a little
// in the dark (never the glow).
vec4 ShadeArcCard(float exposure) {
    if (CandleItem()) {
        return CandleFlame();
    }
    float scene_luma;
    SceneLight(scene_luma);
    const float scene_level = clamp(scene_luma * 4.0, 0.0, 1.0);
    const vec2 p = in_local.xy;
    const float radius = 0.10;
    const vec2 q = abs(p) - vec2(0.5 - radius);
    const float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius; // < 0 inside
    const float fw = max(fwidth(d), 1e-5);
    const float inside = clamp(-d / fw + 0.5, 0.0, 1.0);
    // The frame: a line `width` wide, `inset` inside the edge.
    const float inset = 0.045;
    const float width = 0.016;
    const float frame = clamp((0.5 * width - abs(d + inset)) / fw + 0.5, 0.0, 1.0);
    // The slate: a gentle vertical gradient and a darker rim (the game's boxes are lit from above).
    const float shade = mix(0.75, 1.15, clamp(0.5 - p.y, 0.0, 1.0)) *
                        mix(0.80, 1.0, clamp(-d * 6.0, 0.0, 1.0));
    vec3 rgb = dd.color.rgb * shade * mix(0.7, 1.0, scene_level) * exposure;
    float alpha = dd.color.a * inside;
    // The candlelight: a soft warm glow from the centre, under the icon.
    const float glow = dd.uv.x * exp(-dot(p, p) / 0.09) * inside;
    rgb += vec3(1.00, 0.62, 0.30) * glow * 0.55;
    alpha = max(alpha, glow * 0.6);
    // The frame over it all.
    const float fa = frame * dd.emissive.a * inside;
    rgb = mix(rgb, dd.emissive.rgb * mix(0.75, 1.0, scene_level) * exposure, fa);
    alpha = max(alpha, fa);
    return vec4(rgb, alpha);
}


// ---- The Blood Gauge (core/vr/vitals.h; core/vr/bloodborne_vitals.cpp builds its items) --------
// An aged brass plate strapped to the left wrist with two leather straps, three glass tubes in
// it read along the arm (full toward the hand): blood (HP, the game's orange rally segment as an
// amber layer above it), olive ichor (stamina; the glass fogs grey when it runs out) and
// quicksilver (bullets, brass ticks every 5), and the counter's window at the hand end of the
// quicksilver tube (its digits are Text items on it). Fake 3D: analytic capsules and a height
// field in the plate's own millimetres (the design's, +15%), lit like the scene round it, the
// liquids with a floor so they read in the dark. color = the levels (hp, rally, stamina,
// bullets), emissive = (glow, heartbeat pulse, fog, heal bubbles' phase), uv = (alpha,
// quicksilver tick step, part: 0 plate / 1 strap, shake mm).
const float G_GL = 52.9;
const float G_GW = 34.5;
const float G_MARGIN_U = 2.0; // the plate mesh's margin round the plate (the lugs)
const float G_MARGIN_V = 3.0;
const vec4 G_TUBE_HP = vec4(8.74, 5.18, 3.45, 49.68); // centre v, radius, u0, u1
const vec4 G_TUBE_ST = vec4(20.36, 3.45, 3.45, 33.81);
const vec4 G_TUBE_QS = vec4(28.64, 2.30, 3.45, 33.81);
const vec4 G_WIN = vec4(36.1, 16.45, 49.7, 32.3); // u0 v0 u1 v1
const float G_FERRULE = 1.96;
const vec2 G_STRAP1 = vec2(6.9, 13.2);
const vec2 G_STRAP2 = vec2(39.7, 46.0);

const vec3 G_BLOOD = vec3(0.3278, 0.0110, 0.0130);
const vec3 G_FRESH = vec3(0.6308, 0.0232, 0.0160);
const vec3 G_BLOOD_DEEP = vec3(0.0176, 0.0012, 0.0015);
const vec3 G_AMBER = vec3(0.6038, 0.2307, 0.0252);
const vec3 G_AMBER_HI = vec3(0.8714, 0.4452, 0.1070);
const vec3 G_AMBER_DEEP = vec3(0.0685, 0.0160, 0.0021);
const vec3 G_STAM = vec3(0.1590, 0.2747, 0.0762);
const vec3 G_STAM_GLOW = vec3(0.3278, 0.5210, 0.1441);
const vec3 G_STAM_DEEP = vec3(0.0116, 0.0194, 0.0060);
const vec3 G_BONE = vec3(0.7913, 0.7157, 0.5647);
const vec3 G_BRASS = vec3(0.3916, 0.2270, 0.0685);
const vec3 G_BRASS_SH = vec3(0.1470, 0.0844, 0.0296);
const vec3 G_BRASS_DK = vec3(0.0160, 0.0091, 0.0040);
const vec3 G_QS = vec3(0.5841, 0.6172, 0.6514);
const vec3 G_QS_HI = vec3(0.8550, 0.8963, 0.9301);
const vec3 G_QS_DK = vec3(0.0296, 0.0356, 0.0437);
const vec3 G_SOOT = vec3(0.0060, 0.0040, 0.0033);
const vec3 G_LEATHER = vec3(0.0685, 0.0307, 0.0160);
const vec3 G_LEATHER_HI = vec3(0.1946, 0.0931, 0.0395);
const vec3 G_FOG = vec3(0.3419, 0.3712, 0.3613);
const vec3 G_DRUM = vec3(0.0437, 0.0252, 0.0137);

float GHash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float GNoise(vec2 p) {
    const vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(GHash(i), GHash(i + vec2(1.0, 0.0)), f.x),
               mix(GHash(i + vec2(0.0, 1.0)), GHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float GFbm(vec2 p) {
    return 0.57 * GNoise(p) + 0.29 * GNoise(p * 2.03 + 7.1) + 0.14 * GNoise(p * 4.11 + 3.7);
}

float GSdBox(vec2 p, vec2 c, vec2 h, float r) {
    const vec2 q = abs(p - c) - h + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// A tube along u (t = centre v, radius, u0, u1).
float GSdTube(vec2 p, vec4 t) {
    const float a = t.z + t.y;
    const float b = t.w - t.y;
    return length(vec2(p.x - clamp(p.x, a, b), p.y - t.x)) - t.y;
}

float GPlateSd(vec2 p) {
    float sd = GSdBox(p, 0.5 * vec2(G_GL, G_GW), 0.5 * vec2(G_GL, G_GW), 0.46);
    // The notched corners.
    sd = max(sd, 2.9 - length(p));
    sd = max(sd, 2.9 - length(p - vec2(0.0, G_GW)));
    sd = max(sd, 2.9 - length(p - vec2(G_GL, 0.0)));
    sd = max(sd, 2.9 - length(p - vec2(G_GL, G_GW)));
    return sd;
}

// The lugs the straps leave the plate through.
float GLugSd(vec2 p) {
    float sd = 1.0e3;
    for (int i = 0; i < 2; ++i) {
        const vec2 s = i == 0 ? G_STRAP1 : G_STRAP2;
        const float cu = 0.5 * (s.x + s.y);
        const float hu = 0.5 * (s.y - s.x) + 0.2;
        sd = min(sd, GSdBox(p, vec2(cu, -0.9), vec2(hu, 1.15), 0.7));
        sd = min(sd, GSdBox(p, vec2(cu, G_GW + 0.9), vec2(hu, 1.15), 0.7));
    }
    return sd;
}

float GWinSd(vec2 p) {
    return GSdBox(p, 0.5 * (G_WIN.xy + G_WIN.zw), 0.5 * (G_WIN.zw - G_WIN.xy), 1.15);
}

// A level (0 .. 1) as a u inside the tube between its ferrules.
float GLevel(vec4 t, float f) {
    const float a = t.z + G_FERRULE;
    const float b = t.w - G_FERRULE;
    return a + clamp(f, 0.0, 1.0) * (b - a);
}

// The quicksilver tube's ticks (every `step` of it), raised brass beside it.
float GTicksSd(vec2 p, float tick_step) {
    float sd = 1.0e3;
    if (tick_step > 0.02) {
        for (int k = 1; k < 12; ++k) {
            const float f = float(k) * tick_step;
            if (f > 0.999) {
                break;
            }
            sd = min(sd, GSdBox(p, vec2(GLevel(G_TUBE_QS, f), 32.2), vec2(0.48, 1.4), 0.29));
        }
    }
    return sd;
}

// The plate's relief (mm): the plate, its engraved border, the tubes' channels, the window's
// bezel and recess, the ticks and the lugs.
float GHeight(vec2 p, float tick_step) {
    const float sdp = GPlateSd(p);
    float h = smoothstep(0.0, 0.85, -sdp);
    h -= 0.16 * (1.0 - smoothstep(0.0, 0.32, abs(-sdp - 1.8)));
    h -= 0.8 * smoothstep(0.0, 0.55, -GSdTube(p, G_TUBE_HP + vec4(0.0, 0.8, -0.6, 0.6)));
    h -= 0.8 * smoothstep(0.0, 0.55, -GSdTube(p, G_TUBE_ST + vec4(0.0, 0.8, -0.6, 0.6)));
    h -= 0.8 * smoothstep(0.0, 0.55, -GSdTube(p, G_TUBE_QS + vec4(0.0, 0.8, -0.6, 0.6)));
    const float sw = GWinSd(p);
    h += 0.35 * clamp(1.0 - pow((sw - 0.6) / 0.6, 2.0), 0.0, 1.0) * step(0.0, sw);
    h -= 0.8 * smoothstep(0.0, 0.35, -sw);
    h += 0.35 * smoothstep(0.17, -0.17, GTicksSd(p, tick_step));
    h = max(h, 0.6 * smoothstep(0.0, 0.55, -GLugSd(p)));
    return h;
}

float GLuma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec4 ShadeBloodGauge(float exposure) {
    const int part = int(dd.uv.z + 0.5);
    const float alpha_all = clamp(dd.uv.x, 0.0, 1.0);
    float scene_luma;
    const vec3 scene = SceneLight(scene_luma);
    const float level = clamp(scene_luma * 4.0, 0.0, 1.0);
    const vec3 tint = mix(vec3(1.0), clamp(scene / max(scene_luma, 1e-3), 0.6, 1.6), 0.5);
    const vec3 IL = tint * mix(0.30, 1.0, level) * exposure; // the light on the brass
    const float liquid_light = max(GLuma(IL), 0.55) * exposure; // the liquids' floor
    vec3 n = normalize(in_normal);
    const vec3 v = normalize(-in_view);
    if (dot(n, v) < 0.0) {
        n = -n; // two-sided (no culling: the view space is left-handed)
    }
    const vec3 L = normalize(vec3(0.35, 0.80, -0.45));
    const vec3 H = normalize(L + v);
    if (part == 1) {
        // A strap: leather round the arm, its ends fading where it reaches the sleeve's side (the
        // fit's end, uv.w) or goes round out of sight. uv.y: the side of the ring this item draws
        // (+1: a >= 0, -1: a <= 0, 0: both; the seam is under the plate).
        const float a = atan(in_local.z, in_local.y);
        const float side = dd.uv.y;
        if (side * a < -0.02) {
            return vec4(0.0);
        }
        const float across = in_local.x + 0.5;
        const float fw_a = max(fwidth(across), 1e-4);
        const float cover = clamp(min(across, 1.0 - across) / fw_a, 0.0, 1.0);
        // (a round strap: from 37 degrees, where an oval forearm curves away under it)
        const float a_end = dd.uv.w > 0.1 ? dd.uv.w : 0.95;
        const float ends = 1.0 - smoothstep(a_end - 0.30, a_end, abs(a));
        const float grain = GFbm(vec2(across * 7.0, a * 45.0));
        const float edge = smoothstep(0.0, 0.16, across) * smoothstep(1.0, 0.84, across);
        vec3 alb = mix(G_LEATHER, G_LEATHER_HI, 0.2 + 0.5 * grain) * mix(0.55, 1.0, edge);
        // A row of stitches along each edge.
        const float stitch = step(0.55, fract(a * 26.0)) *
                             (1.0 - smoothstep(0.03, 0.07, abs(abs(across - 0.5) - 0.36)));
        alb = mix(alb, G_BONE * 0.35, 0.6 * stitch);
        const float diff = max(dot(n, L), 0.0);
        const float spec = pow(max(dot(n, H), 0.0), 12.0) * 0.06;
        const vec3 rgb = alb * (0.30 + 0.85 * diff) * IL + vec3(spec) * IL;
        return vec4(rgb, alpha_all * cover * ends);
    }
    const float step_q = dd.uv.y;
    const vec2 p = vec2((in_local.x + 0.5) * (G_GL + 2.0 * G_MARGIN_U) - G_MARGIN_U,
                        (in_local.z + 0.5) * (G_GW + 2.0 * G_MARGIN_V) - G_MARGIN_V);
    const float fw = max(0.5 * (fwidth(p.x) + fwidth(p.y)), 1e-3); // mm per pixel
    const float sdp = GPlateSd(p);
    const float sd_all = min(sdp, GLugSd(p));
    const float cover = clamp(0.5 - sd_all / fw, 0.0, 1.0);
    if (cover <= 0.0) {
        return vec4(0.0);
    }
    // The relief's normal (a cotangent frame from the screen derivatives).
    const float e = 0.12;
    const float h0 = GHeight(p, step_q);
    const float hu = (GHeight(p + vec2(e, 0.0), step_q) - h0) / e;
    const float hv = (GHeight(p + vec2(0.0, e), step_q) - h0) / e;
    const vec3 dp1 = dFdx(in_view);
    const vec3 dp2 = dFdy(in_view);
    const vec2 dq1 = dFdx(p);
    const vec2 dq2 = dFdy(p);
    const vec3 dp2perp = cross(dp2, n);
    const vec3 dp1perp = cross(n, dp1);
    vec3 T = dp2perp * dq1.x + dp1perp * dq2.x;
    vec3 B = dp2perp * dq1.y + dp1perp * dq2.y;
    const float inv = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-30));
    const float handed = dot(n, cross(dp1, dp2)) < 0.0 ? -1.0 : 1.0;
    T *= inv * handed;
    B *= inv * handed;
    const vec3 nb = normalize(n - 1.4 * (hu * T + hv * B));
    // Aged brass.
    const float grime = GFbm(p / 2.4);
    const float fine = GFbm(vec2(p.x * 0.35, p.y * 3.0) / 0.6);
    vec3 alb = mix(G_BRASS, G_BRASS_SH, 0.30) * (0.55 + 0.42 * grime) * (0.88 + 0.24 * fine);
    const float tarn = smoothstep(0.35, 0.80, GFbm(p / 1.6 + 13.0)) * 0.62;
    alb = mix(alb, G_BRASS_DK * 1.2, tarn);
    const float stain = smoothstep(0.64, 0.80, GFbm(p / 3.2 + 9.0)) * 0.7;
    alb = mix(alb, vec3(0.0212, 0.0015, 0.0015), stain);
    const float cav = smoothstep(-0.05, 0.85, h0);
    alb = mix(G_BRASS_DK * 0.6, alb, 0.25 + 0.75 * cav);
    const float slope = 1.0 - clamp(dot(nb, n), 0.0, 1.0);
    const float near_edge = clamp(smoothstep(1.3, 0.2, -sdp) +
                                      smoothstep(1.6, 0.2, abs(GWinSd(p) - 0.55)),
                                  0.0, 1.0);
    const float polish = smoothstep(0.05, 0.35, slope) * (1.0 - stain) * near_edge;
    alb = mix(alb, G_BRASS * 1.05, 0.55 * polish);
    const float diff = max(dot(nb, L), 0.0);
    const float sp = max(dot(nb, H), 0.0);
    const vec3 spec_tint = mix(G_BRASS / 0.3916, vec3(1.0), 0.25);
    const float spw = (0.22 * pow(sp, 18.0) + 0.7 * polish * pow(sp, 6.0)) * (1.0 - 0.6 * tarn);
    vec3 col = IL * (alb * (0.14 + 0.80 * diff) + spec_tint * spw * 0.65);
    col *= 1.0 - 0.45 * smoothstep(-0.3, 0.0, sd_all); // a dark outer edge
    const float glow = dd.emissive.x;
    const float pulse = dd.emissive.y;
    const float fog = dd.emissive.z;
    const float bubbles = dd.emissive.w;
    // The counter's window: a dark drum recess (the digits are drawn over it).
    const float sw = GWinSd(p);
    const float win = clamp(0.5 - sw / fw, 0.0, 1.0);
    if (win > 0.0) {
        const float wv = clamp((p.y - G_WIN.y) / (G_WIN.w - G_WIN.y) * 2.0 - 1.0, -1.0, 1.0);
        const vec3 drum = G_DRUM * (0.45 + 0.35 * GFbm(p * 0.9)) *
                              (0.55 + 0.45 * sqrt(max(1.0 - wv * wv, 0.0))) *
                              mix(0.55, 1.0, level) * exposure +
                          G_AMBER * glow * 0.05;
        col = mix(col, drum, win);
    }
    // The glass tubes and their liquids (shaken a little after a hit).
    const vec2 pt = p + vec2(0.0, dd.uv.w);
    for (int k = 0; k < 3; ++k) {
        const vec4 t = k == 0 ? G_TUBE_HP : (k == 1 ? G_TUBE_ST : G_TUBE_QS);
        const float sdt = GSdTube(pt, t);
        const float m = clamp(0.5 - sdt / fw, 0.0, 1.0);
        if (m <= 0.0) {
            continue;
        }
        const float w = clamp((pt.y - t.x) / t.y, -1.0, 1.0);
        const float z = sqrt(max(1.0 - w * w, 0.0));
        const float ri = t.y - 0.44;
        const float wi = clamp((pt.y - t.x) / ri, -1.0, 1.0);
        const float zi = sqrt(max(1.0 - wi * wi, 0.0));
        const float inner = clamp(0.5 - (abs(pt.y - t.x) - ri) / fw, 0.0, 1.0);
        const float a0 = t.z + G_FERRULE;
        const vec3 gcol = IL * (G_SOOT * (0.6 + 0.7 * zi) + G_BRASS_SH * 0.06 * zi);
        vec3 liq = vec3(0.0);
        float la = 0.0;
        if (k == 0) {
            const float lv = GLevel(t, dd.color.x);
            const float rv = GLevel(t, max(dd.color.x, dd.color.y));
            const float b = lv + 0.5 * (wi * wi - 0.35);
            const float inl = clamp(0.5 - (pt.x - b) / fw, 0.0, 1.0) *
                              clamp(0.5 - (a0 - 0.3 - pt.x) / fw, 0.0, 1.0) *
                              step(0.001, dd.color.x);
            const float d = b - pt.x;
            vec3 c = mix(G_BLOOD_DEEP, G_BLOOD, pow(zi, 0.9));
            c = mix(c, G_BLOOD_DEEP * 0.8, 0.40 * smoothstep(12.0, 34.0, d));
            c *= 0.78 + 0.34 * GFbm(vec2(pt.x * 0.45, pt.y * 1.6) / 2.2);
            c = mix(c, G_FRESH, 0.45 * smoothstep(3.5, 0.0, d) * zi);
            const float ml = smoothstep(0.5, 0.0, d) * step(-0.05, d);
            c += (G_FRESH * 0.9 + G_BONE * 0.08) * ml * 0.85 * (0.4 + 0.6 * zi);
            c *= 1.0 + 0.7 * pulse; // the heartbeat's glow
            if (bubbles > 0.0) {
                // A heal: three bright bubbles rising through the blood.
                for (int i = 0; i < 3; ++i) {
                    const float ph = fract(bubbles * 1.3 + float(i) * 0.31);
                    const vec2 bc = vec2(a0 + (lv - a0) * ph, t.x + (float(i) - 1.0) * 0.38 * ri);
                    const float bd = length(pt - bc) - 0.55;
                    c += G_BONE * 0.9 * clamp(0.5 - bd / fw, 0.0, 1.0) * (1.0 - bubbles);
                }
            }
            liq = c * inl;
            la = inl;
            if (rv > lv + 0.05) {
                // The rally: amber, lighter, above the blood; the game drains it from the top.
                const float rb = rv + 0.45 * (wi * wi - 0.35);
                const float inr = clamp(0.5 - (pt.x - rb) / fw, 0.0, 1.0) *
                                  clamp(0.5 - (b - pt.x) / fw, 0.0, 1.0);
                vec3 ca = mix(G_AMBER_DEEP, G_AMBER, pow(zi, 0.8)) *
                          (0.9 + 0.2 * smoothstep(2.0, 0.0, pt.x - b));
                const float rd = rb - pt.x;
                ca += G_AMBER_HI * smoothstep(0.45, 0.0, rd) * step(-0.05, rd) * 0.6 *
                      (0.4 + 0.6 * zi);
                const float ra = 0.9 * inr;
                liq = liq * (1.0 - ra) + ca * ra;
                la = la + ra * (1.0 - la);
            }
        } else if (k == 1) {
            const float lv = GLevel(t, dd.color.z);
            const float b = lv + 0.4 * (wi * wi - 0.35);
            const float inl = clamp(0.5 - (pt.x - b) / fw, 0.0, 1.0) *
                              clamp(0.5 - (a0 - 0.3 - pt.x) / fw, 0.0, 1.0) *
                              step(0.001, dd.color.z);
            const float d = b - pt.x;
            vec3 c = mix(G_STAM_DEEP, G_STAM, pow(zi, 0.9));
            c *= 0.82 + 0.3 * GFbm(vec2(pt.x * 0.45, pt.y * 1.6) / 2.0);
            c = c * 0.86 + G_STAM_GLOW * 0.05 * pow(zi, 6.0);
            c += G_STAM_GLOW * smoothstep(0.45, 0.0, d) * step(-0.05, d) * 0.55 * (0.4 + 0.6 * zi);
            liq = c * inl;
            la = inl;
        } else {
            const float lv = GLevel(t, dd.color.w);
            const float b = lv - 0.9 * (1.0 - sqrt(max(1.0 - wi * wi, 0.0)));
            const float inl = clamp(0.5 - (pt.x - b) / fw, 0.0, 1.0) *
                              clamp(0.5 - (a0 - 0.3 - pt.x) / fw, 0.0, 1.0) *
                              step(0.001, dd.color.w);
            vec3 c = G_QS_DK;
            c += G_QS_HI * 0.95 * exp(-pow((wi + 0.40) / 0.22, 2.0));
            c += G_QS * 0.45 * exp(-pow((wi - 0.60) / 0.22, 2.0));
            c *= 1.0 - 0.5 * exp(-pow((wi - 0.12) / 0.15, 2.0));
            c += G_QS_HI * smoothstep(0.8, 0.0, b - pt.x) * step(-0.05, b - pt.x) * 0.3 * zi;
            liq = c * inl;
            la = inl;
        }
        const vec3 liq_out = liq * (k == 2 ? max(GLuma(IL) * exposure, 0.6 * exposure)
                                           : liquid_light);
        vec3 interior = gcol * (1.0 - la) + liq_out;
        if (k == 1 && fog > 0.0) {
            const float fv = fog * smoothstep(0.25, 0.75, GFbm(vec2(pt.x, pt.y * 1.4) / 2.2)) *
                             (0.55 + 0.45 * (1.0 - zi));
            interior = mix(interior, vec3(GLuma(interior)), 0.3 * fog);
            interior = mix(interior, G_FOG * IL * 0.40, clamp(0.16 * fog + fv * 0.36, 0.0, 1.0));
        }
        interior += liq_out * glow * 1.6; // a glance or a change: a soft inner glow
        if (k == 0) {
            interior += G_FRESH * 0.10 * pulse * liquid_light; // the lub flashes the glass red
        }
        const vec3 wall = IL * (G_BRASS_SH * 0.18 + 0.03) * (0.4 + 0.6 * z);
        vec3 tube = mix(wall, interior, inner);
        // The glass's highlights: a broken strip along it, a fainter one, the rims.
        const float along = smoothstep(t.z + 0.8, t.z + 2.8, pt.x) *
                            smoothstep(t.w - 0.8, t.w - 2.8, pt.x);
        const float brk =
            0.55 + 0.45 * smoothstep(0.3, 0.6, GNoise(vec2(pt.x / 3.5, float(k) * 5.0)));
        const float s1 = exp(-pow((w + 0.50) / 0.09, 2.0)) * brk;
        const float s2 = exp(-pow((w - 0.64) / 0.07, 2.0));
        const float rimf = smoothstep(0.84, 1.0, abs(w));
        const float hi = (0.50 * s1 + 0.10 * s2) * along + 0.10 * rimf;
        tube += vec3(hi) * mix(0.35, 1.0, level) * exposure *
                (0.6 + 0.4 * pow(max(dot(n, H), 0.0), 4.0));
        // The brass ferrules capping both ends.
        const float fe = max(smoothstep(t.z + G_FERRULE + 0.2, t.z + G_FERRULE - 0.2, pt.x),
                             smoothstep(t.w - G_FERRULE - 0.2, t.w - G_FERRULE + 0.2, pt.x));
        const vec3 ferrule = IL * G_BRASS * (0.25 + 0.95 * z) +
                             vec3(0.35 * pow(z, 8.0)) * IL * mix(0.4, 1.0, level);
        tube = mix(tube, ferrule, fe);
        col = mix(col, tube, m);
    }
    return vec4(col, cover * alpha_all);
}

// quick_hold_use's ring (core/vr/bloodborne_quickitems.cpp AddHoldRing) on a quad facing the
// eye round the held item: a thin band (radius uv.z, half width uv.w, of the quad), its dim track
// (color.rgb at color.a: dark slate, readable on a bright street) filling clockwise from the top
// with the fill (emissive.rgb at emissive.a: bone warming to gold) as uv.x goes 0..1, the leading
// end a little brighter; uv.y the finish's flash (1 at the use, fading): the band full and
// brightened, a soft golden halo round it. Anti-aliased from the mesh coordinates (the angle's
// pixel width from the radius': no seam at the top).
vec4 ShadeHoldRingFn(float exposure, float scene_level) {
    const vec2 p = in_local.xy;
    const float r = length(p);
    const float fw = max(fwidth(r), 1e-5);
    const float band = clamp((dd.uv.w - abs(r - dd.uv.z)) / fw + 0.5, 0.0, 1.0);
    const float a = fract(atan(p.x, p.y) / 6.2831853 + 1.0); // 0 at the top, clockwise
    const float afw = fw / max(r, 1e-3) / 6.2831853;
    const float progress = clamp(dd.uv.x, 0.0, 1.0);
    const float flash = clamp(dd.uv.y, 0.0, 1.0);
    float filled = progress >= 0.999 ? 1.0 : clamp((progress - a) / afw + 0.5, 0.0, 1.0);
    filled = max(filled, step(0.001, flash));
    // The leading end: the last few degrees of the fill a little brighter (where it is now).
    const float lead = filled * smoothstep(progress - 0.08, progress, a) * (1.0 - flash);
    const float level = mix(0.8, 1.0, scene_level) * exposure;
    vec3 rgb = mix(dd.color.rgb * level, dd.emissive.rgb * level * (1.0 + 0.45 * lead),
                   filled);
    rgb *= 1.0 + 1.6 * flash;
    float alpha = band * mix(dd.color.a, dd.emissive.a, filled);
    // The flash: a soft golden halo hugging the band (a glint off the ring, not a veil over the
    // item) and a faint warm glint at its centre.
    const float d = (r - dd.uv.z) / 0.07;
    const float glow = flash * (0.42 * exp(-d * d) + 0.10 * exp(-(r * r) / 0.02));
    rgb = mix(vec3(1.00, 0.74, 0.40) * level * 1.35, rgb, alpha / max(alpha + glow, 1e-4));
    alpha = max(alpha, glow);
    return vec4(rgb, alpha);
}

// quick_arc_pick=fan's segment (core/vr/bloodborne_quickitems.cpp AddFanSegment) on a quad in the
// fan's plane, its y out from the hand along the segment's middle: radii uv.y .. 1 (of the outer
// radius), uv.x radians either side of the middle (uv.z: that middle's direction in the fan, for
// the ticks; uv.w: the icons' radius). color: x the lit glow (eased), y a deny's flash, z the
// game's selected item, w the opacity. emissive: x the style (quick_fan_style), y the fan's ends
// on this quad's sides (bit 1: -x, 2: +x; bit 4: +x is the viewer's right), z how far the quad
// reaches past its sides (an end's mullion or stud is drawn whole there). Each segment draws its
// half of what it shares with a neighbour. Lines keep at least half a pixel (thinner ones fade
// instead of shimmering). Dims with the scene round it as the cards do.
//   0 Fan A, lancet: a pointed gothic pane of smoky glass per item (denser round the icons, so
//     they read on the streets), iron mullions with a brass edge, a brass tracery line; lit, the
//     glass glows as if a candle burned behind it, blood red to ember, and its arch turns gold.
//   1 Fan B, brass: a dark fan inside an instrument's double brass rim, engraved ticks every 3
//     degrees and a diamond stud at each division, engraved hairlines between the items; lit, an
//     ember glow between Fan A's candlelight and a plain ember rising from the rim.
const vec3 FanIron = vec3(0.0080, 0.0065, 0.0056);
const vec3 FanBrass = vec3(0.392, 0.216, 0.061);
const vec3 FanBrassHi = vec3(0.760, 0.468, 0.138);
const vec3 FanBlood = vec3(0.195, 0.006, 0.007);
const vec3 FanEmber = vec3(0.672, 0.231, 0.037);
const vec3 FanRust = vec3(0.252, 0.072, 0.042);

// A cheap arithmetic hash (no sin: exact on every GPU) and its value noise, for the lit glass's
// grain only.
float FanHash(vec2 i) {
    vec3 q = fract(vec3(i.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float FanNoise(vec2 q) {
    const vec2 i = floor(q);
    vec2 f = fract(q);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(FanHash(i), FanHash(i + vec2(1.0, 0.0)), f.x),
               mix(FanHash(i + vec2(0.0, 1.0)), FanHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// A line of half width `hw` round dist = 0 (fw: a pixel in dist's units), at least half a pixel
// wide, fainter where it would be thinner.
float FanLine(float dist, float hw, float fw) {
    const float w = max(hw, 0.5 * fw);
    return clamp((w - abs(dist)) / fw + 0.5, 0.0, 1.0) * (hw / w);
}

float FanInside(float dist, float fw) {
    return clamp(dist / fw + 0.5, 0.0, 1.0);
}

void FanOver(inout vec4 acc, vec3 rgb, float a) {
    a = clamp(a, 0.0, 1.0);
    acc = vec4(rgb * a + acc.rgb * (1.0 - a), a + acc.a * (1.0 - a));
}

vec4 ShadeFanFn(float exposure, float scene_level) {
    const float h = dd.uv.x;
    const float k = dd.uv.y;
    const float s = sin(h);
    const float c = cos(h);
    const vec2 p = vec2(in_local.x * 2.0 * (s + dd.emissive.z), mix(k * c, 1.0, in_local.y + 0.5));
    const float r = length(p);
    // Every field below is a distance in these units, so one derivative serves all their lines
    // (taken here, before any pixel leaves).
    const float fw = max(fwidth(r), 1e-5);
    // From the nearer long edge (a line through the hand at +-h), inward; past an edge that is not
    // the fan's end the neighbour draws.
    const float side = p.y * s - abs(p.x) * c;
    const int ends = int(dd.emissive.y + 0.5);
    const bool end_side = (ends & (p.x < 0.0 ? 1 : 2)) != 0;
    // Nothing to draw: past the rim, inside the foot, past a neighbour's edge, or past an end's
    // mullion and stud (the quad's corners: about a third of it).
    if (r > 1.0 + fw || r < k - 0.03 || side < (end_side ? -0.031 : -fw)) {
        return vec4(0.0);
    }
    // The pane's own pixels stop at the edge (anti-aliased); what is centred on an edge (a
    // mullion, a division, a stud) both neighbours draw whole up to a pixel past it, so the two
    // halves overlap a little instead of leaving a lighter seam.
    const float in_sector = FanInside(side, fw);
    // Across the segment, -1 .. 1 (tan phi / tan h: as good as the angle for the shapes).
    const float u = clamp(p.x * c / max(p.y * s, 1e-5), -1.0, 1.0);
    const float au = abs(u);
    const float t = (r - k) / max(1.0 - k, 1e-3);
    const float r_icon = dd.uv.w;
    const float glow = dd.color.x;
    const float deny = dd.color.y;
    const float selected = dd.color.z;
    // The candle behind the lit glass: brightest at the icon, blood red toward the edges.
    const float candle = 1.0 - clamp(length(vec2(u, (r - r_icon) / 0.33)), 0.0, 1.0);
    vec4 acc = vec4(0.0);
    // The style, glow and deny are the draw's own (push constants): these branches never diverge.
    if (int(dd.emissive.x + 0.5) == 1) {
        // Fan B: engraved brass.
        const float ro = 0.981;
        const float ri = 0.948;
        const float fan = in_sector * FanInside(r - k, fw) * FanInside(1.0 - r, fw);
        const float fade = smoothstep(0.0, 0.35, t) * (1.0 - smoothstep(0.97, 1.0, t));
        FanOver(acc, vec3(0.0040, 0.0037, 0.0044), fan * fade * 0.6 * (0.75 + 0.25 * t));
        if (glow > 0.0) {
            const float grain = 0.5 * FanNoise(p * 40.0) + 0.5 * FanNoise(p * 121.0);
            const float g = smoothstep(0.25, 1.0, t);
            const vec3 ember = mix(vec3(0.045, 0.016, 0.005), FanEmber, g * g);
            const vec3 lit = mix(ember, mix(FanBlood, FanEmber, candle), 0.5) + (grain - 0.5) * 0.03;
            const float a = mix(0.35 + 0.45 * g, 0.55 + 0.3 * candle, 0.5);
            FanOver(acc, lit, fan * fade * a * glow);
        }
        if (deny > 0.0) {
            FanOver(acc, FanRust, fan * fade * 0.6 * deny);
        }
        const vec3 rim = mix(mix(FanBrass, FanBrassHi, 0.35 * selected), FanBrassHi, glow);
        const float rims = FanLine(r - ro, 0.00275, fw) + FanLine(r - ri, 0.0022, fw);
        FanOver(acc, rim, in_sector * min(rims, 1.0) * mix(0.8, 0.95, glow));
        // Ticks every 3 degrees of the fan between the rims (only the band there needs the angle).
        if (r > ri && r < ro) {
            const float sigma = (ends & 4) != 0 ? 1.0 : -1.0;
            const float step3 = 3.0 * 3.14159265 / 180.0;
            const float q = (dd.uv.z + sigma * atan(p.x, p.y)) / step3;
            const float tick_d = abs(fract(q + 0.5) - 0.5) * step3 * r;
            const float tick = FanLine(tick_d, 0.0014, fw) * FanInside(r - (ri + 0.0055), fw) *
                               FanInside((ro - 0.0055) - r, fw);
            FanOver(acc, rim, in_sector * tick * mix(0.6, 0.9, glow));
        }
        // Engraved divisions between neighbours (none at the fan's ends).
        if (!end_side) {
            const float span = FanInside(r - (k + 0.06), fw) * FanInside(ri - r, fw);
            FanOver(acc, vec3(0.0012, 0.0012, 0.0015), span * FanLine(side, 0.00275, fw) * 0.85);
            FanOver(acc, vec3(0.188, 0.138, 0.080), span * FanLine(side - 0.0044, 0.0014, fw) * 0.35);
        }
        // A diamond stud on the rim at each division (an end's whole, on the quad's margin).
        const float sx = p.x < 0.0 ? -s : s;
        const vec2 er = vec2(sx, c);
        const vec2 et = vec2(c, -sx);
        const vec2 d = p - 0.9645 * er;
        const float l1 = abs(dot(d, er)) + abs(dot(d, et));
        if (l1 < 0.03) {
            FanOver(acc, FanIron, (1.0 - smoothstep(0.0248, 0.0289, l1)) * 0.95);
            FanOver(acc, mix(FanBrassHi, vec3(1.0, 0.78, 0.36), 0.5 * glow),
                    (1.0 - smoothstep(0.0151, 0.0193, l1)) * 0.9);
        }
        // The inner rim.
        FanOver(acc, FanBrass, in_sector * FanLine(r - (k + 0.055), 0.0017, fw) * 0.45);
    } else {
        // Fan A: the lancet window.
        const float r_out = 0.985 - 0.125 * (0.4 * au + 0.6 * au * au);
        const float r_in = k + 0.0386 * (1.0 - u * u);
        const float pane = in_sector * FanInside(r_out - r, fw) * FanInside(r - r_in, fw);
        const float fade = smoothstep(0.0, 0.30, t);
        // Smoky glass, denser round the icons so they never melt into the street (the user,
        // 2026-10-27).
        const float lq = (r - r_icon) / (0.3 * (1.0 - k));
        FanOver(acc, vec3(0.0130, 0.0116, 0.0152), pane * fade * (0.58 + 0.22 * exp(-lq * lq)));
        if (glow > 0.0) {
            const float grain = 0.5 * FanNoise(p * 40.0) + 0.5 * FanNoise(p * 121.0);
            const vec3 lit = mix(FanBlood, FanEmber, candle) + (grain - 0.5) * 0.06;
            FanOver(acc, lit, pane * fade * (0.55 + 0.3 * candle) * glow);
        }
        if (deny > 0.0) {
            FanOver(acc, FanRust, pane * fade * 0.6 * deny);
        }
        // Tracery: a brass line inside the arch and round its foot.
        const float trace = FanLine(r - (r_out - 0.022), 0.0022, fw) +
                            FanLine(r - (r_in + 0.0165), 0.0017, fw);
        FanOver(acc, FanBrass,
                in_sector * min(trace, 1.0) * (1.0 - smoothstep(0.88, 0.95, au)) * 0.55 * fade);
        // Iron mullions from the hand to the arches, a brass highlight on each side.
        const float rib_len = FanInside(r - (k - 0.016), fw) * FanInside(0.89 - r, fw);
        FanOver(acc, FanIron, rib_len * FanLine(side, 0.011, fw) * 0.95);
        FanOver(acc, FanBrass, rib_len * FanLine(abs(side) - 0.0118, 0.00165, fw) * 0.65);
        // The arch: iron with a brass edge; lit, gold; the game's selected item, a little.
        FanOver(acc, FanIron, in_sector * FanLine(r - r_out, 0.006, fw) * 0.9);
        FanOver(acc, mix(FanBrass, FanBrassHi, glow),
                in_sector * FanLine(r - (r_out + 0.0066), 0.00165, fw) * 0.7);
        FanOver(acc, FanBrassHi,
                in_sector * FanLine(r - r_out, 0.0039, fw) * max(0.9 * glow, 0.4 * selected));
    }
    const vec3 rgb = acc.rgb / max(acc.a, 1e-4) * mix(0.7, 1.0, scene_level) * exposure;
    return vec4(rgb, acc.a * dd.color.w);
}

// BEGIN TONITRUS BRIDGE MATERIAL
vec4 BoltShade(vec3 local,float age,float fade) {
    if(local.z<4.5) {
        // A thin white-hot core, an electric-blue glow round it and a soft violet bloom to the
        // strip's edge (drawn nearly additive, main): light, not paint.
        float x=abs(local.x),aa=max(fwidth(x),.006);
        float core=1.0-smoothstep(.02-aa,.06+aa,x);
        float glow=exp(-x*x*14.0);
        float bloom=pow(max(0.0,1.0-x),2.0);
        float fork=local.z<.5?1.0:1.0-smoothstep(.40,1.0,local.y);
        // A fast initial discharge and weaker restrikes; no strobing after the cue ends.
        float pulse=1.0-.23*smoothstep(.012,.040,mod(age,.045));
        vec3 rgb=vec3(2.6,2.8,3.0)*core+vec3(.6,1.2,4.0)*glow*(1.0-core)+
                 vec3(.32,.14,1.0)*bloom*.5;
        float alpha=fade*fork*pulse*clamp(core+glow*.7+bloom*.25,0.0,1.0);
        return vec4(rgb,alpha);
    }
    vec2 p=local.xy;float r=length(p);
    if(local.z<6.5) {
        // The flashes where it leaves the tool and where it lands: a white-hot centre, a blue
        // corona and four streaks, over in about a tenth of a second.
        // A soft falloff to nothing at the edge: no disc (the mock's eye dumps).
        float corona=pow(max(0.0,1.0-r),4.0);
        float centre=1.0-smoothstep(.02,.12,r);
        float rays=pow(max(0.0,1.0-min(abs(p.x),abs(p.y))*16.0),3.0)*pow(max(0.0,1.0-r),2.0);
        float flash=fade*(1.0-smoothstep(.02,.13,age));
        return vec4(mix(vec3(.25,.40,1.7),vec3(2.8,3.0,3.2),centre),
                    flash*max(centre,max(corona*.40,rays*.30)));
    }
    if(local.z<7.5) {
        // ... and the light it spills round where it lands, brief and soft.
        float pool=pow(max(0.0,1.0-r),2.0);
        return vec4(.35,.55,1.6,fade*pool*.42*(1.0-smoothstep(0.0,.10,age)));
    }
    float spark=(1.0-smoothstep(.15,1.0,abs(p.x)))*(1.0-smoothstep(.3,1.0,abs(p.y)));
    return vec4(.9,1.5,3.0,fade*spark*(1.0-smoothstep(.065,.22,age)));
}
// END TONITRUS BRIDGE MATERIAL
// BEGIN AUGUR SPRAY MATERIAL
vec4 AugurSprayShade(vec3 drop_data,float age,float kind,float style) {
    vec2 uv=drop_data.xy;
    if(style>.5) {
        // Beast Roar's burst, gritty and dark (the tester's 2026-11-04 "a cosmic horror game ...
        // gritty dark world", "make the embers deep red"): soot and ash flakes, embers smouldering
        // a deep blood red and dying to near black, a few dim sparks, burning ash flakes. No
        // smoke (kind 4 is never drawn in this style: it hid the claw).
        float life=kind<.5?1.1:kind<1.5?.8:kind<2.5?.35:kind<3.5?.9:.6;
        float r=dot(uv,uv);
        float fade=1.-smoothstep(life*.5,life,age);
        // Ragged flecks, not discs: the edge torn by the angle round each one (its own pattern).
        float ang=r>1e-8?atan(uv.y,uv.x):0.; // (no angle at the very centre)
        float torn=r*(1.+.45*sin(ang*5.+drop_data.z*7.)+.25*sin(ang*11.+drop_data.z*13.));
        // ... faded out before the quad's edge, so no lobe is cut straight by it.
        float disc=(1.-smoothstep(.5,.9,torn))*(1.-smoothstep(.75,1.,r));
        if(kind<.5)return vec4(vec3(.06,.05,.045),.9*disc*fade); // soot and ash
        float cool=clamp(age/life,0.,1.);
        float flick=.75+.25*sin(age*61.+drop_data.z*17.);
        vec3 hot=mix(vec3(.5,.035,.02),vec3(.09,.004,.003),cool)*flick;
        if(kind>2.5) {
            // A burning ash flake: dark, its torn edge smouldering.
            float rim=smoothstep(.35,.75,torn);
            return vec4(mix(vec3(.05,.03,.028),hot,rim),disc*fade*.95);
        }
        float core=1.-smoothstep(0.,.5,torn);
        return vec4(hot*(.6+.4*core),disc*fade*(kind>1.5&&kind<2.5?.8:.9));
    }
    // Fresh arterial red, wet: drops and strands with tails behind their motion, heavy globs
    // with a hard glint, a fine mist a little see-through, and soft clouds of blood mist (the
    // kinds of core/vr/augur.h; their lives as the vertex shader's AugurSprayLife).
    float life=kind<.5?.55:kind<1.5?.95:kind<2.5?.5:kind<3.5?1.15:.45;
    if(kind>3.5) {
        float r=dot(uv,uv);
        return vec4(.16,.008,.012,.38*(1.-smoothstep(0.,1.,r))*(1.-smoothstep(life*.3,life,age)));
    }
    vec2 q=vec2(uv.x/(.70+.27*uv.y),uv.y+.08);
    float r=dot(q,q),edge=max(fwidth(r),.025);
    float alpha=(1.-smoothstep(1.-edge,1.+edge,r))*(1.-smoothstep(life*.6,life,age));
    float body=sqrt(max(0.,1.-r));
    vec2 g=q-vec2(-.26,.30);
    float shine=kind>2.5?exp(-dot(g,g)*30.)*.4:exp(-dot(g,g)*40.)*.2;
    vec3 color=mix(vec3(.06,.004,.007),vec3(.34,.017,.028),body)+vec3(shine);
    return vec4(color,alpha*(kind<.5?.8:.96));
}
// END AUGUR SPRAY MATERIAL
vec4 SkyEye();
void ShadeItem() {
    const int code = int(dd.extra.x + 0.5);
    const int shade = code & 15;
    if (shade == ShadeUnlit && dd.uv.x == -2010.0) {
        out_color = SkyEye();
        out_cover = vec4(0); // celestial background, never part of the DIBR hand mask
        return;
    }
    if(shade==ShadeUnlit && dd.uv.x==-2070.0) {
        vec4 bolt=BoltShade(in_local,dd.uv.y,dd.uv.w);
        if(bolt.a<.03)discard;
        // Light: mostly added to the scene, little of it covered (premultiplied blend).
        out_color=vec4(bolt.rgb*bolt.a,bolt.a*.55);out_cover=vec4(bolt.a);return;
    }
    // BEGIN BLADE SHINE
    // A honed split blade (core/vr/dual_blades.h; bloodborne_quickitems.cpp AddBladeShine): the
    // blade's own steel (its model as the game poses it, in its grip's frame) drawn again over it,
    // light only: a cold sheen on its surface while it is honed, brightest where the steel turns the
    // light to the eye (it slides over the curve as the blade turns), and with the ring a band of
    // light running over it from the guard to the tip. uv = (-2091, the band's place, 0 the guard
    // .. 1 the tip (< 0 none), the sheen's strength, the band's); color.xyz / emissive.xyz = the
    // blade's guard and tip in the model's frame (in_local's); color.w 1: tests
    // (VR_BLADE_SHINE_DEBUG), the whole model magenta. A wet glint on steel, never a glow round it.
    if(shade==ShadeUnlit && dd.uv.x==-2091.0) {
        if(dd.color.w>.5){out_color=vec4(.5,0.,.5,.5);out_cover=vec4(.5);return;}
        const vec3 base=dd.color.xyz,ax=dd.emissive.xyz-base;
        const float len2=max(dot(ax,ax),1e-6);
        const float u=dot(in_local-base,ax)/len2;          // 0 the guard .. 1 the tip
        // Only the steel's span, and only its faces toward the eye (the normals point out,
        // core/vr/blade_shine_mesh.h): the far side, within the depth pull where the steel is thin,
        // would add its light through the edge.
        if(u<-.03||u>1.08)discard;
        const vec3 v=normalize(-in_view);
        vec3 n=normalize(in_normal);
        const float nv=dot(n,v);
        if(nv<-.15)discard;
        if(nv<0.)n=normalize(n+v*(.001-nv));                // a silhouette's last texels: square on
        // The light it catches: from above and a little ahead of the eye (the moon, the lamps
        // overhead), as the held items' key light.
        const vec3 l=normalize(vec3(.30,.85,-.40));
        const float nh=max(dot(n,normalize(l+v)),0.);
        const float spec=pow(nh,48.)*1.1+pow(nh,8.)*.12;
        const float rim=pow(1.-max(dot(n,v),0.),3.)*.22;
        const float steel=smoothstep(-.03,.06,u)*(1.-smoothstep(1.02,1.08,u));
        float light=(spec+rim+.05)*dd.uv.z*steel;
        if(dd.uv.y>-.5) {
            const float d=(u-dd.uv.y)*sqrt(len2);           // metres along from the band
            const float band=exp(-d*d/(.025*.025));
            const float trail=d<0.?exp(d/.08)*.25:0.;
            light+=(band*(.55+spec*2.5+rim)+trail*(.2+spec))*dd.uv.w*steel;
        }
        float scene_luma;
        SceneLight(scene_luma); // (read only for what is drawn)
        light*=mix(.5,1.,clamp(scene_luma*4.,0.,1.))*dd.extra.y;
        if(light<.003)discard;
        const float a=min(light,1.);
        out_color=vec4(vec3(.80,.86,1.)*light,a*.15);out_cover=vec4(a);return;
    }
    // END BLADE SHINE
    const int edge = code & (EdgeDisc | EdgeRing);
    const float exposure = dd.extra.y;
    // Roughly lit like the scene: lit items follow the light around them (a dark room darkens and
    // tints them), cards and text keep a floor so they stay readable in the dark.
    float scene_luma;
    const vec3 scene = SceneLight(scene_luma);
    const float scene_level = clamp(scene_luma * 4.0, 0.0, 1.0);
    if(shade==ShadeUnlit && dd.uv.x==-9000.0) {
        vec2 q=in_local.xy*2.0;
        float edge=1.0-smoothstep(.3,1.0,dot(q,q));
        float curl=.65+.35*sin(q.x*9.0+sin(q.y*7.0+dd.uv.y*2.0)+dd.uv.z);
        float a=dd.color.a*edge*curl;
        if(a<.003)discard;
        out_color=vec4(dd.color.rgb*a*mix(.35,1.,scene_level)*exposure,a);
        out_cover=vec4(0); return;
    }
    if(shade==ShadeUnlit && dd.uv.x==-2000.0) {
        vec4 spray=AugurSprayShade(in_local,dd.uv.y,in_uv.x,in_uv.y);
        if(spray.a<.03)discard;
        // Still red in the dark; the roar's embers and sparks give their own light.
        const bool glows=in_uv.y>.5 && in_uv.x>.5;
        spray.rgb*=(glows?1.:mix(.45,1.,scene_level))*exposure;
        out_color=vec4(spray.rgb*spray.a,spray.a);out_cover=vec4(spray.a);return;
    }
    const vec3 tint = mix(vec3(1.0), clamp(scene / max(scene_luma, 1e-3), 0.6, 1.6), 0.5);
    vec3 rgb;
    float alpha = dd.color.a;
    if (shade == ShadeLit) {
        // One key light from above and behind the viewer (head-relative, as the overlay's rig),
        // a dim ambient and a soft rim: a held object reads in Yharnam's dark streets without
        // glowing.
        vec3 n = normalize(in_normal);
        const vec3 v = normalize(-in_view);
        if (dot(n, v) < 0.0) {
            n = -n; // two-sided (no culling: the view space is left-handed)
        }
        const vec3 l = normalize(vec3(0.35, 0.80, -0.45));
        const float diffuse = max(dot(n, l), 0.0);
        const vec3 h = normalize(l + v);
        const float specular = pow(max(dot(n, h), 0.0), 32.0) * 0.25;
        const float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.15;
        rgb = dd.color.rgb * (0.22 + 0.85 * diffuse) + vec3(specular + rim);
        rgb = rgb * tint * mix(0.22, 1.0, scene_level) * exposure + dd.emissive.rgb;
    } else if (shade == ShadeIcon) {
        const vec4 t = Footprint(in_uv, true);
        alpha *= t.a;
        rgb = t.rgb * dd.color.rgb * mix(0.62, 1.0, scene_level) * exposure + dd.emissive.rgb;
    } else if (shade == ShadeText) {
        // The letters over a soft halo (emissive.rgb at opacity emissive.a): readable on any
        // background without a hard outline or drop shadow.
        const vec2 t = Footprint(in_uv, false).rg;
        const float fill = t.r;
        const float halo = t.g * dd.emissive.a;
        const float cover = fill + halo * (1.0 - fill);
        alpha *= cover;
        rgb = mix(dd.emissive.rgb, dd.color.rgb, cover > 0.0 ? fill / cover : 0.0);
        rgb *= mix(0.75, 1.0, scene_level) * exposure;
    } else if (shade == ShadeModel) {
        const vec4 m = ShadeGameModel(exposure, false);
        rgb = m.rgb;
        alpha = m.a;
    } else if (shade == ShadeCard) {
        const vec4 c = ShadeArcCard(exposure);
        rgb = c.rgb;
        alpha = c.a;
    } else if (shade == ShadeGauge) {
        const vec4 g = ShadeBloodGauge(exposure);
        rgb = g.rgb;
        alpha = g.a;
    } else if (shade == ShadeHoldRing) {
        const vec4 h = ShadeHoldRingFn(exposure, scene_level);
        rgb = h.rgb;
        alpha = h.a;
    } else if (shade == ShadeVialDrain) {
        const vec4 d = ShadeVialDrainFn(exposure);
        rgb = d.rgb;
        alpha = d.a;
    } else if (shade == ShadeApothecary) {
        const vec4 m = ShadeApothecaryFn(exposure);
        rgb = m.rgb;
        alpha = m.a;
    } else if (shade == ShadeGoods) {
        const vec4 m = ShadeGoodsFn(exposure);
        rgb = m.rgb;
        alpha = m.a;
    } else if (shade == ShadeMolotov) {
        const vec4 m = ShadeMolotovFn(exposure);
        rgb = m.rgb;
        alpha = m.a;
    } else if (shade == ShadeNativeLiquid) {
        const vec4 d = ShadeSyringeFn(exposure, int(dd.emissive.z + 0.5));
        rgb = d.rgb;
        alpha = d.a;
    } else if (shade == ShadeSyringe) {
        const vec4 d = ShadeSyringeFn(exposure, 0);
        rgb = d.rgb;
        alpha = d.a;
    } else if (shade == ShadeSteel) {
        const vec4 d = ShadeSteelFn(exposure);
        rgb = d.rgb;
        alpha = d.a;
    } else if (shade == ShadeFan) {
        const vec4 f = ShadeFanFn(exposure, scene_level);
        rgb = f.rgb;
        alpha = f.a;
    } else {
        rgb = dd.color.rgb * exposure + dd.emissive.rgb;
    }
    if (edge != 0 && edge != Candle) {
        alpha *= EdgeCoverage(edge);
    }
    if (alpha < 0.03) {
        discard;
    }
    out_color = vec4(rgb * alpha, alpha);
    out_cover = vec4(alpha);
}

// The backdrop's scene (the probe: half size, ProbeLevels mips) at `uv`, linear.
vec3 BackdropScene(vec2 uv, float lod) {
    vec3 color = textureLod(tex, uv, lod).rgb;
    if (dd.extra.y > 0.5) color = color.bgr;
    if (dd.extra.x < 0.5) color = mix(color / 12.92, pow((color + .055) / 1.055, vec3(2.4)),
                                      greaterThan(color, vec3(.04045)));
    return color;
}

// Tonitrus' strike (s: 0..1, its flicker): lightning's flash over the scene, which keeps its own
// detail underneath (premultiplied: rgb added, a the share of the scene given way, most of its warm
// colour, so the flash reads blue and white, not purple). The world lit a cold blue-white, the dark
// reaching a deep blue (never black), what the strike's own light reached (the bright: near the
// mace) burning toward white, and a glow round every bright thing (the probe's blurred mips: wider
// at each); exposed (1 - e^-1.5x), so the brightest ease into white instead of clipping flat.
vec4 StormFlash(vec3 color, vec2 uv, float s) {
    const vec3 luma = vec3(.2126,.7152,.0722);
    const float l = dot(color, luma);
    const vec3 cold = mix(color, vec3(l), .8) * vec3(.62, .86, 1.7);
    const vec3 glow = max(BackdropScene(uv, 2.0) - .22, 0.0) * .8 +
                      max(BackdropScene(uv, 3.0) - .16, 0.0) * 1.1 +
                      max(BackdropScene(uv, 4.0) - .10, 0.0) * 1.5;
    const float g = dot(glow, luma);
    const vec3 light = cold * 1.6 + vec3(.16, .26, .80) * .16 +
                       vec3(.6 * smoothstep(.22, .75, l)) +
                       mix(vec3(.55, .70, 1.0), vec3(1.0), clamp(g, 0.0, 1.0)) * (1.2 * g);
    const vec3 flash = 1.0 - exp(-1.5 * light);
    // The scene at strength s: mix(scene, flash, s) = scene * (1 - a) + this, a = .6 s.
    return vec4(max(s * (flash - .4 * color), 0.0), .6 * s);
}

// A Call Beyond: an almond-shaped lid, fibrous iris and a deep slit pupil. Four
// small distant-sky quads share this analytic material; no textures or scene samples.
vec4 SkyEye() {
    vec2 p = in_local.xy * 2.0;
    float lid = .72 * max(0.0, 1.0 - p.x*p.x) * dd.uv.y;
    float edge = lid - abs(p.y);
    float aa = max(fwidth(edge), .008);
    float mask = smoothstep(-aa, aa, edge) * (1.0-smoothstep(.86,1.0,abs(p.x)));
    if (mask <= 0.0 || dd.uv.y <= 0.0) return vec4(0);
    vec2 iris = vec2(p.x * 2.46, p.y);
    vec3 colour = dd.color.rgb * (.40 + .35*max(edge,0.0));
    // The iris occupies only the middle of the eye. Outside it its original blend
    // weight is exactly zero, so skip all fibre trigonometry, pupil and glint work.
    float radius2 = dot(iris, iris);
    if (radius2 < .56*.56) {
        float radius = sqrt(radius2);
        float angle = atan(iris.y, iris.x + 1e-6);
        float fibres = .5 + .25*sin(angle*43.0 + radius*23.0 + dd.uv.z*2.0)
                              + .25*sin(angle*71.0 - radius*17.0);
        float irisMask = 1.0-smoothstep(.50,.56,radius);
        float pupil = 1.0-smoothstep(.075,.11,length(vec2(iris.x*1.6,iris.y*.44)));
        colour = mix(colour, mix(vec3(.035,.023,.055),vec3(.20,.17,.29),fibres),irisMask);
        colour = mix(colour,vec3(.002,.003,.006),pupil*irisMask);
        float glint = 1.0-smoothstep(.015,.045,length(iris-vec2(-.12,.18)));
        colour += glint * vec3(.20,.24,.28) * irisMask;
    }
    float rim = (1.0-smoothstep(.015,.06,abs(edge))) * .22;
    colour += rim * vec3(.22,.17,.29);
    // Feathered low opacity; the sky remains visible through the apparition.
    float alpha = dd.color.a * mask * smoothstep(.0,.09,dd.uv.y);
    return vec4(colour*alpha,alpha);
}

void main() {
    const vec3 luma = vec3(.2126,.7152,.0722);
    if (in_gray == 256) {
        const vec2 uv = gl_FragCoord.xy * dd.extra.zw;
        const vec3 color = BackdropScene(uv, 0.0);
        // A Call Beyond's gathered world, pale and luminous (dd.color.r: call_beyond_bright,
        // rising to call_beyond_flash once raised): the gray lifted up to (1 + b) times, eased
        // toward its ceiling (white stays white, never past it), a little cold. Raised
        // (dd.color.g, the flare 0..1), it turns the stars' icy blue and even the dark glows.
        const float b = clamp(dd.color.r, 0.0, 8.0);
        const float s = clamp(dd.color.g, 0.0, 1.0);
        const float g = dot(color,luma);
        const vec3 tint = mix(mix(vec3(1.0), vec3(.94,.98,1.06), min(b, 1.0)),
                              vec3(.80,.90,1.25), s);
        const vec3 lit = min(vec3(g * (1.0 + b) / (1.0 + b * g) + .06 * s) * tint, vec3(1.0));
        out_color = vec4(lit * dd.color.a, dd.color.a); // 0: no gathering (a strike alone)
        // Tonitrus' strike (dd.color.b, 0..1) over it.
        const float storm = clamp(dd.color.b, 0.0, 1.0);
        if (storm > 0.0) {
            const vec4 flash = StormFlash(color, uv, storm);
            out_color = vec4(out_color.rgb * (1.0 - flash.a) + flash.rgb,
                             out_color.a + flash.a - out_color.a * flash.a);
        }
        out_cover = vec4(0);
        return;
    }
    ShadeItem();
    if (in_gray > 0)
        out_color.rgb = mix(out_color.rgb,vec3(dot(out_color.rgb,luma)),float(in_gray)/255.0);
}
