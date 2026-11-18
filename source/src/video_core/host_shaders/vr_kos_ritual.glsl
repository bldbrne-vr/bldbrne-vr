// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The Kos ritual's peripheral vision (kos_visions): bruised, wet tissue and a few partly
// concealed eyes. One small glance, then stillness. The forward view stays untouched;
// no kaleidoscope, luminous pupils, spinning pattern or full-screen flash.
// Shared by native stereo and the final DIBR pass, after upscaling.
//
// The view's frame `c`: x right, y up, KosRitualScale times the tangent of the angle off the eye's
// line of sight (the same for every headset's field). The including shader defines, before
// including this:
//   vec3 KosRitualFetch(vec2 c): the view's linear RGB (RGB order) at `c`, clamped to the view.

const float KosRitualScale = 0.55;
vec3 KosRitualFetch(vec2 c); // (the including shader's)
#ifdef VR_KOS_IRIS
// Conservative view-space width of an output pixel: explicit mip selection also works in
// compute and non-uniform eye branches (no implicit fragment derivatives).
float KosRitualPixelSpan();
#endif

float KosHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float KosNoise(vec2 p) {
    const vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(KosHash(i), KosHash(i + vec2(1.0, 0.0)), f.x),
               mix(KosHash(i + vec2(0.0, 1.0)), KosHash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float KosFbm(vec2 p) {
    float s = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        s += a * KosNoise(p);
        p = p * 2.03 + 17.1;
        a *= 0.5;
    }
    return s;
}
float KosBand(float x, float w) {
    return 1.0 - smoothstep(0.0, w, abs(x));
}
// A heartbeat (lub-dub, 1.15 a second), 0 .. 1.
float KosBeat(float t) {
    const float ph = fract(t * 1.15);
    const float d1 = (ph - 0.05) * 25.0;
    const float d2 = (ph - 0.22) * 25.0;
    return exp(-d1 * d1) + 0.6 * exp(-d2 * d2);
}

// Wet flesh: warped, veined at two scales, its relief lit from above with wet glints, pulsing.
vec3 KosFlesh(vec2 p, float seed, float t) {
    const vec2 w = p + 0.6 * vec2(KosFbm(p * 2.0 + seed), KosFbm(p * 2.0 + seed + 5.2));
    const float f = KosFbm(w * 3.0);
    const float g = KosFbm(w * 7.0 + 3.0);
    vec3 c = mix(vec3(0.035, 0.028, 0.033), vec3(0.18, 0.115, 0.125), f);
    c = mix(c, vec3(0.26, 0.22, 0.20), smoothstep(0.55, 0.85, g) * 0.35);
    const float v1 = KosBand(KosFbm(w * 2.4 + 11.0) - 0.5, 0.025);
    const float v2 = KosBand(KosFbm(w * 6.0 + 23.0) - 0.5, 0.018) * 0.6;
    c = mix(c, vec3(0.095, 0.025, 0.043), clamp(v1 + v2, 0.0, 1.0) * 0.8);
    // (The relief from one-sided differences: three of the fbm, not four.)
    const float h0 = KosFbm(w * 4.0);
    const float hx = KosFbm(w * 4.0 + vec2(0.03, 0.0)) - h0;
    const float hy = KosFbm(w * 4.0 + vec2(0.0, 0.03)) - h0;
    const float light = clamp(0.5 + (-hx * 0.45 + hy * 0.65) * 18.0, 0.0, 1.0);
    c *= 0.6 + 0.65 * light;
    c += vec3(0.30, 0.22, 0.2) * pow(light, 6.0) * 0.18;
    c *= 0.97 + 0.03 * sin(t * 1.4 + seed * 6.0);
    return c;
}

// Reptilian eyes under asymmetric, heavy lids: a distinct mottled iris sits inside
// discoloured sclera. The black slit absorbs light; only the cornea reflects it.
vec4 KosEye(vec2 p, float open, vec2 look, float seed, float dil, float t, float eye_size) {
    const float arch = max(0.0, 1.0 - p.x * p.x);
    const float fold = 1.0 + 0.13 * (KosNoise(vec2(p.x * 3.0 + seed, seed)) - 0.5);
    const float tilt = p.x * 0.055;
    const float upper = open * 0.53 * pow(arch, 0.56) * fold * (0.88 + 0.16 * p.x) + tilt;
    const float lower = open * 0.32 * pow(arch, 1.25) * fold - tilt;
    const float edge = max(p.y - upper, -p.y - lower);
    // This shader is shared with a compute pass: analytic feather, no fragment derivatives.
    const float inside = (1.0 - smoothstep(-0.018, 0.018, edge)) *
                         (1.0 - smoothstep(0.97, 1.02, abs(p.x))) * smoothstep(0.01, 0.08, open);
    const float around = (1.0 - smoothstep(0.12, 0.38, edge)) *
                         (1.0 - smoothstep(0.85, 1.25, abs(p.x)));
    const vec2 ip = (p - look * 0.26) / 0.51;
    const float d = length(ip);
#ifdef VR_KOS_IRIS
    const float grain = KosNoise(p * 7.0 + seed);
    const float vessels = KosBand(KosFbm(p * vec2(6.0, 10.0) + seed) - 0.5, 0.025);
    vec3 col = vec3(0.24, 0.225, 0.19) * (0.80 + 0.30 * grain);
    col = mix(col, vec3(0.085, 0.035, 0.045), vessels * 0.40);
    col *= 1.0 - 0.55 * smoothstep(0.65, 1.0, abs(p.x));
    if (inside > 0.001 && d < 1.02) {
        const float footprint = 128.0 * KosRitualPixelSpan() / max(eye_size * 1.02, 0.001);
        const float lod = max(0.0, log2(max(footprint, 1.0)));
        // The immutable sRGB view decodes before hardware filtering. Its black slit is part
        // of the approved texture; do not overlay a second pupil or decode it twice.
        const vec3 iris = textureLod(kos_iris, clamp(vec2(ip.x, -ip.y) * 0.5 + 0.5, 0.0, 1.0), lod).rgb;
        col = mix(col, iris, 1.0 - smoothstep(0.94, 1.02, d));
    }
#else
    const float ang = atan(ip.y, ip.x + 0.000001);
    // Fibres follow the iris radius; periodic angular detail has no atan seam.
    const vec2 direction = vec2(cos(ang), sin(ang));
    const float grain = KosFbm(direction * 5.5 + vec2(d * 2.0, seed));
    const float fibres = 0.5 + 0.5 * sin(ang * 57.0 + grain * 6.0 + d * 10.0);
    vec3 iris = mix(vec3(0.085, 0.075, 0.055), vec3(0.29, 0.265, 0.18), grain);
    iris *= 0.58 + 0.42 * fibres;
    iris *= 1.0 - 0.50 * smoothstep(0.65, 1.0, d); // dark limbus, no luminous rim
    const float crypt = KosBand(KosNoise(direction * 12.0 + d * 3.0 + seed) - 0.5, 0.055);
    iris *= 1.0 - 0.40 * crypt;
    const float vessels = KosBand(KosFbm(p * vec2(6.0, 10.0) + seed) - 0.5, 0.025);
    vec3 sclera = vec3(0.24, 0.225, 0.19) * (0.80 + 0.30 * grain);
    sclera = mix(sclera, vec3(0.085, 0.035, 0.045), vessels * 0.40);
    sclera *= 1.0 - 0.55 * smoothstep(0.65, 1.0, abs(p.x));
    vec3 col = mix(sclera, iris, 1.0 - smoothstep(0.94, 1.02, d));
    // A tall pointed slit; tiny irregularities keep the pupil from looking stamped on.
    const float width = mix(0.055, 0.15, dil) * max(0.0, 1.0 - pow(abs(ip.y) / 0.86, 2.0));
    const float pupilEdge = abs(ip.x + 0.008 * sin(ip.y * 13.0 + seed)) - width;
    const float pupil = (1.0 - smoothstep(-0.008, 0.012, pupilEdge)) *
                        (1.0 - smoothstep(0.80, 0.88, abs(ip.y)));
    col = mix(col, vec3(0.0015, 0.001, 0.002), pupil);
#endif
    // The upper lid casts a deep soft shadow over the cornea. No white cartoon outline.
    col *= 0.22 + 0.78 * smoothstep(0.0, 0.25, upper - p.y);
    col *= 0.45 + 0.55 * smoothstep(0.0, 0.12, -edge);
    const vec2 glint = (p - vec2(-0.22, 0.08)) / vec2(0.075, 0.035);
    col += vec3(0.20, 0.22, 0.21) * exp(-dot(glint, glint)) *
           smoothstep(0.015, 0.10, upper - p.y);
    vec3 lidc = KosFlesh(p * 0.9, seed, t) * 0.50;
    lidc *= 0.32 + 0.68 * smoothstep(0.0, 0.16, edge);
    const float crease = p.y > 0.0 ? p.y - upper - 0.16 * sqrt(arch) : -p.y - lower - 0.10;
    lidc *= 1.0 - 0.50 * KosBand(crease, 0.045); // uneven recessed lid crease
    // Only a broken damp trace on the lower lid, never a complete bright rim.
    lidc += vec3(0.055, 0.048, 0.045) * KosBand(edge - 0.035, 0.020) *
            step(p.y, 0.0) * smoothstep(0.4, 0.7, grain);
    return vec4(mix(lidc, col, inside), max(inside, around));
}

// An eye half buried in tissue. A single slow glance resolves into a stare; no framewise jitter.
vec4 KosEyeAt(vec2 c, vec2 centre, float size, float rot, float open, float seed, float dil,
              float t, float stare) {
    const vec2 dd = c - centre;
    if (dot(dd, dd) > size * size * 2.25) {
        return vec4(0.0);
    }
    const vec2 ep =
        vec2(dd.x * cos(rot) + dd.y * sin(rot), -dd.x * sin(rot) + dd.y * cos(rot)) / size;
    const vec2 away = (vec2(KosHash(vec2(seed, 2.0)), KosHash(vec2(3.0, seed))) - 0.5) * 0.55;
    const vec2 look = mix(away, vec2(0.0), smoothstep(0.0, 1.0, stare));
    return KosEye(ep, open, look, seed, dil, t, size);
}

// The ritual over `col` (linear, RGB order) at `c` (the view's frame): r.x its rise (0 .. 1, the
// arms rising and the coil), r.y its burst (1 at the burst, easing to 0), r.z its clock (s),
// r.w its seed (a new flesh each ritual).
vec3 KosRitual(vec3 col, vec2 c, vec4 r_in) {
    const float rit = clamp(r_in.x, 0.0, 1.0);
    const float burst = clamp(r_in.y, 0.0, 1.0);
    const float k = max(rit, burst);
    if (k <= 0.001) {
        return col;
    }
    const float t = r_in.z;
    const float r = length(c);
    const float a = atan(c.y, c.x);
    // Tiny local refraction, fading to zero before the clear centre. No rotation or tiling
    // of the scene: the apparent movement is bounded to 0.003 view units at peak strength.
    const float peripheral = smoothstep(0.30, 0.62, r);
    const float kw = k * peripheral;
    if (kw > 0.001) {
        const vec2 ripple = vec2(sin(c.y * 11.0 + t * 0.8), sin(c.x * 9.0 - t * 0.7));
        const vec3 refracted = KosRitualFetch(c + ripple * (0.003 * kw));
        col = mix(col, refracted, 0.35 * kw);
    }
    // Bruised, cold edges; preserve exposure and colour in the forward view.
    const float lumk = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(col, vec3(lumk) * vec3(0.83, 0.85, 0.92), 0.24 * kw) * (1.0 - 0.18 * kw);
    const float fr = 0.70 - 0.14 * k + 0.025 * sin(a * 3.0 + r_in.w * 6.0) +
                     0.005 * sin(t * 1.6);
    const float fm = smoothstep(fr - 0.065, fr + 0.045, r) * k;
    if (fm > 0.001) {
        col = mix(col, KosFlesh(c * 3.0, 1.7 + r_in.w, t) *
                  (0.48 + 0.035 * rit * KosBeat(t)), fm * 0.72);
    }
    // Nine irregular sites, partly closed and embedded in the peripheral tissue.
    // Only nearby eyes evaluate the expensive flesh/iris detail (KosEyeAt's distance cull).
    if (r > 0.38 && fm > 0.001) {
        const float stare = smoothstep(0.45, 0.93, rit) + burst;
        const float dil = mix(0.24, 0.15, clamp(stare, 0.0, 1.0));
        for (int i = 0; i < 9; i++) {
            const float id = float(i);
            const float h = KosHash(vec2(id + r_in.w * 13.0, 7.3));
            const float ac = (id + 0.5) * (6.28318 / 9.0) + (h - 0.5) * 0.5 + 0.3;
            const float rr = 0.53 + 0.12 * KosHash(vec2(id, 2.0));
            const vec2 ec = vec2(cos(ac), sin(ac)) * rr;
            const float size = 0.055 + 0.025 * KosHash(vec2(id, 4.0));
            const float rot = ac + 1.5708 + (h - 0.5) * 0.8;
            const float delay = 0.18 + h * 0.3;
            float open = smoothstep(delay, delay + 0.45, rit) * (0.62 + h * 0.22);
            // Shut at the release, then let the tissue recede with the existing settle clock.
            if (burst > 0.001) {
                open = smoothstep(0.88, 1.0, burst) * (0.62 + h * 0.22);
            }
            const vec4 e = KosEyeAt(c, ec, size, rot, open, h * 10.0 + id,
                                    dil, t, clamp(stare, 0.0, 1.0));
            col = mix(col, e.rgb, e.a * fm * 0.90);
        }
    }
    // A soft peripheral release, never a bright ring crossing the centre.
    const float release = burst * burst * peripheral;
    col = mix(col, col * vec3(1.03, 1.04, 1.05) + vec3(0.008, 0.009, 0.012), release * 0.30);
    return col;
}
