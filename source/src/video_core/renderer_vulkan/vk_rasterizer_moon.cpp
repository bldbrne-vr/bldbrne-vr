// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Moonlight in the air (shader_recompiler/ir/passes/moon_shafts.h): what the light composite's
// VrMoonShafts reads. The game's screen-space shadow pass gives the moon's cascades (cascades336,
// the flat camera's) and its shadow atlas (the composite's host image); the directional light
// pass its light-set table (light 0 of a set is the shadowed light: the moon or the evening sun;
// the GPU writes the table, so the composite binds it as a host buffer and reads the colours
// itself). The composite gets, for the flat camera's view, each cascade's view-to-shadow rows
// (S_k P: the screen-to-shadow matrix after the projection), its tile of the atlas and its depth
// bias, and the moon's direction (the one the cascades' uv does not change along, towards the
// light). Only the real moon or sun: the sets' unshadowed second light (a fill) is never read.

#include "video_core/native_stereo/draw_memory.h"
#include "video_core/native_stereo/lighting_reconstruction.h"
#include "video_core/native_stereo/native_stereo.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "common/logging/log.h"
#include "core/vr/game_area.h"
#include "core/vr/graphics_settings.h"
#include "core/vr/live_tune.h"
#include "core/vr/moon_screen.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vr_shadow_hold.h"

namespace Vulkan {
namespace {

namespace Moon = Shader::Optimization::MoonShafts;
using Matrix4d = std::array<double, 16>; // row-major

// The inverse of a general 4x4 (cofactors); false when it is singular.
bool Invert(const Matrix4d& m, Matrix4d& out) {
    Matrix4d inv;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::abs(det) < 1e-30) {
        return false;
    }
    for (u32 i = 0; i < 16; ++i) {
        out[i] = inv[i] / det;
    }
    return true;
}

Matrix4d Product(const Matrix4d& a, const Matrix4d& b) {
    Matrix4d c{};
    for (u32 r = 0; r < 4; ++r) {
        for (u32 k = 0; k < 4; ++k) {
            for (u32 col = 0; col < 4; ++col) {
                c[4 * r + col] += a[4 * r + k] * b[4 * k + col];
            }
        }
    }
    return c;
}

// A captured value is this frame's or the last one's (the recording may run a frame behind
// the flips).
bool Fresh(u32 captured, u32 frame) {
    // (Three since 2026-11-14: air_light.h FrameSlack, the headset's dark blinks.)
    return captured != ~0u && frame - captured <= 3;
}

float KnobValue(const char* name, float fallback, float lo, float hi) {
    const char* v = std::getenv(name);
    const float x = v ? std::strtof(v, nullptr) : fallback;
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}

// Read once, at the first use (getenv locks the environment), as the pipeline cache's rule.
struct Knobs {
    const char* shafts = std::getenv("VR_MOON_SHAFTS");
    const char* quality = std::getenv("VR_MOON_SHAFTS_QUALITY");
    float density = KnobValue("VR_MOON_SHAFTS_DENSITY", Moon::Density, 0.0f, 10.0f);
    // Tuning only: VR_MOON_SHAFTS_G, _FORWARD, _STRENGTH, _FAR.
    float phase_g = KnobValue("VR_MOON_SHAFTS_G", Moon::PhaseG, 0.0f, 0.95f);
    float forward = KnobValue("VR_MOON_SHAFTS_FORWARD", Moon::ForwardShare, 0.0f, 1.0f);
    float strength = KnobValue("VR_MOON_SHAFTS_STRENGTH", Moon::Strength, 0.0f, 100.0f);
    float far_m = KnobValue("VR_MOON_SHAFTS_FAR", Moon::FarM, 5.0f, 500.0f);
    // Tests only: VR_MOON_SHAFTS_COLOUR=r,g,b when no light set gives the moon's.
    std::array<float, 3> colour = [] {
        std::array<float, 3> c{-1.0f, -1.0f, -1.0f};
        if (const char* v = std::getenv("VR_MOON_SHAFTS_COLOUR")) {
            std::sscanf(v, "%f,%f,%f", &c[0], &c[1], &c[2]);
        }
        return c;
    }();
};
const Knobs& GetKnobs() {
    static const Knobs knobs;
    return knobs;
}

} // namespace

