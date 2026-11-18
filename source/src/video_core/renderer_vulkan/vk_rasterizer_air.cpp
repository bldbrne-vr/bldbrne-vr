// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The air pass's side of the rasterizer (shader_recompiler/ir/passes/air_light.h): what the
// moon's shadow pass and the light composite draw into and read (VrAirCapture), the parameters of
// the two host passes (VrAirUpload: this draw's part, each eye's ndc-to-view matrix, the cells'
// size, the moon's colour), the cells' buffer, the colour probe's buffer and its reading
// (VrAirFrame), and the wait before the scene's copy reads the cells (VrAirBeforeDraw).

#include "video_core/native_stereo/draw_memory.h"
#include "video_core/native_stereo/draw_registers.h"
#include "video_core/native_stereo/lighting_reconstruction.h"
#include "video_core/native_stereo/native_stereo.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"
#include "core/vr/game_area.h"
#include "shader_recompiler/ir/passes/fog_patches.h"
#include "core/vr/live_tune.h"
#include "core/vr/moon_screen.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/bounce_light.h"
#include "shader_recompiler/ir/passes/fog_grid.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

namespace Air = Shader::Optimization::AirLight;
namespace Moon = Shader::Optimization::MoonShafts;

// The draw's first image of its own (the picture a blur or a copy reads): its guest address.
u64 FirstInput(const Shader::Info& stage) {
    for (const auto& desc : stage.images) {
        if (desc.IsVrHost()) {
            continue;
        }
        const AmdGpu::Image sharp = desc.GetSharp(stage);
        return sharp ? sharp.Address() : 0;
    }
    return 0;
}

// A 4x4 matrix's inverse (rows; Gauss-Jordan with pivoting): false when it has none.
bool Invert4(const std::array<float, 16>& m, std::array<double, 16>& out) {
    std::array<std::array<double, 8>, 4> a{};
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            a[r][c] = m[4 * r + c];
        }
        a[r][4 + r] = 1.0;
    }
    for (u32 c = 0; c < 4; ++c) {
        u32 pivot = c;
        for (u32 r = c + 1; r < 4; ++r) {
            if (std::abs(a[r][c]) > std::abs(a[pivot][c])) {
                pivot = r;
            }
        }
        if (!(std::abs(a[pivot][c]) > 1e-12)) {
            return false;
        }
        std::swap(a[c], a[pivot]);
        const double inv = 1.0 / a[c][c];
        for (double& v : a[c]) {
            v *= inv;
        }
        for (u32 r = 0; r < 4; ++r) {
            if (r != c) {
                const double k = a[r][c];
                for (u32 j = 0; j < 8; ++j) {
                    a[r][j] -= k * a[c][j];
                }
            }
        }
    }
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            out[4 * r + c] = a[r][4 + c];
            if (!std::isfinite(out[4 * r + c])) {
                return false;
            }
        }
    }
    return true;
}

// What the air pass did over the last second (logs): the scene copy's draws, how many had this
// frame's cells, the computes by host, and why a compute host left without computing.
struct AirDiag {
    u32 applies{}, with_cells{}, with_grid{}, half{}, blur{};
    u32 no_shadow{}, stale_cascades{}, mirror{}, no_moon{}, bad_view{}, not_host{};
    // The scene copy's own: left with nothing (no shadow pass near, stale cascades, not the scene's
    // picture), and how often its record was not the shadow pass's / the composite's (the old rule
    // would have left then), with the widest gap seen.
    u32 apply_no_shadow{}, apply_stale{}, apply_no_scene{}, apply_other_shadow{}, apply_other_scene{};
    u64 widest_gap{};
    u64 first_frame{};
    std::chrono::steady_clock::time_point since{std::chrono::steady_clock::now()};
};
AirDiag g_air_diag;

// Tests only: VR_AIR_COLOUR=r,g,b instead of the probe's.
const std::array<float, 3>& TestColour() {
    static const std::array<float, 3> colour = [] {
        std::array<float, 3> c{-1.0f, -1.0f, -1.0f};
        if (const char* v = std::getenv("VR_AIR_COLOUR")) {
            std::sscanf(v, "%f,%f,%f", &c[0], &c[1], &c[2]);
        }
        return c;
    }();
    return colour;
}

} // namespace

// At the moon's shadow pass: the eye depth it reads (the host image of the two air passes) and
// the picture it draws (the blur that reads it computes the cells). At the light composite: the
// scene's picture (the copy that reads it applies them).
void Rasterizer::VrAirCapture(const Shader::Info& stage) {
    if (!Air::Pass() || VrInMirrorPass()) {
        return;
    }
    const u64 frame = VR::DrawFrameRecord().id;
    const u64 target = ScratchDraw::Registers(liverpool).color_buffers[0].Address();
    if (Shader::Optimization::BounceLight::IsCompositeProgram(stage.pgm_hash)) {
        vr_air.scene_target = target;
        vr_air.scene_frame = frame;
        return;
    }
    for (const auto& desc : stage.images) {
        if (desc.is_depth || desc.IsVrHost()) {
            continue; // the atlas (a compare image), ours
        }
        const AmdGpu::Image sharp = desc.GetSharp(stage);
        if (!sharp) {
            continue;
        }
        if (Shader::SetVrHostImage(Shader::VrHostImage::AirDepth, sharp)) {
            LOG_INFO(Render_Vulkan, "VR air pass: the eye depth {:#x} {}x{}", sharp.Address(),
                     u32(sharp.width + 1), u32(sharp.height + 1));
        }
        vr_air.shadow_target = target;
        vr_air.shadow_frame = frame;
        return;
    }
}

