// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The status build-ups at the corners of what the headset shows (core/vr/status_corners.h
// StatusCorner mirrors this line by line; included by vr_overlay.frag and vr_eye_upscale.comp).
// Clotted olive vessels, bruised capillaries, blood smears, tarnished gold fractures and
// claw wounds. StatusCorner returns linear colour and opacity; it exits early past the reach.

// Multiplies only (Hoskins' hash11).
float StatusVeinHash(float x) {
    float p = fract(x * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// One heartbeat's swell at `x` beats after it (0..1): a quick rise, a slower fall.
float StatusVeinBeat(float x) {
    return smoothstep(0.0, 0.03, x) * (1.0 - smoothstep(0.03, 0.22, x));
}

// Smooth value noise (four hashes).
float StatusNoise(vec2 p) {
    const vec2 i = floor(p);
    const vec2 f = p - i;
    const vec2 u = f * f * (3.0 - 2.0 * f);
    const float a = StatusVeinHash(i.x * 127.1 + i.y * 311.7);
    const float b = StatusVeinHash((i.x + 1.0) * 127.1 + i.y * 311.7);
    const float c = StatusVeinHash(i.x * 127.1 + (i.y + 1.0) * 311.7);
    const float d = StatusVeinHash((i.x + 1.0) * 127.1 + (i.y + 1.0) * 311.7);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// The grime that breaks every edge (two scales of the noise), 0..1.
float StatusGrime(vec2 c, float seed) {
    return 0.65 * StatusNoise(c * 38.0 + vec2(seed * 3.1, 0.0)) +
           0.35 * StatusNoise(c * 110.0 + vec2(7.0, seed));
}

// A stroke of half-width w at distance d: x its opacity (a faint halo round it),
// y its shade (dark at its core).
vec2 StatusLine(float d, float w, float halo) {
    w = max(w, 0.0025);
    const float core = 1.0 - smoothstep(0.25 * w, w, d);
    return vec2(max(core, halo * (1.0 - smoothstep(w, 3.0 * w, d))),
                0.95 - 0.67 * (1.0 - smoothstep(0.0, 0.5 * w, d)));
}

// `col` at opacity `a` over `under` (straight alpha).
vec4 StatusOver(vec4 under, vec3 col, float a) {
    a = clamp(a, 0.0, 1.0);
    const float out_a = a + under.a * (1.0 - a);
    return vec4((col * a + under.rgb * under.a * (1.0 - a)) / max(out_a, 1e-6), out_a);
}

// Slow poison's vessels: from just beyond the corner, forking and thinning, swaying, a heartbeat
// running out along them, a faint bruise at the corner. x: opacity, y: shade.
vec2 StatusVeins(vec2 q, float reach, float beats, float width) {
    const float Pi = 3.14159265;
    const float Origin = 1.12;
    const float K = 4.0;
    const vec2 c = vec2(Origin) - min(abs(q), vec2(Origin));
    const float r = length(c);
    const float total = reach + (Origin - 1.0) * 1.41;
    if (!(r < total) || !(width > 0.0)) {
        return vec2(0.0, 1.0);
    }
    const float seed = (q.x > 0.0 ? 1.0 : 0.0) + (q.y > 0.0 ? 2.0 : 0.0);
    const float sway = sin(6.2831853 * fract(beats * (1.0 / 6.0)) + seed * 1.7 + r * 2.0);
    float u = atan(c.y, max(c.x, 1e-6)) * (2.0 / Pi);
    u += 1.3 * (0.10 * r * sin(seed * 1.9 + u * 3.0) + 0.028 * sin(r * 8.0 + u * 7.0 + seed * 2.1) +
                0.009 * sin(r * 31.0 + u * 5.0 + seed * 5.3)) +
         0.03 * r * r * sway;
    const float v = u * K;
    const float cell = floor(v);
    const float f = v - cell - 0.5;
    const float h = StatusVeinHash(cell + 7.0 * seed);
    const float h2 = StatusVeinHash(cell + 7.0 * seed + 3.7);
    const float arc = r * (Pi * 0.5) / K;
    const float t = r / total;
    const float sd = h > 0.5 ? 1.0 : -1.0;
    const float jit = (h2 - 0.5) * 0.16;
    const float r1 = total * (0.22 + 0.18 * h);
    const float s1 = smoothstep(r1, r1 + 0.12 * total, r);
    const float r2 = total * (0.52 + 0.16 * h2);
    const float s2 = smoothstep(r2, r2 + 0.10 * total, r);
    const float side_end = min(total, r1 + total * (0.40 + 0.25 * h2));
    const float twig_end = min(total, r2 + total * (0.22 + 0.15 * h));
    const float b_main = jit - sd * (0.04 + 0.08 * h2) * s1;
    const vec3 at = vec3(b_main, jit + sd * (0.17 + 0.08 * h) * s1,
                         b_main + sd * (0.09 + 0.04 * h) * s2);
    const vec3 wide = vec3(width * pow(max(1.0 - t, 0.0), 1.1),
                           width * 0.62 * (1.0 - pow(smoothstep(r1, side_end, r), 0.8)),
                           width * 0.45 * (1.0 - pow(smoothstep(r2, twig_end, r), 0.8)));
    const float x = fract(beats - r * 1.4);
    const float pulse = max(StatusVeinBeat(x), 0.6 * StatusVeinBeat(x - 0.3));
    const float MinWidth = 0.0025; // about two pixels: thinner fades instead of shimmering
    vec2 vein = vec2(0.0, 1.0);
    for (int i = 0; i < 3; ++i) {
        const float thin = wide[i] * (0.92 + 0.3 * pulse);
        if (!(thin > 1e-4)) {
            continue;
        }
        const float w = max(thin, MinWidth);
        const float d = abs(f - at[i]) * arc;
        const float a = min(thin / MinWidth, 1.0) *
                        max(1.0 - smoothstep(0.25 * w, w, d), 0.16 * (1.0 - smoothstep(w, 3.0 * w, d)));
        if (a > vein.x) {
            vein = vec2(a, 0.95 - 0.67 * (1.0 - smoothstep(0.0, 0.5 * w, d)));
        }
    }
    vein.x *= (1.0 - smoothstep(0.8 * total, total, r)) * (0.72 + 0.28 * pulse);
    vein.y *= 1.0 + 0.5 * pulse;
    const vec2 vis = vec2(1.0) - min(abs(q), vec2(1.0));
    const float b = 1.0 - smoothstep(0.0, 0.42 * max(reach, 0.05), length(vis));
    if (0.30 * b * b > vein.x) {
        vein = vec2(0.30 * b * b, 0.25);
    }
    return vein;
}

// Slow poison: clotted olive vessels with patchy residue along the rims.
vec4 StatusPoison(vec2 q, float reach, float beats, float width, float g) {
    const vec2 v = StatusVeins(q, reach, beats, width);
    const float core = clamp((0.95 - v.y) / 0.67, 0.0, 1.0);
    const float residue = (1.0 - core) * smoothstep(0.28, 0.72, g);
    const vec3 rgb = mix(vec3(0.043, 0.055, 0.025), vec3(0.21, 0.29, 0.09), residue);
    return vec4(rgb, v.x * (0.70 + 0.30 * g));
}

// Rapid poison: fine bruised capillaries, with broken branches over a dark stain.
vec4 StatusWeb(vec2 q, float reach, float beats, float g) {
    const float Pi = 3.14159265;
    const float Origin = 1.12;
    const vec2 oc = vec2(Origin) - min(abs(q), vec2(Origin));
    const float r = length(oc);
    const float total = reach + 0.17;
    if (!(r < total)) {
        return vec4(0.0);
    }
    const float seed = (q.x > 0.0 ? 1.0 : 0.0) + (q.y > 0.0 ? 2.0 : 0.0) + 4.0;
    const float u = atan(oc.y, max(oc.x, 1e-6)) * (2.0 / Pi);
    const float K = 9.0;
    const float v = (u + 0.05 * sin(r * 9.0 + seed * 2.3) + 0.012 * sin(r * 37.0 + u * 9.0 + seed)) * K;
    const float cell = floor(v);
    const float f = v - cell - 0.5;
    const float arc = r * (Pi * 0.5) / K;
    const float t = r / total;
    const float dt = abs(f - (StatusVeinHash(cell + 7.0 * seed) - 0.5) * 0.3) * arc;
    vec2 web = StatusLine(dt, 0.0055 * (1.0 - t), 0.16);
    const float rr = sqrt(max(r + 0.048 * sin(u * 14.0 + seed * 3.1) + 0.016 * sin(u * 43.0 + seed), 0.0));
    const float ring = floor(rr / 0.06);
    const float gd = (fract(rr / 0.06) - 0.5) * 0.06 * 2.0 * rr; // d r = 2 sqrt(r) d sqrt(r)
    const float pulse = 0.85 + 0.15 * sin(6.2831853 * (beats - r * 1.2));
    if (StatusVeinHash(ring * 13.1 + cell * 3.7 + seed) > 0.35) {
        const vec2 ring_line = StatusLine(abs(gd), 0.004 * (1.0 - t), 0.16);
        if (ring_line.x > web.x) web = ring_line;
    }
    const float a = clamp(web.x * (1.0 - smoothstep(0.75 * total, total, r)) * pulse * 1.15, 0.0, 1.0);
    const float core = clamp((0.95 - web.y) / 0.67, 0.0, 1.0);
    const vec3 rgb = mix(vec3(0.23, 0.14, 0.31), vec3(0.055, 0.030, 0.081), pow(core, 0.7));
    const float stain = 0.35 * (1.0 - smoothstep(0.17, reach * 0.8 + 0.17, r)) * g;
    return StatusOver(vec4(0.036, 0.018, 0.048, stain), rgb, a * (0.6 + 0.4 * g));
}

// Bleed: uneven smears at the top and clotted blood at the bottom.
vec4 StatusBleed(vec2 q, vec2 c, float reach, float beats, float g) {
    const bool top = q.y > 0.0;
    const float seed = (q.x > 0.0 ? 1.0 : 0.0) + (top ? 2.0 : 0.0) + 8.0;
    vec2 blood = vec2(0.0, 1.0);
    if (top) {
        const float lane_w = 0.032;
        const float lane = floor(c.x / lane_w);
        const float h = StatusVeinHash(lane + seed * 7.0);
        const float h2 = StatusVeinHash(lane + seed * 7.0 + 5.3);
        const float by_corner = pow(clamp(1.0 - c.x / (reach * 1.15), 0.0, 1.0), 0.7);
        if (h > 0.18 && by_corner > 0.0) {
            const float len = (0.03 + reach * 0.97 * h * h + 0.018 * StatusVeinBeat(fract(beats - h))) * by_corner;
            const float xc = (lane + 0.5 + (h2 - 0.5) * 0.45) * lane_w + 0.003 * sin(c.y * 41.0 + h * 11.0);
            const float taper = clamp(c.y / max(len, 1e-3), 0.0, 1.0);
            const float w = (0.005 + 0.007 * h2) * (1.0 - 0.45 * taper) +
                            0.006 * (1.0 - smoothstep(0.0, 0.03, c.y));
            const float dx = abs(c.x - xc);
            blood = StatusLine(dx, w * (0.60 + 0.65 * g), 0.12);
            blood.x *= 1.0 - smoothstep(0.65 * len, len, c.y);
        }
    } else {
        float level = reach * 0.34 * pow(clamp(1.0 - c.x / (reach * 0.9), 0.0, 1.0), 1.8);
        level *= 1.0 + 0.06 * sin(c.x * 23.0 + beats * 1.4 + seed) + 0.025 * sin(c.x * 61.0 - beats * 2.2);
        level *= 0.78 + 0.44 * g;
        if (level > 0.002) {
            const float under = level - c.y;
            const float meniscus = 1.0 - smoothstep(0.0, 0.005, abs(under));
            blood = vec2(max(smoothstep(-0.004, 0.006, under) * 0.6, meniscus * (0.42 + 0.33 * g)),
                         meniscus > 0.5 ? 1.5 : 0.45 - 0.2 * smoothstep(0.0, 0.08, under));
        }
    }
    const float core = clamp((0.95 - blood.y) / 0.67, 0.0, 1.0);
    vec3 rgb = mix(vec3(0.23, 0.037, 0.030), vec3(0.055, 0.007, 0.010), core * 0.8);
    rgb = mix(rgb, vec3(0.34, 0.078, 0.059), clamp(blood.y - 1.2, 0.0, 1.0) * 0.7); // wet glint
    vec4 o = vec4(rgb, blood.x * (0.75 + 0.25 * g));
    if (top) {
        const float cs = 0.03;
        const vec2 cell = floor(c / cs);
        const float h = StatusVeinHash(cell.x * 7.3 + cell.y * 13.7 + 41.0);
        const float h2 = StatusVeinHash(cell.x * 3.1 + cell.y * 5.9 + 43.0);
        const vec2 ctr = (cell + vec2(0.2 + 0.6 * h, 0.2 + 0.6 * h2)) * cs;
        const float d = length(vec2(c.x - ctr.x, (c.y - ctr.y) * (1.0 + 0.6 * h)));
        const float rad = cs * (0.06 + 0.16 * h2 * h2);
        if (StatusVeinHash(cell.x * 1.7 + cell.y * 9.1 + 47.0) < 0.9 - 2.2 * length(ctr) / max(reach, 1e-3)) {
            o = StatusOver(o, vec3(0.16, 0.025, 0.021), (1.0 - smoothstep(rad * 0.6, rad, d)) * 0.9);
        }
    }
    return o;
}

// Frenzy: branching fractures with a tarnished gold edge. The shape stays fixed;
// a soft pulse travels down the cracks as the effect builds.
vec4 StatusFrenzy(vec2 q, vec2 c, float reach, float beats, float g) {
    const float seed = (q.x > 0.0 ? 1.0 : 0.0) + (q.y > 0.0 ? 2.0 : 0.0) + 12.0;
    const float along = 0.72 * c.x + 0.69 * c.y;
    const float across = -0.69 * c.x + 0.72 * c.y;
    if (along >= 1.02 * reach) return vec4(0.0);
    const float spread = 0.045 + 0.17 * along;
    const float band = across / spread;
    const float lane = clamp(floor(band + 0.5), -2.0, 2.0);
    const float key = seed * 9.0 + lane * 11.0;
    const float lane_reach = reach * (0.80 + 0.22 * StatusVeinHash(key + 5.0));
    const float taper = 1.0 - smoothstep(0.53 * lane_reach, lane_reach, along);
    const float bend = (StatusNoise(vec2(along * 19.0, key)) - 0.5) * 0.38 +
                       (StatusNoise(vec2(along * 57.0, key + 1.0)) - 0.5) * 0.14;
    const float u = along / max(lane_reach, 1e-3);
    const float split = smoothstep(0.14, 0.82, u) * (StatusVeinHash(key) - 0.5) * 0.35;
    const float local = band - lane - bend;
    const float d = min(abs(local), abs(local - split) + 0.018) * spread;
    const float w = (0.0045 + 0.0038 * StatusVeinHash(key + 2.0)) * (0.6 + 0.4 * taper);
    const float crack = (1.0 - smoothstep(0.18 * w, w, d)) * taper;
    const float halo = 0.20 * (1.0 - smoothstep(w, 3.1 * w, d)) * taper;
    const float pulse = 0.86 + 0.14 * cos(6.2831853 * (fract(beats) - along * 0.8));
    const float edge = smoothstep(0.11 * w, 0.72 * w, d) * (0.46 + 0.54 * g);
    return vec4(mix(vec3(0.34, 0.23, 0.115), vec3(0.54, 0.40, 0.23), edge),
                (crack + halo * (1.0 - crack)) * pulse * (0.79 + 0.21 * g));
}

// Beasthood: coarse dark fur combed out of the corner, three claw gashes torn through it (dark
// wet wounds, raw red edges), throbbing.
vec4 StatusBeast(vec2 q, vec2 c, float reach, float beats, float g) {
    const float Pi = 3.14159265;
    const float Origin = 1.12;
    const vec2 oc = vec2(Origin) - min(abs(q), vec2(Origin));
    const float r = length(oc);
    const float total = reach + 0.17;
    const float seed = (q.x > 0.0 ? 1.0 : 0.0) + (q.y > 0.0 ? 2.0 : 0.0) + 16.0;
    vec4 o = vec4(0.0);
    if (r < total) {
        const float u = atan(oc.y, max(oc.x, 1e-6)) * (2.0 / Pi);
        const float K = 46.0;
        const float v = (u + 0.02 * sin(r * 30.0 + seed)) * K;
        const float lane = floor(v);
        const float f = v - lane - 0.5;
        const float shift = StatusVeinHash(lane + seed);
        const float row = floor(r / 0.035 + shift);
        const float hr = StatusVeinHash(lane * 3.7 + row * 11.3 + seed);
        const float tt = fract(r / 0.035 + shift);
        const float d = abs(f - (hr - 0.5) * 0.8 * tt) * r * (Pi * 0.5) / K;
        const float strand = (1.0 - smoothstep(0.0012, 0.0028, d)) *
                             smoothstep(0.0, 0.15, tt) *
                             (1.0 - smoothstep(0.7, 1.0, tt));
        const float dens = 1.0 - smoothstep(total * 0.45, total * 0.9, r + 0.1 * (g - 0.5));
        const float fur = (hr > 0.2 ? strand : 0.0) * dens +
                          0.45 * dens * g * (1.0 - smoothstep(total * 0.3, total * 0.8, r));
        o = vec4(mix(vec3(0.041, 0.030, 0.019), vec3(0.22, 0.16, 0.093), clamp(strand * hr, 0.0, 1.0)),
                 min(fur, 0.9));
    }
    const float th = radians(38.0) + (StatusVeinHash(seed) - 0.5) * 0.6;
    const vec2 dir = vec2(cos(th), sin(th));
    const float along = dot(c, dir);
    const float across = -c.x * dir.y + c.y * dir.x - 0.35 * (along - 0.15) * (along - 0.15);
    const float k = clamp(floor(across / 0.07 + 0.5), -1.0, 1.0);
    const float hk = StatusVeinHash(k * 5.1 + seed);
    const float s0 = 0.05 * hk + 0.03 * abs(k);
    const float s1 = s0 + reach * (0.75 + 0.25 * StatusVeinHash(k * 2.3 + seed + 1.1)) * (1.0 - 0.15 * abs(k));
    if (along > s0 && along < s1) {
        const float tt = clamp((along - s0) / max(s1 - s0, 1e-3), 0.0, 1.0);
        const float rag = 1.0 + 0.35 * sin(along * 90.0 + hk * 13.0) * sin(along * 37.0 + hk * 5.0);
        const float w = max(0.02 * pow(sin(Pi * pow(tt, 0.8)), 0.7) * rag, 1e-4);
        const float d = abs(across - k * 0.07 + (g - 0.5) * 0.014);
        const float wound = 1.0 - smoothstep(0.3 * w, 0.8 * w, d);
        const float torn = 1.0 - smoothstep(0.8 * w, 1.25 * w, d);
        const float a = max(torn, 0.2 * (1.0 - smoothstep(w, 2.5 * w, d))) *
                        (0.88 + 0.12 * sin(6.2831853 * beats - along * 10.0));
        const vec3 flesh = mix(vec3(0.36, 0.048, 0.036), vec3(0.080, 0.010, 0.012), wound);
        o = StatusOver(o, flesh, a * (0.77 + 0.23 * g));
    }
    return o;
}

// The status `type` (StatusCorners::Type) at q, the fitted field's coordinates (per side, 1 at
// the displayed edge): its colour (linear) and opacity. reach: how far in it has crept (fractions
// of the half-field); beats: the beat clock; width: slow poison's trunk width.
vec4 StatusCorner(vec2 q, float reach, float beats, float width, int type) {
    const vec2 c = vec2(1.0) - min(abs(q), vec2(1.0)); // in from the displayed corner
    const float reach_max = 1.2 * reach + 0.2;
    if (dot(c, c) > reach_max * reach_max) {
        return vec4(0.0);
    }
    const float g = StatusGrime(c, float(type + 1));
    if (type == 3) {
        return StatusFrenzy(q, c, reach, beats, g);
    }
    if (type == 1) {
        return StatusWeb(q, reach, beats, g);
    }
    if (type == 2) {
        return StatusBleed(q, c, reach, beats, g);
    }
    if (type == 4) {
        return StatusBeast(q, c, reach, beats, g);
    }
    return StatusPoison(q, reach, beats, width, g);
}

// StatusCorner zoomed `zoom` times about each corner of the field (status_corners.h
// StatusCornerZoomed: into what the lenses show; 1 = as drawn).
vec4 StatusCornerZoomed(vec2 q, float zoom, float reach, float beats, float width, int type) {
    const float z = clamp(zoom, 1.0, 2.2);
    const vec2 p = vec2(1.0) - (vec2(1.0) - min(abs(q), vec2(1.0))) / z;
    return StatusCorner(vec2(q.x < 0.0 ? -p.x : p.x, q.y < 0.0 ? -p.y : p.y), reach, beats, width,
                        type);
}