namespace {

// The moon's light space of a frame's cascades (as VrMoonUpload's): the nearest cascade's
// view-to-shadow rows as unit axes, and the direction towards the shadow light.
bool MoonLightSpace(const std::array<float, 84>& c, float compare_sign,
                    std::array<std::array<double, 3>, 3>& axes, std::array<double, 3>& towards) {
    Matrix4d inverse_p, p, s;
    for (u32 i = 0; i < 16; ++i) {
        inverse_p[i] = c[i];
        s[i] = c[16 + i];
    }
    if (!Invert(inverse_p, p)) {
        return false;
    }
    Matrix4d s0 = Product(s, p);
    const double w = s0[15];
    if (!std::isfinite(w) || std::abs(w) < 1e-12) {
        return false;
    }
    for (double& v : s0) {
        v /= w;
    }
    towards = {s0[1] * s0[6] - s0[2] * s0[5], s0[2] * s0[4] - s0[0] * s0[6], s0[0] * s0[5] - s0[1] * s0[4]};
    const double norm = std::sqrt(towards[0] * towards[0] + towards[1] * towards[1] + towards[2] * towards[2]);
    if (!(norm > 1e-12)) {
        return false;
    }
    for (double& v : towards) {
        v /= norm;
    }
    if (compare_sign * (s0[8] * towards[0] + s0[9] * towards[1] + s0[10] * towards[2]) > 0) {
        for (double& v : towards) {
            v = -v;
        }
    }
    for (u32 r = 0; r < 3; ++r) {
        const double x = s0[4 * r], y = s0[4 * r + 1], z = s0[4 * r + 2];
        const double len = std::sqrt(x * x + y * y + z * z);
        if (!(len > 1e-12) || !std::isfinite(len)) {
            return false;
        }
        axes[r] = {x / len, y / len, z / len};
    }
    return true;
}

} // namespace