// The reflections pass's buffer (ssr320): its rows 32..43, the frame's view to world as the light
// pass's light208 has them (compared in the first frames: the log says how far apart they are).
void Rasterizer::VrAirCaptureView(const float* source, u32 size, bool guest) {
    if (size < 320 || VrInMirrorPass()) {
        return;
    }
    std::array<float, 80> values;
    std::memcpy(values.data(), guest ? ScratchDraw::MemorySource(source, 320) : source, 320);
    for (u32 d = 32; d < 44; ++d) {
        if (!std::isfinite(values[d])) {
            return;
        }
    }
    // Its rows 32..42 turn the G-buffer's world normals into the view: world to view. Kept the
    // other way round, view to world, as the lamp memory's rows are (the transpose). (Until
    // 2026-11-14 they were taken as they are: right for a level head, the inverse otherwise: the
    // mist's layer tilted with the head's pitch and its banks turned against its yaw.)
    for (u32 r = 0; r < 3; ++r) {
        for (u32 c = 0; c < 3; ++c) {
            vr_air.view[4 * r + c] = values[32 + 4 * c + r];
        }
        vr_air.view[4 * r + 3] = 0.0f;
    }
    vr_air.view_frame = VR::DrawFrameRecord().id;
    if constexpr (Common::Log::Enabled) {
        static u32 logs = 0, calls = 0;
        if (logs < 24 && (logs < 2 || ++calls % 900 == 0)) {
            ++logs;
            std::array<float, 12> fog;
            {
                std::scoped_lock lock{vr_fog.mutex};
                fog = vr_fog.view;
            }
            LOG_INFO(Render_Vulkan,
                     "VR air pass: the reflections pass's rows ({:.3f} {:.3f} {:.3f} {:.2f} | {:.3f} {:.3f} {:.3f} "
                     "{:.2f} | {:.3f} {:.3f} {:.3f} {:.2f}), the light pass's ({:.3f} {:.3f} {:.3f} {:.2f} | {:.3f} "
                     "{:.3f} {:.3f} {:.2f} | {:.3f} {:.3f} {:.3f} {:.2f})",
                     vr_air.view[0], vr_air.view[1], vr_air.view[2], vr_air.view[3], vr_air.view[4], vr_air.view[5],
                     vr_air.view[6], vr_air.view[7], vr_air.view[8], vr_air.view[9], vr_air.view[10], vr_air.view[11],
                     fog[0], fog[1], fog[2], fog[3], fog[4], fog[5],
                     fog[6], fog[7], fog[8], fog[9], fog[10], fog[11]);
        }
    }
}

// The colour probe (air_light.h), once a frame: the sums of ProbeReadBehind frames ago, eased
// into the air's colour (held while no surface gave any), the slot a frame ahead cleared.
void Rasterizer::VrAirFrame(u64 frame) {
    if (frame == vr_air.probe_frame) {
        return;
    }
    vr_air.probe_frame = frame;
    ++vr_air.probe_counter;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - vr_air.probe_time).count(), 0.0f, 10.0f);
    vr_air.probe_time = now;
    // The blood moon's share of the air's colour (air_light.h BloodMoon), eased; at once on the
    // first frame.
    {
        namespace Blood = Air::Mist::BloodMoon;
        const float target = Blood::ColourNow()[3] > 0.0f && VR::GameArea::BloodMoon() ? 1.0f : 0.0f;
        const float before = vr_air.blood_moon;
        vr_air.blood_moon = vr_air.probe_counter <= 1 ? target : before + (target - before) * (1.0f - std::exp(-dt / Blood::EaseS));
        if constexpr (Common::Log::Enabled) {
            if ((before < 0.5f) != (vr_air.blood_moon < 0.5f)) {
                LOG_INFO(Render_Vulkan, "VR air pass: the blood moon {}: the air takes {}", target > 0.5f ? "is up here" : "is not up here",
                         target > 0.5f ? "the night's own colour" : "the probe's colour again");
            }
        }
    }
    if (!vr_air.probe) {
        return;
    }
    auto* const words = reinterpret_cast<u32*>(vr_air.probe->mapped_data.data());
    const u32 read = (vr_air.probe_counter + Air::ProbeSlots - Air::ProbeReadBehind) % Air::ProbeSlots;
    std::array<u32, Air::ProbeDwords> sums;
    std::memcpy(sums.data(), words + read * Air::ProbeSlotDwords, sizeof(sums));
    // The lights' source votes of that frame (air_light.h Sources), for VrFogUpload to count.
    std::memcpy(vr_air.votes.data(), words + read * Air::ProbeSlotDwords + Air::ProbeDwords,
                sizeof(vr_air.votes));
    vr_air.votes_list = vr_air.source_lists[read];
    vr_air.votes_pending = true;
    // ... and its brightest sky fragment (MoonAim), with the camera it was drawn with.
    vr_air_aim.read_word = words[read * Air::ProbeSlotDwords + Air::MoonAimDword];
    vr_air_aim.read_camera = vr_air_aim.cameras[read];
    vr_air_aim.pending = true;
    const float weight = float(sums[3]) / Air::ProbeWeightScale;
    if (weight > 1e-3f) {
        std::array<float, 3> colour;
        for (u32 k = 0; k < 3; ++k) {
            colour[k] = float(sums[k]) / Air::ProbeColourScale / weight;
        }
        // (It holds: up over ProbeUpS, down over ProbeDownS, by brightness: air_light.h.)
        for (u32 k = 0; k < 3; ++k) {
            colour[k] = std::isfinite(colour[k]) ? std::clamp(colour[k], 0.0f, 1024.0f) : 0.0f;
        }
        // (No brighter than ProbeLevel, the hue kept.)
        if (const float top = std::max({colour[0], colour[1], colour[2]}); top > Air::ProbeLevel) {
            for (float& v : colour) {
                v *= Air::ProbeLevel / top;
            }
        }
        const auto lum = [](const std::array<float, 3>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
        const float ease = lum(colour) >= lum(vr_air.colour) ? Air::ProbeUpS : Air::ProbeDownS;
        const float share = vr_air.colour_valid ? 1.0f - std::exp(-dt / ease) : 1.0f;
        for (u32 k = 0; k < 3; ++k) {
            vr_air.colour[k] += (colour[k] - vr_air.colour[k]) * share;
        }
        vr_air.colour_valid = true;
    }
    const u32 clear = (vr_air.probe_counter + 1) % Air::ProbeSlots;
    std::memset(words + clear * Air::ProbeSlotDwords, 0, Air::ProbeSlotDwords * sizeof(u32));
    vr_air.source_lists[clear].count = 0;
    vr_air_aim.cameras[clear].valid = false;
    if constexpr (Common::Log::Enabled) {
        if (vr_air.probe_counter % 600 == 5) {
            LOG_INFO(Render_Vulkan,
                     "VR air pass: the moon's colour ({:.3f} {:.3f} {:.3f}){}, last sums' weight {:.4f}; "
                     "cells {}x{} an eye",
                     vr_air.colour[0], vr_air.colour[1], vr_air.colour[2],
                     vr_air.colour_valid ? "" : " (none yet)", weight, vr_air.cells_w, vr_air.cells_h);
        }
    }
}

