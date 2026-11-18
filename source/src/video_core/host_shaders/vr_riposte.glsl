// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The riposte's burst (vr.ini riposte_style; core/vr/riposte_style.h, the timing): the view as
// black / blood red / white with black ink lines, blended over it by `strength`. Its brightness,
// smoothed over a few pixels (3x3 taps `s` apart, a 1-2-1 kernel) so the bands follow shapes and
// not texture, is cut at thresholds from the view's brightness (a dark street splits as a lit one
// does). That brightness is one value for both eyes, measured over both by vr_riposte_meter.comp
// and eased from frame to frame. When each eye measured itself from four taps, one eye could be
// red beside a black one, and the bands crawled with the head. Ink where the smoothed brightness
// jumps (the taps' Sobel slope). Tuned on eye frames against the look chosen.
// Cheap on purpose (the tester: "almost no overhead"): 8 reads a pixel besides its own, compiled
// only into the passes' burst variants, which run only while the burst shows. (Four diagonal taps
// alone were cheaper but doubled edges and hatched brickwork: too sparse a filter.)
//
// The including shader defines, before including this:
//   vec3 RiposteFetch(vec2 p): the source's linear RGB at pixel position p (pixel centres at
//   +0.5), clamped to the source, read as vr_riposte_meter.comp reads it;
//   optionally RIPOSTE_SPACING(size): the taps' distance (pixels) for a source `size` wide (a
//   pass without a sampler fetches whole texels: a whole number of them).

#ifndef RIPOSTE_SPACING
#define RIPOSTE_SPACING(size) max(1.5, (size).x / 640.0 * 1.5)
#endif

const vec3 RiposteBlack = vec3(0.0030, 0.0027, 0.0037); // sRGB (10, 9, 12), linear
// The red and the ink stronger for both bursts (2026-11-01: "increase the intensity";
// the throw's burst already covers the view): red was sRGB (214, 18, 40), the ink from 2.6x the
// brightness to 1.6x that.
const vec3 RiposteRed = vec3(0.8388, 0.0030, 0.0116);   // sRGB (236, 10, 28)
const vec3 RiposteWhite = vec3(0.9216, 0.8714, 0.8069); // sRGB (246, 240, 232)

float RiposteLuma(vec3 lin) {
    return sqrt(max(dot(lin, vec3(0.2126, 0.7152, 0.0722)), 0.0));
}

float RiposteSmooth(float e0, float e1, float x) {
    const float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// `base`: what the pixel shows (linear); `l4`: the RiposteLuma of its own source texel, read as
// RiposteFetch reads (the 3x3's centre: one image with the taps); `p`: its position in the source
// (pixels, at a pixel centre); `size`: the source's size (the taps' spacing); `avg`: the view's
// brightness (vr_riposte_meter.comp).
vec3 RiposteStyle(vec3 base, float l4, vec2 p, vec2 size, float avg, float strength, bool roar) {
    if (roar) {
        // Three tonal bands from the current pixel. Roar needs a sudden change of
        // palette; the eight neighboring samples are for the visceral's ink outlines.
        const float hi = max(1.65 * avg, avg + .06);
        vec3 c = mix(RiposteBlack, vec3(.16, .004, .011), RiposteSmooth(avg - .02, avg + .02, l4));
        c = mix(c, vec3(.48, .027, .038), RiposteSmooth(hi - .02, hi + .02, l4));
        return mix(base, c, clamp(strength, 0.0, 1.0));
    }
    const float s = RIPOSTE_SPACING(size);
    const float l0 = RiposteLuma(RiposteFetch(p + vec2(-s, -s)));
    const float l1 = RiposteLuma(RiposteFetch(p + vec2(0.0, -s)));
    const float l2 = RiposteLuma(RiposteFetch(p + vec2(s, -s)));
    const float l3 = RiposteLuma(RiposteFetch(p + vec2(-s, 0.0)));
    const float l5 = RiposteLuma(RiposteFetch(p + vec2(s, 0.0)));
    const float l6 = RiposteLuma(RiposteFetch(p + vec2(-s, s)));
    const float l7 = RiposteLuma(RiposteFetch(p + vec2(0.0, s)));
    const float l8 = RiposteLuma(RiposteFetch(p + vec2(s, s)));
    const float lc = (l0 + 2.0 * l1 + l2 + 2.0 * l3 + 4.0 * l4 + 2.0 * l5 + l6 + 2.0 * l7 + l8) *
                     (1.0 / 16.0);
    const float gx = (l2 + 2.0 * l5 + l8) - (l0 + 2.0 * l3 + l6);
    const float gy = (l6 + 2.0 * l7 + l8) - (l0 + 2.0 * l1 + l2);
    const float g = length(vec2(gx, gy));
    const float lo = avg;
    const float hi = max(1.65 * avg, lo + 0.06);
    const float band = 0.02;
    vec3 c = mix(RiposteBlack, RiposteRed, RiposteSmooth(lo - band, lo + band, lc));
    c = mix(c, RiposteWhite, RiposteSmooth(hi - band, hi + band, lc));
    const float ink_t = max(2.2 * avg, 0.08);
    c = mix(c, RiposteBlack, RiposteSmooth(ink_t, ink_t * 1.5, g));
    return mix(base, c, clamp(strength, 0.0, 1.0));
}