// The moon you can see (air_light.h MoonAim; VrMoonUpload, under vr_moon.mutex): the brightest
// sky fragment the scene's copy reported a few frames ago, as a direction of that frame's camera
// in the moon's light space; taken for the moon once enough frames in a row agree.
void Rasterizer::VrAirMoonAim(const std::array<double, 3>* shadow_world) {
    namespace Air = Shader::Optimization::AirLight;
    auto& aim = vr_air_aim;
    if (!aim.pending) {
        return;
    }
    aim.pending = false;
    const u32 word = aim.read_word;
    if (word != 0) {
        aim.sky_seen_ms = VR::MoonScreen::NowMs(); // some sky is in view (air_light.h Mist::Indoors)
    }
    const auto& camera = aim.read_camera;
    const float brightness = float(word >> 20) / Air::MoonAimLumScale;
    if (!camera.valid || word == 0 || brightness <= 0.0f) {
        aim.agree = 0;
        return;
    }
    // The fragment's ray: its pixel through that frame's first eye (a mid depth: any point on it),
    // ahead of the eye (view z is ahead; a depth past the projection's range comes back mirrored).
    const double px = 2.0 * double((word >> 10) & 1023u) + 1.0, py = 2.0 * double(word & 1023u) + 1.0;
    const double nx = px * camera.ndc[0] + camera.ndc[2], ny = py * camera.ndc[1] + camera.ndc[3];
    // Two points of the pixel's ray (two depths): its direction is their difference. (Until
    // 2026-11-14 one point's direction from the view's origin was taken: only right when the eye
    // sits at that origin, and the moon came out some 18 degrees up where it stood near 80.)
    const auto point = [&](double depth, std::array<double, 3>& out) {
        std::array<double, 4> q{};
        for (u32 r = 0; r < 4; ++r) {
            q[r] = camera.inverse[4 * r] * nx + camera.inverse[4 * r + 1] * ny + camera.inverse[4 * r + 2] * depth +
                   camera.inverse[4 * r + 3];
        }
        if (!(std::abs(q[3]) > 1e-12)) {
            return false;
        }
        out = {q[0] / q[3], q[1] / q[3], q[2] / q[3]};
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    };
    std::array<double, 3> pa{}, pb{}, one{};
    if (!point(0.25, pa) || !point(0.75, pb) || !point(0.5, one)) {
        return;
    }
    std::array<double, 3> d{pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
    const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dl > 1e-12) || !std::isfinite(dl)) {
        return;
    }
    const double ahead = d[2] < 0.0 ? -1.0 : 1.0;
    for (double& v : d) {
        v *= ahead / dl;
    }
    if constexpr (Common::Log::Enabled) {
        static u32 ray_logs = 0;
        if (ray_logs < 12 && brightness > 0.05f && vr_moon.frame % 120 == 5) {
            ++ray_logs;
            LOG_INFO(Render_Vulkan,
                     "VR air pass: moon ray check: pixel ({:.0f} {:.0f}) ndc ({:.3f} {:.3f}); points at depth 0.25 "
                     "({:.4f} {:.4f} {:.4f}), 0.5 ({:.4f} {:.4f} {:.4f}), 0.75 ({:.4f} {:.4f} {:.4f}); ray ({:.3f} {:.3f} "
                     "{:.3f})",
                     px, py, nx, ny, pa[0], pa[1], pa[2], one[0], one[1], one[2], pb[0], pb[1], pb[2], d[0], d[1], d[2]);
        }
    }
    // Into the world (that frame's view-to-world rows): the moon does not turn with the head.
    std::array<double, 3> world{};
    for (u32 r = 0; r < 3; ++r) {
        world[r] = camera.view[4 * r] * d[0] + camera.view[4 * r + 1] * d[1] + camera.view[4 * r + 2] * d[2];
    }
    const double wl = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
    if (!(wl > 1e-9) || !std::isfinite(wl)) {
        return;
    }
    for (double& v : world) {
        v /= wl;
    }
    const double agree_cos = aim.candidate[0] * world[0] + aim.candidate[1] * world[1] + aim.candidate[2] * world[2];
    const bool agrees = aim.agree > 0 && agree_cos >= std::cos(Air::MoonAimAgreeDeg / 57.29577951308232);
    aim.agree = agrees ? aim.agree + 1 : 1;
    aim.candidate = world;
    // The moon is held (air_light.h MoonAimSameDeg): refined by sightings near it, replaced only
    // by something clearly brighter that stays, never by the next brightest sky while it hides.
    const auto dot3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const double deg = 57.29577951308232;
    const bool near_shadow =
        world[1] >= Air::MoonAimMinUp &&
        (shadow_world == nullptr || dot3(*shadow_world, world) >= std::cos(Air::MoonAimShadowDeg / deg));
    const bool have = aim.moon_seen_ms >= 0;
    const bool same = have && dot3(aim.moon_world, world) >= std::cos(Air::MoonAimSameDeg / deg);
    const char* verdict = "not taken";
    if (aim.agree >= Air::MoonAimFrames && near_shadow) {
        if (!have) {
            aim.moon_world = world;
            aim.held_brightness = brightness;
            verdict = "the moon (first find)";
        } else if (same && brightness >= Air::MoonAimKeep * aim.held_brightness) {
            double len = 0.0;
            for (u32 k = 0; k < 3; ++k) {
                aim.moon_world[k] += (world[k] - aim.moon_world[k]) * Air::MoonAimRefine;
                len += aim.moon_world[k] * aim.moon_world[k];
            }
            len = std::sqrt(std::max(len, 1e-12));
            for (double& v : aim.moon_world) {
                v /= len;
            }
            aim.held_brightness = std::max(aim.held_brightness, brightness);
            verdict = "the moon (refined)";
        } else if (!same && brightness >= Air::MoonAimReplace * aim.held_brightness &&
                   aim.agree >= Air::MoonAimReplaceFrames) {
            aim.moon_world = world;
            aim.held_brightness = brightness;
            verdict = "the moon (replaced: brighter)";
        }
        if (verdict[0] == 't') {
            aim.moon_seen_ms = VR::MoonScreen::NowMs();
            if (shadow_world != nullptr) {
                aim.shadow_world = *shadow_world;
            }
        }
    }
    if constexpr (Common::Log::Enabled) {
        static u32 logs = 0;
        if (logs < 60 && (aim.agree == Air::MoonAimFrames || aim.agree == Air::MoonAimReplaceFrames ||
                          vr_moon.frame % 600 == 3)) {
            ++logs;
            LOG_INFO(Render_Vulkan,
                     "VR air pass: brightest sky at pixel ({:.0f} {:.0f}), brightness {:.3f} (held {:.3f}), {} frame(s) "
                     "agreeing, {} the shadow light: {}; world ({:.3f} {:.3f} {:.3f}), the moon held ({:.3f} {:.3f} {:.3f}), "
                     "the shadow light in the world ({:.3f} {:.3f} {:.3f})",
                     px, py, brightness, aim.held_brightness, aim.agree, near_shadow ? "near" : "far from", verdict,
                     world[0], world[1], world[2], aim.moon_world[0], aim.moon_world[1], aim.moon_world[2],
                     shadow_world ? (*shadow_world)[0] : 0.0, shadow_world ? (*shadow_world)[1] : 0.0,
                     shadow_world ? (*shadow_world)[2] : 0.0);
        }
    }
}