// The moon's buffer for the two programs of the air pass that read it (after VrMoonUpload): the
// blur marches with a white colour (its cells keep the amount), the composite's probe gets its
// slot.
void Rasterizer::VrAirMoon(std::span<u32> moon, u64 pgm_hash) {
    if (!Air::Pass()) {
        return;
    }
    // (Both compute hosts: the half-size one too. It was left out, 2026-11-14: its cells then held
    // the mist only, the moonlight's own air times a colour of 0.)
    if (Air::IsComputeProgram(pgm_hash) || Air::IsHalfProgram(pgm_hash)) {
        for (u32 k = 0; k < 3; ++k) {
            moon[12 + k] = std::bit_cast<u32>(1.0f);
        }
        return;
    }
    VrAirProbeBuffer();
    VrAirFrame(VR::DrawFrameRecord().id);
    moon[23] = vr_air.probe_counter % Air::ProbeSlots;
}

// The list of lights for the scene copy (after VrFogUpload): each light's reach in the air
// wider by its share (air_light.h Mist::LampWide; reach is 1 / radius^2).
void Rasterizer::VrAirWiden(std::span<u32> fog, u64 pgm_hash) {
    namespace Fog = Shader::Optimization::FogLight;
    if (!Air::IsApplyProgram(pgm_hash) || !Air::Mist::Enabled()) {
        return;
    }
    const u32 count = std::min({fog[0] & 0xffffu, vr_air.widen_count, Fog::MaxLights});
    for (u32 k = 0; k < count; ++k) {
        const float wide = vr_air.widen[k];
        if (wide > 1.0f) {
            u32& reach = fog[Fog::HeaderDwords + Fog::LightDwords * k + 3];
            reach = std::bit_cast<u32>(std::bit_cast<float>(reach) / (wide * wide));
        }
    }
}

void Rasterizer::VrAirProbeBuffer() {
    if (vr_air.probe) {
        return;
    }
    const u64 bytes = u64(Air::ProbeSlots) * Air::ProbeSlotDwords * sizeof(u32);
    vr_air.probe = std::make_unique<VideoCore::Buffer>(instance, scheduler, VideoCore::MemoryUsage::Stream, 0,
                                                       VideoCore::AllFlags, bytes);
    std::memset(vr_air.probe->mapped_data.data(), 0, vr_air.probe->mapped_data.size());
}

