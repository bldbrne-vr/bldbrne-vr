// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

// In-frame items (video_core/renderer_vulkan/vr_inframe.cpp). The target is an sRGB view of the
// game's scene colour, so this writes linear light (premultiplied alpha, ONE / ONE_MINUS_SRC_ALPHA)
// and the hardware encodes it like the scene. Texels with almost no coverage are discarded, so
// they write no depth either (DIBR then shows what is behind them at its own depth). The second
// output is the coverage (blended MAX): the DIBR warp keeps the HUD's edges from cutting our items.
// The pass is single-sampled after the game's AA: the cards' round edges are anti-aliased here
// (analytic, from the mesh-space radius), and the icons and labels, which have no mips, are
// box-filtered over each pixel's footprint.

layout(location = 0) in vec3 in_view;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec3 in_local;
layout(location = 4) flat in vec2 in_anchor; // Shade::Model: the holding hand (frame uv), -1 none

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_cover;

layout(set = 0, binding = 0) uniform sampler2D tex;
// The scene's light: the scene colour box-filtered into a mip chain (VrInFrame::ProbeScene); its
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
// Silhouettes in the mesh's xy plane (the card faces the eye): a disc of radius 0.5 (the plate),
// a ring between radii 0.47 and 0.53 (the rim).
const int EdgeDisc = 16;
const int EdgeRing = 32;

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
    vec4 t = texture(tex, in_uv);
    float alpha = (liquid_data ? 1.0 : dd.color.a) * smoothstep(0.2, 0.55, t.a);
    vec3 n = normalize(in_normal);
    const vec3 v = normalize(-in_view);
    if (dot(n, v) < 0.0) {
        n = -n; // two-sided (no culling: the view space is left-handed)
    }
    const vec3 albedo = t.rgb * (liquid_data ? vec3(1.0) : dd.color.rgb);
    const vec3 tint =
        mix(vec3(1.0), clamp(tint_src / max(tint_luma, 1e-3), 0.6, 1.6), 0.4);
    const vec3 l = normalize(vec3(-0.30, 0.80, -0.45));
    const float diffuse = max(dot(n, l), 0.0);
    const vec3 h = normalize(l + v);
    const float specular = pow(max(dot(n, h), 0.0), 28.0) * 0.18;
    const float facing = max(dot(n, v), 0.0);
    vec3 rgb = (albedo * (0.55 + 0.75 * diffuse) + vec3(specular)) * tint * e * exposure;
    // The game grades its frame toward grey-blue; the item's raw texture is a little richer than
    // what it shows: 18% of its colour goes, so it sits in the scene instead of popping out of it.
    rgb = mix(vec3(dot(rgb, vec3(0.2126, 0.7152, 0.0722))), rgb, 0.82);
    // The glow (warm, soft): the item brightens a little and a halo of warm light rises at its
    // silhouette, its own texture always readable through it.
    const float rim = pow(1.0 - facing, 2.0);
    const float glow = liquid_data ? 0.0 : max(dd.emissive.r, max(dd.emissive.g, dd.emissive.b));
    rgb = rgb * (1.0 + 0.8 * glow) + (liquid_data ? vec3(0.0) : dd.emissive.rgb) * (0.10 + 0.90 * rim);
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
vec4 ShadeGoodsFn(float exposure) {
    vec3 tint; const float e=HeldLight(tint)*exposure;
    vec3 n=normalize(in_normal), v=normalize(-in_view);
    if(dot(n,v)<0.0) n=-n;
    const vec3 l=normalize(vec3(-.30,.80,-.45));
    const float nh=max(dot(n,normalize(l+v)),0.0);
    const vec3 p=in_local;
    const int id=int(in_uv.x+.5);
    const float cav=clamp(in_uv.y,0.0,1.0);
    // Sub-millimetre detail fades with the pixel's footprint: no shimmer in the headset.
    const float detail=1.0-smoothstep(.0004,.002,length(fwidth(p)));
    const float fine=mix(.5,1.0,detail);
    vec3 albedo; vec3 glow=vec3(0.0);
    float rough=.9, metal=0.0, G=1.3, K=.7;
    vec3 grime=vec3(.020,.014,.008);
    if(id==6) {
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
    rgb+=dd.emissive.rgb*.10;
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
    const vec3 blood = liquid_tint*diffuse*e*tint;
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
    const float film = potion ? 0.0 : dd.emissive.z*(0.07+0.19*pow(age,3.0))*(1.0-exp(-max(-below,0.0)*110.0));
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

void main() {
    const int code = int(dd.extra.x + 0.5);
    const int shade = code & 15;
    const int edge = code & (EdgeDisc | EdgeRing);
    const float exposure = dd.extra.y;
    // Roughly lit like the scene: lit items follow the light around them (a dark room darkens and
    // tints them), cards and text keep a floor so they stay readable in the dark.
    float scene_luma;
    const vec3 scene = SceneLight(scene_luma);
    const float scene_level = clamp(scene_luma * 4.0, 0.0, 1.0);
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
    } else {
        rgb = dd.color.rgb * exposure + dd.emissive.rgb;
    }
    if (edge != 0) {
        alpha *= EdgeCoverage(edge);
    }
    if (alpha < 0.03) {
        discard;
    }
    out_color = vec4(rgb * alpha, alpha);
    out_cover = vec4(alpha);
}