void Rasterizer::VrMoonCaptureCascades(const float* source, u32 size, bool guest) {
    static u32 rejects = 0; // logs only
    if (size < 336 || VrInMirrorPass()) {
        if constexpr (Common::Log::Enabled) {
            if (rejects++ < 5) {
                LOG_INFO(Render_Vulkan, "VR moon shafts: shadow pass buffer 0 of {} bytes{}", size,
                         VrInMirrorPass() ? " (a reflection's)" : "");
            }
        }
        return; // a planar reflection's pass has its mirrored camera's
    }
    std::array<float, 84> values;
    std::memcpy(values.data(), guest ? ScratchDraw::MemorySource(source, 336) : source, 336);
    // (A held atlas' own matrices for the game's of that frame: vr_shadow_hold.h. The ring's raw
    // copy holds them already; a guest read finds them by the raw bytes.)
    ShadowHold::Lookup(values.data());
    if (NativeProjection::RecognizeLighting({values.data(), values.size()}) !=
        NativeProjection::LightingLayout::Cascades) {
        if constexpr (Common::Log::Enabled) {
            if (rejects++ < 5) {
                LOG_INFO(Render_Vulkan,
                         "VR moon shafts: shadow pass buffer 0 ({} bytes) is no cascades336: "
                         "{:.3f} {:.3f} {:.3f} {:.3f} / splits {:.2f} {:.2f} {:.2f} {:.2f}",
                         size, values[0], values[5], values[11], values[15], values[80],
                         values[81], values[82], values[83]);
            }
        }
        return;
    }
    std::scoped_lock lock{vr_moon.mutex};
    vr_moon.cascades = values;
    vr_moon.cascades_frame = vr_moon.frame;
    ++vr_moon.cascades_serial;
}

void Rasterizer::VrMoonCaptureAtlas(const Shader::Info& stage) {
    if (VrInMirrorPass()) {
        return;
    }
    for (const auto& desc : stage.images) {
        if (!desc.is_depth || desc.IsVrHost()) {
            continue;
        }
        const AmdGpu::Image sharp = desc.GetSharp(stage);
        // The atlas: the big depth image the pass compares against (the eye's depth is read as a
        // plain texture).
        if (!sharp || sharp.width + 1 < 1024 || sharp.height + 1 < 1024) {
            continue;
        }
        float sign = 1.0f;
        for (const auto& sampler : stage.samplers) {
            const auto func = sampler.GetSharp(stage).depth_compare_func.Value();
            if (func == AmdGpu::DepthCompare::Greater || func == AmdGpu::DepthCompare::GreaterEqual) {
                sign = -1.0f;
            }
        }
        const bool changed = Shader::SetVrHostImage(Shader::VrHostImage::MoonAtlas, sharp);
        std::scoped_lock lock{vr_moon.mutex};
        vr_moon.atlas_width = u32(sharp.width + 1);
        vr_moon.atlas_height = u32(sharp.height + 1);
        vr_moon.compare_sign = sign;
        vr_moon.atlas_frame = vr_moon.frame;
        if (changed) {
            LOG_INFO(Render_Vulkan,
                     "VR moon shafts: shadow atlas {:#x} {}x{} (data format {}, number format {}), "
                     "compare sign {}",
                     sharp.Address(), vr_moon.atlas_width, vr_moon.atlas_height,
                     u32(sharp.GetDataFmt()), u32(sharp.GetNumberFmt()), sign);
        }
        return;
    }
}

void Rasterizer::VrMoonCaptureSets(const AmdGpu::Buffer& sharp, u32 size) {
    const u32 set_bytes = Moon::SetDwords * sizeof(float);
    if (size < set_bytes || VrInMirrorPass()) {
        return;
    }
    const bool changed = Shader::SetVrHostBuffer(Shader::VrHostBuffer::MoonSets, sharp);
    std::scoped_lock lock{vr_moon.mutex};
    vr_moon.sets = std::min<u32>(size / set_bytes, Moon::TableSets);
    vr_moon.sets_frame = vr_moon.frame;
    if (changed) {
        LOG_INFO(Render_Vulkan, "VR moon shafts: light-set table {:#x}, {} bytes ({} sets)",
                 sharp.base_address, size, size / set_bytes);
    }
}