u32 Rasterizer::VrAirUpload(std::span<u32> out, const Shader::Info& stage) {
    std::ranges::fill(out, 0u);
    const u32 bytes = Air::ParamDwords * sizeof(u32);
    const auto put = [&](u32 dword, float v) { out[dword] = std::bit_cast<u32>(v); };
    out[1] = 1;
    out[2] = 1;
    // The identity: harmless numbers while nothing is drawn.
    for (u32 eye = 0; eye < 2; ++eye) {
        for (u32 d = 0; d < 4; ++d) {
            put(Air::MatrixDword + 16 * eye + 5 * d, 1.0f);
        }
    }
    put(7, 1.0f);
    put(12, 1.0f);
    const bool compute_host = Air::IsComputeProgram(stage.pgm_hash) || Air::IsHalfProgram(stage.pgm_hash);
    if (!Air::Pass() || !VR::IsEnabled() || VrInMirrorPass()) {
        g_air_diag.mirror += compute_host && VrInMirrorPass();
        return bytes;
    }
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    const u64 frame = rec.id;
    if (!compute_host && vr_air.shadow_frame != ~0ull && vr_air.shadow_frame != frame) {
        ++g_air_diag.apply_other_shadow;
        g_air_diag.widest_gap = std::max(g_air_diag.widest_gap, frame >= vr_air.shadow_frame
                                                                    ? frame - vr_air.shadow_frame
                                                                    : vr_air.shadow_frame - frame);
    }
    if (frame == 0 || !Air::FrameNear(vr_air.shadow_frame, frame)) {
        g_air_diag.no_shadow += compute_host;
        g_air_diag.apply_no_shadow += !compute_host;
        return bytes; // no moon's shadow pass round this frame: no depth, no camera
    }
    // The camera: the flat one's cascades (this frame's), each eye's inverse projection.
    std::array<float, 84> cascades;
    bool fresh;
    {
        std::scoped_lock lock{vr_moon.mutex};
        cascades = vr_moon.cascades;
        fresh = vr_moon.cascades_frame != ~0u &&
                vr_moon.frame - vr_moon.cascades_frame <= static_cast<u32>(Air::FrameSlack);
    }
    if (!fresh) {
        g_air_diag.stale_cascades += compute_host;
        g_air_diag.apply_stale += !compute_host;
        return bytes;
    }
    std::array<std::array<float, 16>, 2> inverse;
    NativeStereo::EyeInverseProjections(cascades.data(), rec, inverse);
    for (u32 eye = 0; eye < 2; ++eye) {
        for (u32 d = 0; d < 16; ++d) {
            if (!std::isfinite(inverse[eye][d])) {
                return bytes;
            }
            put(Air::MatrixDword + 16 * eye + d, inverse[eye][d]);
        }
    }
    // ndc from the fragment's coordinates: the draw's viewport, backwards.
    const auto& vp = ScratchDraw::Registers(liverpool).viewports[0];
    if (!(std::abs(vp.xscale) > 0.5f) || !(std::abs(vp.yscale) > 0.5f)) {
        return bytes;
    }
    put(8, 1.0f / vp.xscale);
    put(9, 1.0f / vp.yscale);
    put(10, -vp.xoffset / vp.xscale);
    put(11, -vp.yoffset / vp.yscale);
    const u32 width = static_cast<u32>(std::lround(2.0f * std::abs(vp.xscale)));
    const u32 height = static_cast<u32>(std::lround(2.0f * std::abs(vp.yscale)));
    // The cells: half the picture's size (a half-size host's own size).
    const bool half_host = Air::IsHalfProgram(stage.pgm_hash);
    const u32 cw = std::clamp(half_host ? width : (width + 1) / 2, 1u, 4096u);
    const u32 ch = std::clamp(half_host ? height : (height + 1) / 2, 1u, 4096u);
    static const float far_m = [] {
        const char* v = std::getenv("VR_MOON_SHAFTS_FAR");
        const float x = v ? std::strtof(v, nullptr) : Moon::FarM;
        return std::isfinite(x) ? std::clamp(x, 5.0f, 500.0f) : Moon::FarM;
    }();
    put(7, std::max(far_m * Moon::FarFade, Air::Mist::Enabled() ? Air::Mist::FarM : 0.0f));
    put(12, Air::SkyM);
    put(Air::SlotDword + 1, 1.0f / float(std::max(width, 1u)));
    put(Air::SlotDword + 2, 1.0f / float(std::max(height, 1u)));
    put(13, std::clamp(VR::Tune::Get("lighting.air_sky", 1.0f), 0.0f, 1.0f));
    put(Air::GlowKneeDword, std::clamp(VR::Tune::Get("lighting.glow_knee", Air::GlowKnee), 0.0f, 64.0f));
    const u64 input = FirstInput(stage);
    const bool moon = vr_moon.on.load(std::memory_order_relaxed);
    if (Air::IsComputeProgram(stage.pgm_hash) || half_host) {
        if (!moon) {
            ++g_air_diag.no_moon;
            return bytes;
        }
        if (half_host) {
            vr_air.half_frame = frame;
            ++g_air_diag.half;
        } else if (input == 0 || input != vr_air.shadow_target ||
                   Air::FrameNear(vr_air.half_frame, frame, Air::HalfHostFrames)) {
            ++g_air_diag.not_host;
            return bytes; // not the mask's first blur, or the half-size host computes
        } else {
            ++g_air_diag.blur;
        }
        // The cells: made (or made anew at another size) here, before this draw binds them.
        // (Not with the fog grid, fog_grid.h: nothing marches them any more. This host still
        // works the mist's numbers out below, for the grid and the scene's copy.)
        const bool grid = Shader::Optimization::FogGrid::Enabled();
        const u64 words = u64(cw) * ch * 2;
        if (!grid && (!vr_air.cells || vr_air.cells_words != words)) {
            if (vr_air.cells) {
                scheduler.DeferOperation([old = std::move(vr_air.cells)]() mutable { old.reset(); });
            }
            vr_air.cells = std::make_unique<VideoCore::Buffer>(instance, scheduler,
                                                               VideoCore::MemoryUsage::DeviceLocal, 0,
                                                               VideoCore::AllFlags, words * sizeof(u32));
            vr_air.cells_words = words;
            vr_air.cells_frame = ~0ull;
            LOG_INFO(Render_Vulkan, "VR air pass: cells {}x{} an eye ({} KiB)", cw, ch,
                     (words * sizeof(u32)) >> 10);
        }
        if (!grid) {
            vr_air.cells_w = cw;
            vr_air.cells_h = ch;
            vr_air.cells_frame = frame;
        }
        // The mist (air_light.h Mist): this frame's view to world (the reflections pass's when it
        // is this frame's and holds a rotation; else the lamp memory's, a frame old), the eye's
        // height over the ground (the hunter's feet, eased), the time, the knobs.
        if (Air::Mist::Enabled()) {
            namespace Mist = Air::Mist;
            std::array<float, 12> view;
            bool view_ok = false;
            {
                std::scoped_lock lock{vr_fog.mutex};
                view = vr_fog.view;
                view_ok = vr_fog.view_frame != 0;
            }
            if (Air::FrameNear(vr_air.view_frame, frame, 1)) {
                // (Its rotation only when it is one: the camera's place from the light pass's.)
                float worst = 0.0f;
                for (u32 r = 0; r < 3; ++r) {
                    const float len = vr_air.view[4 * r] * vr_air.view[4 * r] + vr_air.view[4 * r + 1] * vr_air.view[4 * r + 1] +
                                      vr_air.view[4 * r + 2] * vr_air.view[4 * r + 2];
                    worst = std::max(worst, std::abs(len - 1.0f));
                }
                if (worst < 0.01f) {
                    for (u32 r = 0; r < 3; ++r) {
                        for (u32 c = 0; c < 3; ++c) {
                            view[4 * r + c] = vr_air.view[4 * r + c];
                        }
                    }
                    view_ok = true;
                }
            }
            // The camera's place: the hunter's own (his feet, every frame) with the eye 1.6 m up.
            // (The lamp memory's camera is only as new as the last light drawn: looking at the sky
            // or walking where no lamp is in view it stood still, and the mist's banks with it,
            // then jumped. The banks are tens of metres across: the head's own few decimetres do
            // not matter to them.)
            // (Third person, or no body: the frame has no place of his; the game's loop's own
            // then, game_area.h Hunter. Without it the mist's ground was the camera's height less
            // 1.6 m, and went up and down with the camera's zoom.)
            std::array<float, 3> hunter{};
            const bool hunter_known = !rec.has_player_position && VR::GameArea::Hunter(hunter, 500);
            if (rec.has_player_position) {
                view[3] = rec.player_position.x;
                view[7] = rec.player_position.y + 1.6f;
                view[11] = rec.player_position.z;
            } else if (hunter_known) {
                view[3] = hunter[0];
                view[7] = hunter[1] + 1.6f;
                view[11] = hunter[2];
            }
            // The hunter's place (his feet, the eye 1.6 m up) is where the fog's own things are
            // centred: the patches picked, the world volume's box, his push. It is NOT where the
            // fog is seen from. Until 2026-11-18 it was that too: the rays started at his place
            // while the picture was the camera's, so all the fog stood off from the world by the
            // camera's own offset from him: with the head's height and the stairs' smoothing in
            // first person, with the whole orbit in third (the tester: "fog height is mapped to
            // camera, not to the emitter, so if I climb stairs quickly or move the camera up and
            // down our fog patches also move"). The frame's own camera now, where it has one:
            // the very place the frame is drawn from, so with one eye a frame (stereo_mode 1) that
            // eye's own, as the fog grid's history takes it (VrFogGridParams' place). The other
            // modes draw from the head's centre: the same place.
            const std::array<float, 3> place{view[3], view[7], view[11]};
            if (rec.has_render_position) {
                view[3] = rec.render_position.x;
                view[7] = rec.render_position.y;
                view[11] = rec.render_position.z;
            }
            vr_air.mist_place = place;
            const auto now = std::chrono::steady_clock::now();
            const float feet = rec.has_player_position ? rec.player_position.y : hunter_known ? hunter[1] : place[1] - 1.6f;
            const float dt = std::clamp(std::chrono::duration<float>(now - vr_air.ground_time).count(), 0.0f, 1.0f);
            vr_air.ground_time = now;
            if (!vr_air.ground_valid || std::abs(feet - vr_air.ground) > 8.0f) {
                vr_air.ground = feet; // the first frame, or a warp
            } else {
                vr_air.ground += (feet - vr_air.ground) * (1.0f - std::exp(-dt / Mist::GroundEaseS));
            }
            vr_air.ground_valid = true;
            // Under a roof it thins out (Mist::Indoors): no sky seen for a while.
            float open_sky = 1.0f;
            if (Air::MoonAim()) {
                const s64 now_ms = VR::MoonScreen::NowMs();
                const bool open = vr_air_aim.sky_seen_ms >= 0 &&
                                  now_ms - vr_air_aim.sky_seen_ms <= static_cast<s64>(Mist::IndoorsAfterS * 1000.0f);
                const float indoors = std::clamp(VR::Tune::Get("lighting.mist_indoors", Mist::Indoors), 0.0f, 1.0f);
                const float target = open ? 1.0f : indoors;
                // (Back at once-ish when the sky returns: a quarter of the way out's time.)
                const float ease = target > vr_air_aim.open_sky ? 0.25f * Mist::IndoorsEaseS : Mist::IndoorsEaseS;
                vr_air_aim.open_sky += (target - vr_air_aim.open_sky) * (1.0f - std::exp(-dt / ease));
                open_sky = std::clamp(vr_air_aim.open_sky, 0.0f, 1.0f);
            }
            // By the game's area (air_light.h Mist::AreaShare), eased; none with vr.ini ground_mist off.
            {
                const float forced = VR::Tune::Get("lighting.mist_area", -1.0f);
                const float want = !VR::GetSettings().ground_mist ? 0.0f
                                   : forced >= 0.0f               ? std::min(forced, 1.0f)
                                                                  : Mist::AreaShare(VR::GameArea::Area());
                if (!vr_air.area_valid) {
                    vr_air.area_share = want;
                    vr_air.area_valid = true;
                } else {
                    vr_air.area_share += (want - vr_air.area_share) * (1.0f - std::exp(-dt / Mist::AreaEaseS));
                }
            }
            // (More of it in the Shadows of Yharnam's room: fog_patches.h ShadowsRoom.)
            namespace Room = Shader::Optimization::FogPatches::ShadowsRoom;
            const float room =
                Room::Share(VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16, place[0], place[1], place[2]);
            const float density =
                view_ok ? open_sky * vr_air.area_share *
                              std::clamp(VR::Tune::Get("lighting.mist", Mist::Density), 0.0f, 2.0f) *
                              (1.0f + (Room::HazeNow() - 1.0f) * room)
                        : 0.0f;
            const float height = std::clamp(VR::Tune::Get("lighting.mist_height", Mist::HeightM), 0.2f, 30.0f);
            for (u32 d = 0; d < 12; ++d) {
                put(Air::ViewDword + d, view[d]);
            }
            const u32 m = Air::MistDword;
            put(m + 0, Shader::Optimization::FogPatches::ClockPin() >= 0.0f
                           ? Shader::Optimization::FogPatches::ClockPin()
                           : std::fmod(std::chrono::duration<float>(now - vr_air.start_time).count(), 4096.0f));
            put(m + 1, density);
            put(m + 2, 1.0f / height);
            put(m + 3, std::clamp(view[7] - vr_air.ground, -20.0f, 60.0f));
            put(m + 4, std::clamp(VR::Tune::Get("lighting.mist_far", Mist::FarM), 5.0f, 300.0f));
            put(m + 5, Mist::Ambient);
            put(m + 6, Mist::Forward);
            put(m + 7, Mist::NoiseScale);
            put(m + 8, Mist::WindX);
            put(m + 9, Mist::WindY);
            put(m + 10, Mist::WindZ);
            put(m + 11, Mist::Extinction);
            put(m + 12, Mist::NearStartM);
            put(m + 13, Mist::NearFullM);
            put(m + 14, std::clamp(VR::Tune::Get("lighting.mist_patchy", Mist::Patchy), 0.0f, 1.0f));
            put(m + 15, std::clamp(VR::Tune::Get("lighting.mist_strength", Mist::Strength), 0.0f, 4.0f));
            put(Mist::HighDword, std::clamp(VR::Tune::Get("lighting.mist_high", Mist::High), 0.0f, 1.0f));
            put(Mist::HazeDword,
                1.0f / std::clamp(VR::Tune::Get("lighting.haze_height", Mist::HazeHeightM), 1.0f, 500.0f));
            put(Mist::HazeDword + 1, std::clamp(VR::Tune::Get("lighting.haze_far", Mist::HazeFarM), 0.0f, 4000.0f));
            put(Mist::HazeDword + 2, std::clamp(VR::Tune::Get("lighting.haze_glow", Mist::HazeGlow), 0.0f, 16.0f));
            // The moon's rays (air_light.h Rays): the moon the finder holds, through this frame's
            // camera, onto each eye's screen; its weight fades as it leaves the view.
            if (Air::Rays::Enabled()) {
                namespace Rays = Air::Rays;
                const float strength = std::clamp(VR::Tune::Get("lighting.moon_rays", Rays::Strength), 0.0f, 8.0f);
                const u32 at = Air::RaysDword;
                put(at + 8, strength);
                put(at + 10, Rays::MaxNdc);
                put(at + 11, Rays::NearM);
                const s64 now_ms = VR::MoonScreen::NowMs();
                if (strength > 0.0f && view_ok && vr_air_aim.moon_seen_ms >= 0 &&
                    now_ms - vr_air_aim.moon_seen_ms <= Moon::GlowHoldMs) {
                    std::array<double, 3> g{};
                    for (u32 c = 0; c < 3; ++c) {
                        for (u32 r = 0; r < 3; ++r) {
                            g[c] += double(view[4 * r + c]) * vr_air_aim.moon_world[r];
                        }
                    }
                    const double gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                    const auto smooth = [](double e0, double e1, double x) {
                        const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
                        return t * t * (3.0 - 2.0 * t);
                    };
                    std::array<float, 6> seen{};
                    for (u32 eye = 0; eye < 2 && gl > 1e-9 && std::isfinite(gl); ++eye) {
                        std::array<double, 16> p;
                        if (!Invert4(inverse[eye], p)) {
                            continue;
                        }
                        // The direction as a point at infinity; in front when its w has the sign of
                        // the view's own ahead (0 0 1).
                        const double x = (p[0] * g[0] + p[1] * g[1] + p[2] * g[2]) / gl;
                        const double y = (p[4] * g[0] + p[5] * g[1] + p[6] * g[2]) / gl;
                        const double w = (p[12] * g[0] + p[13] * g[1] + p[14] * g[2]) / gl;
                        if (!(w * (p[14] < 0.0 ? -1.0 : 1.0) > 1e-6)) {
                            continue;
                        }
                        const double nx = x / w, ny = y / w;
                        const double weight = smooth(0.1, 0.35, g[2] / gl) *
                                              (1.0 - smooth(1.5, 3.0, std::max(std::abs(nx), std::abs(ny))));
                        if (!std::isfinite(nx) || !std::isfinite(ny)) {
                            continue;
                        }
                        put(at + 4 * eye, static_cast<float>(std::clamp(nx, -60.0, 60.0)));
                        put(at + 4 * eye + 1, static_cast<float>(std::clamp(ny, -60.0, 60.0)));
                        put(at + 4 * eye + 2, static_cast<float>(weight));
                        seen[3 * eye] = static_cast<float>(nx);
                        seen[3 * eye + 1] = static_cast<float>(ny);
                        seen[3 * eye + 2] = static_cast<float>(weight);
                    }
                    if constexpr (Common::Log::Enabled) {
                        static u32 logs = 0;
                        if (logs < 30 && vr_air.probe_counter % 300 == 11) {
                            ++logs;
                            LOG_INFO(Render_Vulkan,
                                     "VR air pass: the moon's rays: the moon on the screen ({:.2f} {:.2f}) weight {:.2f} | "
                                     "({:.2f} {:.2f}) weight {:.2f}; in view ({:.3f} {:.3f} {:.3f})",
                                     seen[0], seen[1], seen[2], seen[3], seen[4], seen[5], g[0], g[1], g[2]);
                        }
                    }
                }
            }
            if constexpr (Common::Log::Enabled) {
                static u32 logs = 0;
                if (logs < 20 && vr_air.probe_counter % 600 == 9) {
                    ++logs;
                    LOG_INFO(Render_Vulkan,
                             "VR air pass: mist density {:.3f}, the eye {:.2f} m over the ground ({:.2f}), camera at "
                             "({:.1f} {:.1f} {:.1f}), up in view ({:.3f} {:.3f} {:.3f}){}",
                             density, view[7] - vr_air.ground, vr_air.ground, view[3], view[7], view[11], view[4],
                             view[5], view[6], vr_air.view_frame == frame ? "" : " (the lamp memory's camera)");
                }
            }
        }
        // The mist's numbers go to the scene copy too (it brings each cell to its pixel's own
        // distance and works the distance haze out: without them it had zeros, 2026-11-14).
        if (Air::Mist::Enabled()) {
            namespace Mist = Air::Mist;
            std::copy_n(out.begin() + Air::ViewDword, 12, vr_air.mist_carry.begin());
            std::copy_n(out.begin() + Air::MistDword, 16, vr_air.mist_carry.begin() + 12);
            vr_air.mist_carry[28] = out[Mist::HighDword];
            std::copy_n(out.begin() + Mist::HazeDword, 3, vr_air.mist_carry.begin() + 29);
            vr_air.mist_carry_frame = frame;
            vr_air.mist_place_frame = frame;
        }
        if (grid) {
            return bytes; // (its permutations have no march: nothing reads the flag)
        }
        out[0] = Air::FlagCompute;
        out[1] = cw;
        out[2] = ch;
        out[3] = cw * ch;
        return bytes;
    }
    g_air_diag.apply_other_scene += vr_air.scene_frame != ~0ull && vr_air.scene_frame != frame;
    if (input == 0 || !Air::FrameNear(vr_air.scene_frame, frame) || input != vr_air.scene_target) {
        g_air_diag.apply_no_scene += input != 0 && input == vr_air.scene_target;
        return bytes;
    }
    u32 flags = Air::FlagApply;
    // The mist's numbers of this frame's compute host (or the host change-over's last).
    if (Air::Mist::Enabled() && Air::FrameNear(vr_air.mist_carry_frame, frame, Air::HalfHostFrames + 1)) {
        namespace Mist = Air::Mist;
        std::copy_n(vr_air.mist_carry.begin(), 12, out.begin() + Air::ViewDword);
        std::copy_n(vr_air.mist_carry.begin() + 12, 16, out.begin() + Air::MistDword);
        out[Mist::HighDword] = vr_air.mist_carry[28];
        std::copy_n(vr_air.mist_carry.begin() + 29, 3, out.begin() + Mist::HazeDword);
    }
    if (vr_fog.on) {
        flags |= Air::FlagHalos;
        if (Air::Sources()) {
            flags |= Air::FlagSources;
        }
    }
    out[Air::SlotDword] = vr_air.probe_counter % Air::ProbeSlots;
    if (moon && Air::MoonAim()) {
        // The moon's place: this frame's camera, for the word read back later.
        flags |= Air::FlagMoonAim;
        auto& camera = vr_air_aim.cameras[vr_air.probe_counter % Air::ProbeSlots];
        {
            // The frame's view to world (the lamp memory's: the light pass's camera).
            std::scoped_lock lock{vr_fog.mutex};
            camera.valid = vr_fog.view_frame != 0;
            camera.view = vr_fog.view;
        }
        // ... this frame's own where the reflections pass gave it: the lamp memory's is only as
        // new as the last light drawn (looking up at the moon none is: its rows were the last
        // level look's, and the moon came out 19 degrees up where it stands near 65).
        if (Air::FrameNear(vr_air.view_frame, frame, 1)) {
            camera.view = vr_air.view;
            camera.valid = true;
        }
        camera.inverse = inverse[0];
        camera.ndc = {1.0f / vp.xscale, 1.0f / vp.yscale, -vp.xoffset / vp.xscale, -vp.yoffset / vp.yscale};
    }
    // The cells: this frame's, or the last ones' through a host change-over (the fallback host
    // only starts after HalfHostFrames frames without the half-size one: a few frames old beats
    // frames with no mist at all).
    if (moon && vr_air.cells && Air::FrameNear(vr_air.cells_frame, frame, Air::HalfHostFrames + 1)) {
        flags |= Air::FlagCells;
        out[1] = vr_air.cells_w;
        out[2] = vr_air.cells_h;
        out[3] = vr_air.cells_w * vr_air.cells_h;
    }
    // The fog grid (fog_grid.h): built just before this draw (VrFogGridBeforeDraw), or a frame or
    // two ago (its buffer keeps the last one built: an old mist beats a frame without any).
    if (Shader::Optimization::FogGrid::Enabled() && Air::FrameNear(vr_fog_grid_state.frame, frame)) {
        flags |= Air::FlagGrid;
    }
    out[0] = flags;
    if constexpr (Common::Log::Enabled) {
        // Once a second (VR_AIR_DIAG=1): what the pass did, for blinks.
        static const bool diag = [] {
            const char* const env = std::getenv("VR_AIR_DIAG");
            return env != nullptr && env[0] == '1';
        }();
        auto& d = g_air_diag;
        ++d.applies;
        d.with_cells += (flags & Air::FlagCells) != 0;
        d.with_grid += (flags & Air::FlagGrid) != 0;
        if (d.first_frame == 0) {
            d.first_frame = frame;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - d.since >= std::chrono::seconds(1)) {
            if (diag) {
                // (A warning: the headset trial's log filter keeps Render.Vulkan at warnings.)
                LOG_WARNING(Render_Vulkan,
                            "VR air diag: frames {} copies {} with cells {} with the fog grid {} | computes half {} blur {} | left: no shadow pass {} "
                            "stale cascades {} mirror {} moon off {} not the host {} | the copy left: no shadow pass {} stale "
                            "cascades {} not the scene's {}; its record was not the shadow pass's {} times (widest gap {}), "
                            "not the composite's {} | colour ({:.3f} {:.3f} {:.3f}) open sky {:.2f} ground {:.2f} cells "
                            "frame {} frame {}",
                            frame - d.first_frame + 1, d.applies, d.with_cells, d.with_grid, d.half, d.blur, d.no_shadow,
                            d.stale_cascades, d.mirror, d.no_moon, d.not_host, d.apply_no_shadow, d.apply_stale,
                            d.apply_no_scene, d.apply_other_shadow, d.widest_gap, d.apply_other_scene,
                            vr_air.colour[0], vr_air.colour[1], vr_air.colour[2], vr_air_aim.open_sky, vr_air.ground,
                            vr_air.cells_frame, frame);
            }
            d = AirDiag{};
            d.since = now;
        }
    }
    const std::array<float, 3> colour = VrAirColour();
    for (u32 k = 0; k < 3; ++k) {
        put(4 + k, colour[k]);
    }
    return bytes;
}

// The air's colour (the moonlight's and the mist's): the probe's, its hue a share greyer
// (air_light.h Mist::Saturation), times the live key's strength.
std::array<float, 3> Rasterizer::VrAirColour() const {
    const auto& test = TestColour();
    const float strength = std::clamp(VR::Tune::Get("lighting.air_colour", 1.0f), 0.0f, 8.0f);
    std::array<float, 3> colour;
    // (Under the blood moon the night's own colour over the probe's: air_light.h BloodMoon.)
    const std::array<float, 4> blood = Air::Mist::BloodMoon::ColourNow();
    for (u32 k = 0; k < 3; ++k) {
        colour[k] = test[0] >= 0.0f ? test[k] : vr_air.colour[k] + (blood[k] - vr_air.colour[k]) * vr_air.blood_moon;
    }
    const float grey = 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2];
    const float saturation = std::clamp(VR::Tune::Get("lighting.air_saturation", Air::Mist::Saturation), 0.0f, 1.0f);
    for (float& c : colour) {
        c = std::max(0.0f, grey + (c - grey) * saturation) * strength;
    }
    return colour;
}

// This draw copies the frame's finished scene (the picture the light composite drew, seen within
// the frames' slack): the air pass's apply, and the draw the fog grid is built before.
bool Rasterizer::VrAirSceneCopy(const Shader::Info& stage, u64 frame) const {
    const u64 input = FirstInput(stage);
    return input != 0 && input == vr_air.scene_target && Air::FrameNear(vr_air.scene_frame, frame);
}