u32 Rasterizer::VrMoonUpload(std::span<u32> out, bool for_grid) {
    std::ranges::fill(out, 0u);
    const u32 bytes = Moon::BufferDwords * sizeof(u32);
    std::scoped_lock lock{vr_moon.mutex};
    // Why a composite gets no moon (logs only: every 600th such upload, 20 at most).
    static u32 idles = 0, idle_logs = 0;
    const auto idle = [&](const char* why, float value) {
        if constexpr (Common::Log::Enabled) {
            if (idles++ % 600 == 0 && idle_logs < 20) {
                ++idle_logs;
                LOG_INFO(Render_Vulkan,
                         "VR moon shafts: no moon for this composite ({} {}; frame {}, cascades "
                         "{}, atlas {}, sets {} x{})",
                         why, value, vr_moon.frame, vr_moon.cascades_frame, vr_moon.atlas_frame,
                         vr_moon.sets_frame, vr_moon.sets);
            }
        }
        return bytes; // steps 0: nothing marched
    };
    // By the game's area (moon_shafts.h AreaFull), eased once a frame.
    static u64 area_frame = ~0ull;
    if (const u64 id = VR::DrawFrameRecord().id; id != area_frame) {
        area_frame = id;
        const auto now_t = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now_t - vr_air.moon_full_time).count(), 0.0f, 1.0f);
        vr_air.moon_full_time = now_t;
        const float want = Moon::AreaFull(VR::GameArea::Area()) ? 1.0f : 0.0f;
        if (!vr_air.moon_full_valid) {
            vr_air.moon_full = want;
            vr_air.moon_full_valid = true;
        } else {
            vr_air.moon_full += (want - vr_air.moon_full) * (1.0f - std::exp(-dt / Moon::AreaEaseS));
        }
    }
    const bool own_numbers = GetKnobs().strength != Moon::Strength || GetKnobs().far_m != Moon::FarM;
    const float full = own_numbers ? 0.0f : std::clamp(vr_air.moon_full, 0.0f, 1.0f);
    const float area_strength = GetKnobs().strength + (Moon::WoodsStrength - Moon::Strength) * full;
    const float area_far = GetKnobs().far_m + (Moon::WoodsFarM - Moon::FarM) * full;
    const float strength = std::clamp(VR::Tune::Get("lighting.shafts", area_strength), 0.0f, 1.0f);
    // (The fog grid shadows its mist with the cascades also while the glow is turned down to 0.)
    if (!vr_moon.on || (strength == 0.0f && !for_grid) || VrInMirrorPass()) {
        return bytes;
    }
    if (!Fresh(vr_moon.cascades_frame, vr_moon.frame) ||
        !Fresh(vr_moon.atlas_frame, vr_moon.frame)) {
        return idle("no cascades or atlas", 0.0f);
    }
    const auto& c = vr_moon.cascades;
    Matrix4d inverse_p, p;
    for (u32 i = 0; i < 16; ++i) {
        inverse_p[i] = c[i];
    }
    if (!Invert(inverse_p, p)) {
        return idle("singular inverse P", c[15]);
    }
    // View to shadow per cascade: S_k P (S_k's last row is inverse P's: an affine result).
    std::array<Matrix4d, Moon::Cascades> shadow;
    for (u32 k = 0; k < Moon::Cascades; ++k) {
        Matrix4d s;
        for (u32 i = 0; i < 16; ++i) {
            s[i] = c[16 + 16 * k + i];
        }
        shadow[k] = Product(s, p);
        const double w = shadow[k][15];
        if (!std::isfinite(w) || std::abs(w) < 1e-12) {
            return idle("cascade w", float(w));
        }
        for (double& v : shadow[k]) {
            v /= w;
        }
    }
    // Towards the moon: the direction along which the nearest cascade's uv stays (cross of its
    // uv rows), the side its depth decreases on (for the compare's sign: lit while <=).
    const Matrix4d& s0 = shadow[0];
    std::array<double, 3> towards{s0[1] * s0[6] - s0[2] * s0[5], s0[2] * s0[4] - s0[0] * s0[6],
                                  s0[0] * s0[5] - s0[1] * s0[4]};
    const double norm = std::sqrt(towards[0] * towards[0] + towards[1] * towards[1] +
                                  towards[2] * towards[2]);
    if (!(norm > 1e-12)) {
        return idle("uv rows parallel", float(norm));
    }
    for (double& v : towards) {
        v /= norm;
    }
    if (vr_moon.compare_sign * (s0[8] * towards[0] + s0[9] * towards[1] + s0[10] * towards[2]) > 0) {
        for (double& v : towards) {
            v = -v;
        }
    }
    // The glow's direction (moon_shafts.h GlowFreshMs): the visible moon. Its screen point through
    // the flat camera (x right, y up, z ahead: the light list's view), into the moon's light space
    // (s0's rows, each a scaled unit axis: normalized) while the game shows it, back from there
    // every frame (the head turns, the light space does not).
    std::array<double, 3> glow = towards;
    const bool moon_aim = Shader::Optimization::AirLight::MoonAim();
    if (Moon::GlowFromScreen() || moon_aim) {
        std::array<std::array<double, 3>, 3> axes{};
        bool axes_ok = true;
        for (u32 r = 0; r < 3; ++r) {
            const double x = s0[4 * r], y = s0[4 * r + 1], z = s0[4 * r + 2];
            const double len = std::sqrt(x * x + y * y + z * z);
            axes_ok = axes_ok && len > 1e-12 && std::isfinite(len);
            axes[r] = {x / len, y / len, z / len};
        }
        const VR::CameraRecord flat = VR::DrawFrameRecord();
        float su = 0.0f, sv = 0.0f;
        const s64 now = VR::MoonScreen::NowMs();
        if (Moon::GlowFromScreen() && axes_ok && VR::MoonScreen::Take(su, sv, Moon::GlowFreshMs) && su >= 0.0f && su <= 1.0f &&
            sv >= 0.0f && sv <= 1.0f && flat.vfov > 0.0f && flat.aspect > 0.0f && towards[2] > 0.0) {
            const double ty = std::tan(0.5 * flat.vfov), tx = ty * flat.aspect;
            std::array<double, 3> d{(2.0 * su - 1.0) * tx, (1.0 - 2.0 * sv) * ty, 1.0};
            const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const double cosine = (d[0] * towards[0] + d[1] * towards[1] + d[2] * towards[2]) / dl;
            const double angle = std::acos(std::clamp(cosine, -1.0, 1.0)) * 57.29577951308232;
            if (angle <= Moon::GlowMaxDeg) {
                for (u32 r = 0; r < 3; ++r) {
                    vr_moon.glow_light[r] =
                        (axes[r][0] * d[0] + axes[r][1] * d[1] + axes[r][2] * d[2]) / dl;
                }
                vr_moon.glow_seen_ms = now;
            }
            if constexpr (Common::Log::Enabled) {
                static u32 logs = 0;
                if (logs < 40 && (logs < 4 || vr_moon.frame % 300 == 0)) {
                    ++logs;
                    LOG_INFO(Render_Vulkan,
                             "VR moon shafts: visible moon at screen ({:.3f} {:.3f}): {:.1f} deg "
                             "from the shadow light ({}); shadow light ({:.3f} {:.3f} {:.3f}), "
                             "visible ({:.3f} {:.3f} {:.3f})",
                             su, sv, angle, angle <= Moon::GlowMaxDeg ? "taken" : "ignored",
                             towards[0], towards[1], towards[2], d[0] / dl, d[1] / dl, d[2] / dl);
                }
            }
        }
        // The moon seen in the sky (air_light.h MoonAim): held in the world, through this frame's
        // camera (the lamp memory's view-to-world rows); the glow glides to it, or back to the
        // shadow light's direction where no moon is held (another area), never jumps.
        if (moon_aim) {
            std::array<float, 12> view;
            bool view_ok;
            {
                std::scoped_lock fog_lock{vr_fog.mutex};
                view = vr_fog.view;
                view_ok = vr_fog.view_frame != 0;
            }
            // This frame's own rotation where the reflections pass gave it (air_light.h Mist: the
            // lamp memory's camera can be frames behind the cascades in a turn; with it the shadow
            // light seemed to swing in the world and the moon was dropped as "another area").
            const u64 frame_id = VR::DrawFrameRecord().id;
            // (Or the frame before's: the blur binds this buffer before the reflections pass.)
            if (Shader::Optimization::AirLight::FrameNear(vr_air.view_frame, frame_id, 1)) {
                view = vr_air.view;
                view_ok = true;
            }
            auto& aim = vr_air_aim;
            if (view_ok) {
                namespace Air = Shader::Optimization::AirLight;
                const double deg = 57.29577951308232;
                std::array<double, 3> shadow_world{};
                for (u32 r = 0; r < 3; ++r) {
                    shadow_world[r] = double(view[4 * r]) * towards[0] + double(view[4 * r + 1]) * towards[1] +
                                      double(view[4 * r + 2]) * towards[2];
                }
                VrAirMoonAim(&shadow_world);
                // Another place or hour (the shadow light points elsewhere in the world, for two
                // seconds of frames in a row: not a turn's stale camera): start again.
                if (aim.moon_seen_ms >= 0 && aim.area_frame != frame_id) {
                    aim.area_frame = frame_id;
                    const bool elsewhere =
                        aim.shadow_world[0] * shadow_world[0] + aim.shadow_world[1] * shadow_world[1] +
                            aim.shadow_world[2] * shadow_world[2] < std::cos(Air::MoonAimAreaDeg / deg);
                    aim.area_miss = elsewhere ? aim.area_miss + 1 : 0;
                    if (aim.area_miss >= 120) {
                        aim.moon_seen_ms = -1;
                        aim.held_brightness = 0.0f;
                        aim.agree = 0;
                        aim.area_miss = 0;
                    }
                }
                // (Only where the finder aims the glow, air_light.h MoonAimGlow: else the shadow
                // light's direction as it is, this frame's.)
                if (Air::MoonAimGlow()) {
                    const std::array<double, 3>& target = aim.moon_seen_ms >= 0 ? aim.moon_world : shadow_world;
                    const double dt = aim.glow_ms >= 0 ? std::clamp(double(now - aim.glow_ms) * 1e-3, 0.0, 0.25) : 0.0;
                    aim.glow_ms = now;
                    if (!aim.glow_valid) {
                        aim.glow_world = target;
                        aim.glow_valid = true;
                    } else {
                        const double share = 1.0 - std::exp(-dt / Air::MoonAimGlideS);
                        double len = 0.0;
                        for (u32 k = 0; k < 3; ++k) {
                            aim.glow_world[k] += (target[k] - aim.glow_world[k]) * share;
                            len += aim.glow_world[k] * aim.glow_world[k];
                        }
                        len = std::sqrt(len);
                        if (len > 1e-6 && std::isfinite(len)) {
                            for (double& v : aim.glow_world) {
                                v /= len;
                            }
                        } else {
                            aim.glow_world = target;
                        }
                    }
                    std::array<double, 3> g{};
                    for (u32 c = 0; c < 3; ++c) {
                        for (u32 r = 0; r < 3; ++r) {
                            g[c] += double(view[4 * r + c]) * aim.glow_world[r];
                        }
                    }
                    const double gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                    if (gl > 1e-9 && std::isfinite(gl)) {
                        glow = {g[0] / gl, g[1] / gl, g[2] / gl};
                    }
                    if constexpr (Common::Log::Enabled) {
                        static u32 logs = 0;
                        if (logs < 30 && vr_moon.frame % 300 == 7) {
                            ++logs;
                            const double off = std::acos(std::clamp(glow[0] * towards[0] + glow[1] * towards[1] +
                                                                        glow[2] * towards[2], -1.0, 1.0)) * deg;
                            LOG_INFO(Render_Vulkan,
                                     "VR air pass: the glow points {:.1f} deg from the shadow light ({}): glow ({:.3f} "
                                     "{:.3f} {:.3f}), shadow light ({:.3f} {:.3f} {:.3f})",
                                     off, aim.moon_seen_ms >= 0 ? "at the moon held" : "no moon held", glow[0], glow[1],
                                     glow[2], towards[0], towards[1], towards[2]);
                        }
                    }
                }
            } else {
                VrAirMoonAim(nullptr);
            }
        } else if (axes_ok && vr_moon.glow_seen_ms >= 0 && now - vr_moon.glow_seen_ms <= Moon::GlowHoldMs) {
            std::array<double, 3> g{};
            for (u32 r = 0; r < 3; ++r) {
                for (u32 i = 0; i < 3; ++i) {
                    g[i] += axes[r][i] * vr_moon.glow_light[r];
                }
            }
            const double gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            if (gl > 1e-9 && std::isfinite(gl)) {
                glow = {g[0] / gl, g[1] / gl, g[2] / gl};
            }
        }
    }
    // The moon's colour is the GPU's (the pixel's light set in the table, a host buffer of the
    // composite: the shader checks its light 0 points that way); a test's (VR_MOON_SHAFTS_COLOUR)
    // is added to it.
    std::array<float, 3> colour{};
    if (GetKnobs().colour[0] >= 0.0f) {
        colour = GetKnobs().colour;
    }
    const u32 sets = Fresh(vr_moon.sets_frame, vr_moon.frame) ? vr_moon.sets : 0u;
    if (sets == 0 && colour[0] + colour[1] + colour[2] <= 0.0f) {
        return idle("no light-set table", 0.0f);
    }
    // The cascades' far ends in view z: the splits, then the last's (dword 83 when it is beyond
    // the third split, else the far distance).
    const Knobs& knobs = GetKnobs();
    const std::array<float, 4> ends{c[80], c[81], c[82], c[83] > c[82] ? c[83] : 4.0f * knobs.far_m};
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    const float half =
        NativeStereo::Enabled() ? 0.5f * NativeStereo::Ipd(rec) * rec.world_scale : 0.0f;
    const auto put = [&](u32 dword, float v) { out[dword] = std::bit_cast<u32>(v); };
    out[0] = vr_moon.steps;
    out[1] = vr_moon.frame;
    put(2, vr_moon.compare_sign);
    put(3, std::isfinite(half) ? half : 0.0f);
    put(4, vr_moon.density);
    put(5, area_far);
    put(6, knobs.phase_g);
    put(7, knobs.forward);
    for (u32 i = 0; i < 3; ++i) {
        put(8 + i, float(glow[i])); // the phase's: the visible moon
        put(12 + i, colour[i]);
    }
    put(11, strength);
    for (u32 i = 0; i < 4; ++i) {
        put(16 + i, ends[i]);
    }
    put(20, float(vr_moon.atlas_width));
    put(21, float(vr_moon.atlas_height));
    out[22] = sets;
    std::array<std::array<float, 2>, Moon::Cascades> mids{};
    for (u32 k = 0; k < Moon::Cascades; ++k) {
        const Matrix4d& s = shadow[k];
        const u32 base = Moon::HeaderDwords + Moon::CascadeDwords * k;
        for (u32 i = 0; i < 12; ++i) {
            put(base + i, float(s[i]));
        }
        // Its tile: the quarter of the atlas holding the point on the view axis halfway through
        // the cascade's depth range.
        const float start = k == 0 ? 0.5f : ends[k - 1];
        const float end = k < 3 ? ends[k] : std::min(ends[3], start * 2.0f);
        const double z = 0.5 * (start + end);
        const double u = s[2] * z + s[3], v = s[6] * z + s[7];
        mids[k] = {float(u), float(v)};
        put(base + 12, std::clamp(float(std::floor(u * 2.0) * 0.5), 0.0f, 0.5f));
        put(base + 13, std::clamp(float(std::floor(v * 2.0) * 0.5), 0.0f, 0.5f));
        put(base + 14, 0.5f);
        // The bias in its depth units: depth per metre along its depth row.
        const double per_m = std::sqrt(s[8] * s[8] + s[9] * s[9] + s[10] * s[10]);
        put(base + 15, float(Moon::BiasM * per_m));
    }
    if constexpr (Common::Log::Enabled) {
        // Every 600th upload (about 10 s), 30 at most.
        static u32 uploads = 0, logs = 0;
        if (uploads++ % 600 == 0 && logs < 30) {
            ++logs;
            LOG_INFO(Render_Vulkan,
                     "VR moon shafts: towards the moon ({:.3f} {:.3f} {:.3f}), test colour ({:.3f} "
                     "{:.3f} {:.3f}), {} light set(s), ends {:.1f} {:.1f} {:.1f} {:.1f} (dword 83 "
                     "{:.1f}), tile mids ({:.2f} {:.2f}) ({:.2f} {:.2f}) ({:.2f} {:.2f}) ({:.2f} "
                     "{:.2f}), atlas {}x{} sign {}, steps {}, density {}, half shift {:.4f}",
                     towards[0], towards[1], towards[2], colour[0], colour[1], colour[2],
                     sets, ends[0], ends[1], ends[2], ends[3], c[83], mids[0][0],
                     mids[0][1], mids[1][0], mids[1][1], mids[2][0], mids[2][1], mids[3][0],
                     mids[3][1], vr_moon.atlas_width, vr_moon.atlas_height, vr_moon.compare_sign,
                     vr_moon.steps, vr_moon.density, half);
        }
    }
    return bytes;
}