static_assert(Air::SourceLights == Shader::Optimization::FogLight::MaxLights);
static_assert(Air::SourceLights == 24 && Air::SourceDwords == 4 && Air::ProbeSlots == 4,
              "vk_rasterizer.h VrAirState's arrays");

// The lights this frame's scene copy is given, in the buffer's order (VrFogUpload, under the lamp
// memory's lock): where they are in the world, for the votes read back later.
void Rasterizer::VrAirSourceList(u32 count, const VrFogCandidate* const* order) {
    auto& list = vr_air.source_lists[vr_air.probe_counter % Air::ProbeSlots];
    list.count = std::min<u32>(count, Air::SourceLights);
    for (u32 k = 0; k < list.count; ++k) {
        const VrFogRemembered* const m = order[k]->remembered;
        list.world[k] = m ? m->halo_world : std::array<float, 3>{1e9f, 1e9f, 1e9f};
    }
}

// The votes read back (VrAirFrame) counted into the remembered lights (VrFogUpload, under the
// lamp memory's lock): a light whose inner disc showed something SourceRatio times brighter than
// its ring gains confidence, one looked at without it loses a little; confirmed at SourceConfirm
// and from then on.
void Rasterizer::VrAirSourceVotes() {
    if (!vr_air.votes_pending) {
        return;
    }
    vr_air.votes_pending = false;
    static const bool log = [] {
        const char* const env = std::getenv("VR_AIR_SOURCE_LOG");
        return env != nullptr && env[0] == '1';
    }();
    for (u32 k = 0; k < vr_air.votes_list.count; ++k) {
        const u32* const v = vr_air.votes.data() + k * Air::SourceDwords;
        const u32 inner_max = v[0], inner_n = v[1], ring_max = v[2], ring_n = v[3];
        if (inner_n < Air::SourceMinInner || ring_n < Air::SourceMinRing) {
            continue; // not looked at (off the screen, hidden, too small)
        }
        const auto& where = vr_air.votes_list.world[k];
        VrFogRemembered* light = nullptr;
        float nearest = Air::SourceMatchM * Air::SourceMatchM;
        for (VrFogRemembered& m : vr_fog.memory) {
            const float dx = m.halo_world[0] - where[0], dy = m.halo_world[1] - where[1],
                        dz = m.halo_world[2] - where[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nearest) {
                nearest = d2;
                light = &m;
            }
        }
        if (light == nullptr) {
            continue;
        }
        const bool was = light->sourced;
        // (A flickering light by the fire's rule, air_light.h SourceFireRatio.)
        const bool flickers = light->lum_dev > Shader::Optimization::FogLight::FlameFlicker *
                                                   std::max(light->lum_mean, 1e-3f);
        const bool flame =
            flickers ? float(inner_max) >= Air::SourceFireLum * Air::SourceLumScale &&
                           float(inner_max) >= Air::SourceFireRatio * float(ring_max)
                     : float(inner_max) >=
                           (was ? Air::SourceRatioKeep : Air::SourceRatio) * float(std::max(ring_max, 1u));
        light->source = std::clamp(light->source + (flame ? Air::SourceYes : -Air::SourceNo), 0.0f, 1.0f);
        light->sourced = light->sourced ? light->source > Air::SourceDrop : light->source >= Air::SourceConfirm;
        if constexpr (Common::Log::Enabled) {
            if (log && (light->sourced != was || (vr_air.probe_counter % 240 == 7))) {
                LOG_INFO(Render_Vulkan,
                         "VR air pass: light at ({:.1f} {:.1f} {:.1f}) r {:.1f} m: inner {:.2f} ({} px), "
                         "ring {:.2f} ({} px): {}; confidence {:.2f}{}",
                         where[0], where[1], where[2], light->radius, float(inner_max) / Air::SourceLumScale,
                         inner_n, float(ring_max) / Air::SourceLumScale, ring_n, flame ? "a flame" : "none",
                         light->source, light->sourced ? (was ? ", has a source" : ", NOW has a source")
                                                      : (was ? ", LOST its source" : ""));
            }
        }
    }
}

// The scene's copy reads the cells the blur wrote earlier in the frame: the frame's first such
// draw waits for those writes (as the lamp draws for the shadow mask's, VrSceneMapsBeforeDraw).
void Rasterizer::VrAirBeforeDraw(const GraphicsPipeline* pipeline) {
    if (!Air::Pass() || !vr_air.cells || !VR::IsEnabled()) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
    if (fs == nullptr || !Air::IsApplyProgram(fs->pgm_hash)) {
        return;
    }
    const u64 frame = VR::DrawFrameRecord().id;
    if (frame == 0 || vr_air.barrier_frame == frame || !Air::FrameNear(vr_air.cells_frame, frame)) {
        return;
    }
    vr_air.barrier_frame = frame;
    scheduler.EndRendering();
    NativeStereo::Barrier(scheduler);
}

} // namespace Vulkan