void Rasterizer::VrMoonEndFrame() {
    const bool upscaler = VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off;
    const Knobs& knobs = GetKnobs();
    int quality = VR::GetSettings().moon_shafts_quality;
    if (knobs.quality) {
        quality = std::atoi(knobs.quality);
    }
    quality = std::clamp(quality, 0, 2);
    std::scoped_lock lock{vr_moon.mutex};
    ++vr_moon.frame;
    // The atlas unseen for AtlasStaleFrames (indoors, a menu, a loading screen): let go of it, so
    // the composite stops binding (and keeping alive, or bringing back) a 64 MB image the game
    // may have freed. (Its pipelines specialise on a null image either way: no recompile.)
    if (vr_moon.atlas_frame != ~0u && vr_moon.frame - vr_moon.atlas_frame > Moon::AtlasStaleFrames) {
        Shader::SetVrHostImage(Shader::VrHostImage::MoonAtlas, AmdGpu::Image{});
        vr_moon.atlas_frame = ~0u;
    }
    vr_moon.density = knobs.density;
    vr_moon.steps = Moon::QualitySteps[quality];
    // The session's rule as the pipeline cache's (an upscaler turned on or off in game).
    vr_moon.on = Moon::Enabled(VR::IsEnabled(), VR::MoonGlowOn(VR::GetSettings()), upscaler,
                               knobs.shafts);
}

} // namespace Vulkan
