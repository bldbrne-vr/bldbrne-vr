#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/sever_reveal.h"
#include "core/vr/vr_session.h"
#include "video_core/host_shaders/vr_inframe_frag.h"
#include "video_core/host_shaders/vr_inframe_vert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vr_inframe.h"
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"
#include "video_core/renderer_vulkan/vr_syringe_mesh.h"
#include "video_core/renderer_vulkan/vr_molotov_mesh.h"
#include "video_core/renderer_vulkan/vr_apothecary_mesh.h"
#include "video_core/renderer_vulkan/vr_goods_meshes.h"
#include "video_core/renderer_vulkan/vr_hunter_relic_meshes.h"
#include "video_core/renderer_vulkan/vr_augur_spray.h"

#include <vk_mem_alloc.h>
#include <cmrc/cmrc.hpp>

CMRC_DECLARE(res);

namespace Vulkan {

namespace {

namespace O = VrOverlay;
namespace F = VR::InFrame;

// Mirrors DrawData in vr_inframe.vert / .frag (std430 push constants).
struct DrawConstants {
    std::array<float, 12> mv;
    std::array<float, 4> color;
    std::array<float, 4> emissive;
    std::array<float, 4> uv;
    std::array<float, 4> proj;
    std::array<float, 4> extra;
};
static_assert(sizeof(DrawConstants) == 128); // the guaranteed push constant size

constexpr u32 AtlasWidth = 2048;
constexpr u32 AtlasHeight = 1024;
constexpr u32 AtlasRow = 64;
constexpr u32 MaxIcons = 16;
// (kept severed pieces: eight cuts of a piece (a few models: a texture of its own each, the
// flesh's) and a stump each: bloodborne_sever.cpp MaxPieces, MaxPieceModels; freed as dropped)
constexpr u32 MaxModels = 96;
// New models uploaded a frame (a cut brings up to eleven at once: spread over a few frames).
constexpr u32 MaxModelUploadsPerFrame = 4;
constexpr u32 MaxUploadsPerFrame = 4;
// vr_inframe.frag: shade + these flags (the cards' analytic edges).
constexpr u32 EdgeDisc = 16;
constexpr u32 EdgeRing = 32;
// ... both (never a card's edge): Item::candle.
constexpr u32 Candle = EdgeDisc | EdgeRing;

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool HasStencil(vk::Format format) {
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint ||
           format == vk::Format::eD16UnormS8Uint;
}

// ---- The in-frame mesh library (metres) ---------------------------------------------------------

O::MeshData Rotated(O::MeshData mesh, const O::Mat4& m) {
    O::TransformMesh(mesh, m);
    return mesh;
}

O::MeshData QuadMesh() {
    O::MeshData q{};
    q.vertices = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
    };
    q.indices = {0, 1, 2, 0, 2, 3};
    q.closed = false;
    return q;
}

O::MeshData LatheOf(std::initializer_list<O::ProfilePoint> points, u32 segments, float shift_y) {
    std::vector<O::ProfilePoint> profile(points);
    O::MeshData mesh = O::Lathe(profile, segments);
    O::TransformMesh(mesh, O::Mat4::Translation({0.0f, shift_y, 0.0f}));
    return mesh;
}

// The Blood Vial's needle (core/vr/vial_needle.h, MeshId::Needle): a wire along -Y, y 0 .. -1,
// drawn as a ribbon the vertex shader turns to the eye (Shade::Steel, uv.x > 0): its vertices on
// the axis in pairs, aux = (side -1 / +1, its radius there as a share of the shaft's: 1 along the
// shaft, tapering to 0 at the point over the last NeedleTaper of it). 4 triangles.
constexpr float NeedleTaper = 0.16f; // of the needle's mesh (3.6 mm of 22.5: a fine point)

O::MeshData NeedleMesh() {
    O::MeshData m{};
    m.closed = false;
    const std::array<std::array<float, 2>, 3> stations{{
        {0.0f, 1.0f},                  // the root (in the hub)
        {-1.0f + NeedleTaper, 1.0f},   // the point's taper starts
        {-1.0f, 0.0f},                 // the point
    }};
    for (const auto& st : stations) {
        for (const float side : {-1.0f, 1.0f}) {
            m.vertices.push_back({{0.0f, st[0], 0.0f}, {0.0f, 0.0f, 1.0f}, {side, st[1]}});
        }
    }
    for (u32 i = 0; i + 1 < stations.size(); ++i) {
        const u32 a = 2 * i;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    return m;
}

O::MeshData BuildMesh(F::MeshId id) {
    constexpr float Quarter = 0.5f * O::Pi;
    const O::Mat4 face_z = O::Mat4::RotationX(Quarter); // +Y (a puck's axis) -> +Z
    switch (id) {
    case F::MeshId::Plate:
        return Rotated(O::Puck(0.5f, 0.06f, 0.025f, 48), face_z);
    case F::MeshId::Ring: {
        std::vector<O::ProfilePoint> circle;
        for (u32 i = 0; i < 10; ++i) {
            const float a = 2.0f * O::Pi * static_cast<float>(i) / 10.0f;
            circle.push_back({0.5f + 0.03f * std::cos(a), 0.03f * std::sin(a)});
        }
        return Rotated(O::Lathe(circle, 48, true), face_z);
    }
    case F::MeshId::Quad:
        return QuadMesh();
    case F::MeshId::Plaque:
        return O::RoundedBox({0.5f, 0.5f, 0.5f}, 0.12f, 3);
    case F::MeshId::Bottle:
        return LatheOf({{0.0f, 0.0f},
                        {0.030f, 0.0f, true},
                        {0.034f, 0.008f},
                        {0.034f, 0.065f},
                        {0.016f, 0.090f},
                        {0.011f, 0.098f},
                        {0.011f, 0.120f, true},
                        {0.014f, 0.124f},
                        {0.0f, 0.127f}},
                       20, -0.055f);
    case F::MeshId::Knife: {
        O::MeshData blade = O::RoundedBox({0.011f, 0.045f, 0.0018f}, 0.0015f, 2);
        O::TransformMesh(blade, O::Mat4::Translation({0.0f, 0.03f, 0.0f}));
        O::MeshData handle = O::RoundedBox({0.008f, 0.028f, 0.007f}, 0.004f, 2);
        O::TransformMesh(handle, O::Mat4::Translation({0.0f, -0.04f, 0.0f}));
        O::AppendMesh(blade, handle);
        return blade;
    }
    case F::MeshId::Pebble: {
        O::MeshData m = O::Sphere(0.022f, 10, 6);
        O::TransformMesh(m, O::Mat4::Scale(O::Vec3{1.0f, 0.75f, 0.9f}));
        return m;
    }
    case F::MeshId::Flask:
        return LatheOf({{0.0f, 0.0f},
                        {0.022f, 0.0f, true},
                        {0.025f, 0.010f},
                        {0.025f, 0.055f},
                        {0.010f, 0.072f},
                        {0.008f, 0.088f, true},
                        {0.0f, 0.089f}},
                       18, -0.045f);
    case F::MeshId::Pellet:
        return O::Sphere(0.013f, 12, 8);
    case F::MeshId::Crystal:
        return O::Sphere(0.020f, 6, 4);
    case F::MeshId::Paper:
        return O::RoundedBox({0.035f, 0.05f, 0.002f}, 0.0015f, 2);
    case F::MeshId::Ash: {
        O::MeshData m = O::Puck(0.022f, 0.035f, 0.010f, 16);
        O::TransformMesh(m, O::Mat4::Translation({0.0f, -0.017f, 0.0f}));
        return m;
    }
    case F::MeshId::Token:
        return Rotated(O::Puck(0.02f, 0.008f, 0.003f, 20), face_z);
    case F::MeshId::Axis:
        return Rotated(O::TaperedCapsule(1.0f, 0.03f, 0.015f, 8, 2), face_z);
    case F::MeshId::GaugePlate: {
        // A gently curved shell: (u, -sag(v), v), u and v in -0.5 .. 0.5; the sag (metres, not
        // scaled with the plate) of a 45 mm radius cylinder over the plate's 34.5 mm (+ the lugs'
        // margin, 40.5 mm). Its normal is the implicit surface's gradient (0, 1, 2 k v): the
        // shader's inverse(R) takes the plate's X / Z scale into account.
        constexpr u32 Nu = 16;
        constexpr u32 Nv = 10;
        constexpr float Radius = 0.045f;
        constexpr float HalfWidth = 0.5f * 0.0405f;
        const float sag = Radius * (1.0f - std::cos(HalfWidth / Radius));
        const float k = sag / 0.25f; // y = -k v^2
        O::MeshData m{};
        m.closed = false;
        for (u32 i = 0; i <= Nu; ++i) {
            for (u32 j = 0; j <= Nv; ++j) {
                const float u = static_cast<float>(i) / Nu - 0.5f;
                const float v = static_cast<float>(j) / Nv - 0.5f;
                const O::Vec3 n = O::Normalize(O::Vec3{0.0f, 1.0f, 2.0f * k * v});
                m.vertices.push_back({{u, -k * v * v, v}, n, {u + 0.5f, v + 0.5f}});
            }
        }
        for (u32 i = 0; i < Nu; ++i) {
            for (u32 j = 0; j < Nv; ++j) {
                const u32 a = i * (Nv + 1) + j;
                const u32 b = a + Nv + 1;
                m.indices.insert(m.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
            }
        }
        return m;
    }
    case F::MeshId::GaugeStrap: {
        // A ring segment of the unit cylinder about X: (x, cos a, sin a), a = -75 .. 75 degrees.
        constexpr u32 Na = 30;
        constexpr float Span = 75.0f * O::Pi / 180.0f;
        O::MeshData m{};
        m.closed = false;
        for (u32 i = 0; i <= Na; ++i) {
            const float a = -Span + 2.0f * Span * static_cast<float>(i) / Na;
            const O::Vec3 n{0.0f, std::cos(a), std::sin(a)};
            for (u32 j = 0; j < 2; ++j) {
                const float x = j == 0 ? -0.5f : 0.5f;
                m.vertices.push_back({{x, n.y, n.z}, n, {x + 0.5f, static_cast<float>(i) / Na}});
            }
        }
        for (u32 i = 0; i < Na; ++i) {
            const u32 a = 2 * i;
            m.indices.insert(m.indices.end(), {a, a + 2, a + 3, a, a + 3, a + 1});
        }
        return m;
    }
    case F::MeshId::ApothecaryCase:
        return O::ApothecaryCaseMesh();
    case F::MeshId::QuickHolster:
        return O::QuickHolsterMesh();
    case F::MeshId::QuickHolsterLow:
        return O::QuickHolsterMesh(true);
    case F::MeshId::StoredSyringe:
        return O::AntiqueSyringeMesh(18);
    case F::MeshId::VictorianMolotov:
        return O::VictorianMolotovMesh();
    case F::MeshId::Syringe:
        return O::AntiqueSyringeMesh();
    case F::MeshId::IosefkaBottle:
        return O::IosefkaBottleMesh();
    case F::MeshId::IosefkaLabel:
        return O::IosefkaLabelMesh();
    case F::MeshId::BloodTube:
        return O::BloodTubeMesh(false);
    case F::MeshId::BloodTubeRing:
        return O::BloodTubeMesh(true);
    case F::MeshId::RubPaperFire:
        return O::RubPaperMesh(O::GoodsMat::FirePaper);
    case F::MeshId::RubPaperBolt:
        return O::RubPaperMesh(O::GoodsMat::BoltPaper);
    case F::MeshId::PhantasmShell: {
        O::MeshData m = O::PhantasmShellMesh();
        O::BakeOcclusion(m);
        return m;
    }
    case F::MeshId::HunterMark:
    case F::MeshId::RegisterBook:
    case F::MeshId::LeagueBook:
    case F::MeshId::BeckoningBell:
    case F::MeshId::ChoirBell:
    case F::MeshId::BeastPellet:
    case F::MeshId::AshUrn:
    case F::MeshId::UmbilicalCord:
        return O::ReusedGoodsMesh(static_cast<int>(id) - static_cast<int>(F::MeshId::HunterMark));
    case F::MeshId::Skull: {
        const auto file = cmrc::res::get_filesystem().open("src/resources/vr/sangiran17.bin");
        return O::SkullMeshFrom({reinterpret_cast<const u8*>(file.begin()), file.size()});
    }
    case F::MeshId::MessengerSkull: {
        const auto file=cmrc::res::get_filesystem().open("src/resources/vr/messenger_skull.bin");
        return O::SkullMeshFrom({reinterpret_cast<const u8*>(file.begin()),file.size()});
    }
    case F::MeshId::TinyTonitrus: return O::HunterRelic::Tonitrus();
    case F::MeshId::ExecutionerGloves: return O::HunterRelic::Gloves();
    case F::MeshId::Cigarette: return O::CigaretteMesh(false);
    case F::MeshId::CigarettePacket: return O::CigaretteMesh(true);
    case F::MeshId::BlackskyEye: return O::HunterRelic::BlackskyEye();
    case F::MeshId::BlackskyCornea: return O::HunterRelic::BlackskyCornea();
    case F::MeshId::TonitrusBridge: return O::HunterRelic::TonitrusBridge();
    case F::MeshId::AugurSpray: return O::AugurSprayMesh();
    case F::MeshId::MessengerChunks: {
        const auto file=cmrc::res::get_filesystem().open("src/resources/vr/messenger_chunks.bin");
        return O::MessengerChunksFrom({reinterpret_cast<const u8*>(file.begin()),file.size()});
    }
    case F::MeshId::BloodClothVelvet:
    case F::MeshId::BloodClothLinen:
    case F::MeshId::ResonantBell:
    case F::MeshId::Antidote:
    case F::MeshId::Notebook:
    case F::MeshId::Coldblood: {
        O::MeshData m = id == F::MeshId::BloodClothVelvet ? O::BloodTubeClothMesh(true)
                        : id == F::MeshId::BloodClothLinen ? O::BloodTubeClothMesh(false)
                        : id == F::MeshId::ResonantBell    ? O::ResonantBellMesh()
                        : id == F::MeshId::Antidote        ? O::AntidoteMesh()
                        : id == F::MeshId::Notebook        ? O::NotebookMesh()
                                                           : O::ColdbloodMesh();
        O::BakeOcclusion(m, id == F::MeshId::ResonantBell ? .04f : .012f); // the hollows, once
        return m;
    }
    case F::MeshId::Needle:
        return NeedleMesh();
    case F::MeshId::NeedleHub:
        // The needle's hub: a collar from the vial's end (radius 0.5 at y 0, a short straight
        // band) narrowing to a quarter of that at y -1, where the needle comes out. 12 round.
        return LatheOf({{0.0f, -1.0f},
                        {0.24f, -1.0f, true},
                        {0.27f, -0.90f},
                        {0.36f, -0.55f},
                        {0.47f, -0.16f, true},
                        {0.50f, -0.10f},
                        {0.50f, 0.0f, true},
                        {0.0f, 0.0f}},
                       12, 0.0f);
    case F::MeshId::Cylinder:
        return O::Puck(1.0f, 1.0f, 0.001f, 24);
    case F::MeshId::Model: // drawn from its own buffers (VrInFrame::ModelOf)
    case F::MeshId::Count:
        break;
    }
    return QuadMesh();
}

struct InFrameShaders {
    std::vector<u32> vertex;
    std::vector<u32> fragment;
};

const InFrameShaders& ItemShaders() {
    static const InFrameShaders shaders{
        CompileGlsl(HostShaders::VR_INFRAME_VERT, vk::ShaderStageFlagBits::eVertex),
        CompileGlsl(HostShaders::VR_INFRAME_FRAG, vk::ShaderStageFlagBits::eFragment)};
    return shaders;
}

} // Anonymous namespace

VrInFrame::VrInFrame(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, device{instance_.GetDevice()} {
    // 0: the item's texture, 1: the scene light probe.
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eVertex},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eFragment},
    }};
    auto layout = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    if (layout.result != vk::Result::eSuccess) {
        Fail("descriptor set layout");
        return;
    }
    set_layout = std::move(layout.value);
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(DrawConstants),
    };
    auto pl = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout.get(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
    if (pl.result != vk::Result::eSuccess) {
        Fail("pipeline layout");
        return;
    }
    pipeline_layout = std::move(pl.value);
    const auto& shaders = ItemShaders();
    if (shaders.vertex.empty() || shaders.fragment.empty()) {
        Fail("shader compilation");
        return;
    }
    vert = CompileSPV(shaders.vertex, device);
    frag = CompileSPV(shaders.fragment, device);
    if (!vert || !frag) {
        Fail("shader module creation");
        return;
    }
    auto s = device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = static_cast<float>(ProbeLevels),
    });
    if (s.result != vk::Result::eSuccess) {
        Fail("sampler");
        return;
    }
    sampler = std::move(s.value);
    // The game models: their own uvs (may repeat) and mips, filtered between levels.
    auto ms = device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        .maxLod = 16.0f,
    });
    if (ms.result != vk::Result::eSuccess) {
        Fail("model sampler");
        return;
    }
    model_sampler = std::move(ms.value);
    white = VideoCore::UniqueImage(device, instance.GetAllocator());
    white.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto wv = device.createImageViewUnique({
        .image = white,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = ColorRange,
    });
    if (wv.result != vk::Result::eSuccess) {
        Fail("white view");
        return;
    }
    white_view = std::move(wv.value);
    const auto bc7_props =
        instance.GetPhysicalDevice().getFormatProperties(vk::Format::eBc7SrgbBlock);
    bc7 = (bc7_props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) !=
          vk::FormatFeatureFlags{};
    // The label atlas.
    label_atlas = VideoCore::UniqueImage(device, instance.GetAllocator());
    label_atlas.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8Unorm,
        .extent = {AtlasWidth, AtlasHeight, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto lv = device.createImageViewUnique({
        .image = label_atlas,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8Unorm,
        .subresourceRange = ColorRange,
    });
    if (lv.result != vk::Result::eSuccess) {
        Fail("label atlas view");
        return;
    }
    label_view = std::move(lv.value);
    if (!BuildMeshes()) {
        return;
    }
    auto qp = device.createQueryPoolUnique({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = TimingSlots * 2,
    });
    if (qp.result == vk::Result::eSuccess) {
        query_pool = std::move(qp.value);
        timestamp_period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    }
    stats_since_ns = NowNs();
    VR::Assets::Start(); // names and the label font load in the background from now on
    LOG_INFO(Render_Vulkan, "VR inframe: renderer ready (BC7 icons {}, label atlas {}x{})",
             bc7 ? "yes" : "NO (plain cards)", AtlasWidth, AtlasHeight);
}

VrInFrame::~VrInFrame() {
    if (pending_translucent && pending_translucent->result.valid()) {
        pending_translucent->result.wait(); // its worker uses the shader modules below
    }
    for (const vk::ShaderModule module : {vert, frag}) {
        if (module) {
            device.destroyShaderModule(module);
        }
    }
    for (Buffer* b : {&vertices, &indices, &probe_buffer}) {
        if (b->buffer) {
            vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(b->buffer),
                             b->allocation);
        }
    }
    for (auto& [id, model] : models) {
        for (Buffer* b : {&model.vertices, &model.indices, &model.animation}) {
            if (b->buffer) {
                vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(b->buffer),
                                 b->allocation);
            }
        }
    }
}

bool VrInFrame::Fail(const std::string& what) {
    if (!failed) {
        failed = true;
        F::NoteDisabled(); // the arc is not opened any more: Y is the game's Square
        LOG_ERROR(Render_Vulkan, "VR inframe: disabled ({}); the frame goes on without it", what);
    }
    return false;
}

bool VrInFrame::CreateBuffer(Buffer& out, vk::DeviceSize size, vk::BufferUsageFlags usage) {
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = std::max<vk::DeviceSize>(size, 16),
        .usage = static_cast<VkBufferUsageFlags>(usage),
    };
    const VmaAllocationCreateInfo alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer{};
    VmaAllocationInfo info_out{};
    if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &buffer, &out.allocation,
                        &info_out) != VK_SUCCESS) {
        return false;
    }
    out.buffer = vk::Buffer{buffer};
    out.mapped = info_out.pMappedData;
    return out.mapped != nullptr;
}

void VrInFrame::DestroyBufferLater(Buffer& b) {
    if (!b.buffer) {
        return;
    }
    scheduler.DeferOperation([allocator = instance.GetAllocator(), buffer = b.buffer,
                              allocation = b.allocation] {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(buffer), allocation);
    });
    b = {};
}

namespace {
// The item library (BuildMesh for every id): the same for every VrInFrame and session. Its
// occlusion bakes took ~1-2 s of stage B at the first world frame, once for each eye's VrInFrame
// (2026-11-07 stall sampler); now built once, on a thread of its own from the first ask.
struct MeshLibrary {
    std::vector<O::Vertex> verts;
    std::vector<u32> idx;
    std::array<std::array<u32, 3>, static_cast<size_t>(F::MeshId::Count)> ranges{};
};
std::shared_future<std::shared_ptr<const MeshLibrary>> MeshLibraryFuture() {
    static const std::shared_future<std::shared_ptr<const MeshLibrary>> library =
        std::async(std::launch::async, [] {
            Common::SetCurrentThreadName("VrInFrameMeshes");
            auto lib = std::make_shared<MeshLibrary>();
            for (u32 i = 0; i < static_cast<u32>(F::MeshId::Count); ++i) {
                const O::MeshData mesh = BuildMesh(static_cast<F::MeshId>(i));
                lib->ranges[i] = {static_cast<u32>(lib->idx.size()),
                                  static_cast<u32>(mesh.indices.size()),
                                  static_cast<u32>(lib->verts.size())};
                lib->verts.insert(lib->verts.end(), mesh.vertices.begin(), mesh.vertices.end());
                lib->idx.insert(lib->idx.end(), mesh.indices.begin(), mesh.indices.end());
            }
            return std::shared_ptr<const MeshLibrary>(std::move(lib));
        }).share();
    return library;
}
} // Anonymous namespace

void VrInFrame::Prewarm() {
    (void)MeshLibraryFuture();
    (void)ItemShaders();
}

bool VrInFrame::BuildMeshes() {
    const auto wait_start = std::chrono::steady_clock::now();
    const auto library = MeshLibraryFuture().get();
    LOG_INFO(Render_Vulkan, "VR inframe: item meshes ready ({} vertices; waited {:.1f} ms)",
             library->verts.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                       wait_start)
                 .count());
    const auto& verts = library->verts;
    const auto& idx = library->idx;
    for (u32 i = 0; i < static_cast<u32>(F::MeshId::Count); ++i) {
        const auto& r = library->ranges[i];
        ranges[i] = {r[0], r[1], static_cast<s32>(r[2])};
    }
    if (!CreateBuffer(vertices, verts.size() * sizeof(O::Vertex),
                      vk::BufferUsageFlagBits::eVertexBuffer |
                          vk::BufferUsageFlagBits::eStorageBuffer) ||
        !CreateBuffer(indices, idx.size() * sizeof(u32), vk::BufferUsageFlagBits::eIndexBuffer)) {
        return Fail("mesh buffers");
    }
    std::memcpy(vertices.mapped, verts.data(), verts.size() * sizeof(O::Vertex));
    std::memcpy(indices.mapped, idx.data(), idx.size() * sizeof(u32));
    vmaFlushAllocation(instance.GetAllocator(), vertices.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(instance.GetAllocator(), indices.allocation, 0, VK_WHOLE_SIZE);
    return true;
}

vk::Pipeline VrInFrame::PipelineFor(vk::Format color_format, vk::Format depth_fmt, bool xray,
                                      const std::array<float, 2>& depth, bool translucent) {
    for (const auto& p : pipelines) {
        if (p.color == color_format && p.depth == depth_fmt && p.xray == xray &&
            p.depth_terms == depth && p.translucent == translucent) {
            return *p.pipeline;
        }
    }
    // Only the depth mapping is specialized: the near and far planes stay put for a session. The
    // eye's projection is pushed (proj, Record), so the Monocular's zoomed eye, whose window
    // slides every frame, costs no pipeline (until 2026-11-03 a new one each frame: the in-frame
    // pass compiled ~30 ms of pipelines a frame while looking around through it).
    if (pipelines.size() == 16) {
        LOG_WARNING(Render_Vulkan, "VR in-frame: 16 pipelines built: a key that changes every "
                                   "frame? (formats, x-ray and the depth mapping only)");
    }
    const auto start = NowNs();
    auto created = CreatePipeline(color_format, depth_fmt, xray, depth, translucent);
    if (!created) {
        Fail(fmt::format("pipeline for {} / {}", vk::to_string(color_format),
                         vk::to_string(depth_fmt)));
        return {};
    }
    pipelines.push_back({color_format, depth_fmt, xray, translucent, depth, std::move(created)});
    LOG_INFO(Render_Vulkan, "VR inframe: {}pipeline for {} + {} ({:.1f} ms)", xray ? "x-ray " : "",
             vk::to_string(color_format), vk::to_string(depth_fmt),
             static_cast<double>(NowNs() - start) * 1e-6);
    return *pipelines.back().pipeline;
}

vk::Pipeline VrInFrame::TranslucentFor(vk::Format color_format, vk::Format depth_fmt,
                                       const std::array<float, 2>& depth) {
    for (const auto& p : pipelines) {
        if (p.color == color_format && p.depth == depth_fmt && !p.xray && p.depth_terms == depth &&
            p.translucent) {
            return *p.pipeline;
        }
    }
    if (failed || translucent_failed) {
        return {};
    }
    if (!pending_translucent) {
        try {
            pending_translucent.emplace(PendingPipeline{
                color_format, depth_fmt, depth, NowNs(),
                std::async(std::launch::async, [this, color_format, depth_fmt, depth] {
                    return CreatePipeline(color_format, depth_fmt, false, depth, true);
                })});
        } catch (const std::exception& e) { // no thread for it (std::system_error)
            translucent_failed = true;
            LOG_WARNING(Render_Vulkan,
                        "VR inframe: no worker for the translucent pipeline ({}): what needs it "
                        "stays hidden",
                        e.what());
        }
        return {};
    }
    auto& p = *pending_translucent;
    if (p.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return {};
    }
    auto created = p.result.get();
    const bool same = p.color == color_format && p.depth == depth_fmt && p.depth_terms == depth;
    const bool built = static_cast<bool>(created);
    if (built) {
        LOG_INFO(Render_Vulkan,
                 "VR inframe: translucent pipeline for {} + {} ({:.1f} ms, on a "
                 "worker)",
                 vk::to_string(p.color), vk::to_string(p.depth),
                 static_cast<double>(NowNs() - p.start_ns) * 1e-6);
        pipelines.push_back({p.color, p.depth, false, true, p.depth_terms, std::move(created)});
    } else {
        // Not asked again every frame (a thread and a build each): what needs it (translucent
        // items, the gray world, the Tonitrus' flash) stays hidden, the rest draws on.
        translucent_failed = true;
        LOG_WARNING(Render_Vulkan, "VR inframe: the translucent pipeline failed on its worker; "
                                   "what needs it stays hidden");
    }
    pending_translucent.reset(); // a different key: the next ask builds that one
    return same && built ? *pipelines.back().pipeline : vk::Pipeline{};
}

vk::UniquePipeline VrInFrame::CreatePipeline(vk::Format color_format, vk::Format depth_fmt,
                                             bool xray, const std::array<float, 2>& depth,
                                             bool translucent) const {
    const std::array<vk::SpecializationMapEntry, 2> entries{{
        {0, 0, sizeof(float)}, {1, 4, sizeof(float)},
    }};
    const vk::SpecializationInfo specialization{
        .mapEntryCount = static_cast<u32>(entries.size()), .pMapEntries = entries.data(),
        .dataSize = sizeof(depth), .pData = depth.data(),
    };
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = vert, .pName = "main",
         .pSpecializationInfo = &specialization},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = frag, .pName = "main"},
    }};
    const vk::VertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(O::Vertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
    const std::array<vk::VertexInputAttributeDescription, 3> attributes{{
        {.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0},
        {.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 12},
        {.location = 2, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = 24},
    }};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    // Items are depth-tested against the game's scene and write their depth (DIBR shows them at
    // their own depth); x-ray items pass exactly where the others fail (something of the scene
    // in front) and leave the depth alone.
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = true,
        .depthWriteEnable = !xray && !translucent,
        .depthCompareOp = xray ? vk::CompareOp::eGreater : vk::CompareOp::eLessOrEqual,
    };
    const std::array<vk::PipelineColorBlendAttachmentState, 2> blend_attachments{{
        {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOne,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            // The game's alpha channel is left alone.
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB,
        },
        {
            // The coverage: the most any item covered each pixel.
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOne,
            .dstColorBlendFactor = vk::BlendFactor::eOne,
            .colorBlendOp = vk::BlendOp::eMax,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOne,
            .alphaBlendOp = vk::BlendOp::eMax,
            .colorWriteMask = vk::ColorComponentFlagBits::eR,
        },
    }};
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = static_cast<u32>(blend_attachments.size()),
        .pAttachments = blend_attachments.data(),
    };
    const std::array<vk::DynamicState, 2> dynamic_states{vk::DynamicState::eViewport,
                                                         vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const std::array<vk::Format, 2> color_formats{color_format, CoverFormat};
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = static_cast<u32>(color_formats.size()),
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = depth_fmt,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };
    const vk::GraphicsPipelineCreateInfo ci{
        .pNext = &rendering,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = *pipeline_layout,
    };
    auto result = device.createGraphicsPipelineUnique({}, ci);
    if (result.result != vk::Result::eSuccess) {
        return {};
    }
    return std::move(result.value);
}

bool VrInFrame::PrepareProbe(vk::Format src_format, vk::Extent2D extent) {
    probe_valid = false;
    if (failed || extent.width < 128 || extent.height < 128) {
        return false;
    }
    const vk::Extent2D size{extent.width / 2, extent.height / 2};
    if (probe && probe_extent == size && probe_format == src_format) {
        return true;
    }
    if (probe) {
        scheduler.DeferOperation([image = std::move(probe),
                                  view = std::move(probe_view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
    probe = VideoCore::UniqueImage(device, instance.GetAllocator());
    probe.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = src_format,
        .extent = {size.width, size.height, 1},
        .mipLevels = ProbeLevels,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto view = device.createImageViewUnique({
        .image = probe,
        .viewType = vk::ImageViewType::e2D,
        .format = src_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, ProbeLevels, 0, 1},
    });
    if (view.result != vk::Result::eSuccess) {
        return Fail("probe view");
    }
    probe_view = std::move(view.value);
    probe_extent = size;
    probe_format = src_format;
    return true;
}

void VrInFrame::ProbeScenes(const RecorderProxy& cmdbuf, std::span<const ProbeSource> eyes,
                            vk::Image src, vk::Format src_format, vk::Extent2D extent,
                            vk::Offset2D inset) {
    std::array<VrInFrame*, 2> probes{};
    std::array<u32, 2> layers{};
    u32 count = 0;
    for (const ProbeSource& eye : eyes) {
        if (count < probes.size() && eye.renderer != nullptr &&
            eye.renderer->PrepareProbe(src_format, extent)) {
            probes[count] = eye.renderer;
            layers[count++] = eye.source_layer;
        }
    }
    if (count == 0) {
        return;
    }
    // One barrier call for every eye's probe (the same step of each chain).
    std::array<vk::ImageMemoryBarrier2, 2> barriers{};
    const auto barrier = [&](const vk::ImageMemoryBarrier2& step, u32 level, u32 levels) {
        for (u32 i = 0; i < count; ++i) {
            barriers[i] = step;
            barriers[i].image = probes[i]->probe;
            barriers[i].subresourceRange = {vk::ImageAspectFlagBits::eColor, level, levels, 0, 1};
        }
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = count,
            .pImageMemoryBarriers = barriers.data(),
        });
    };
    // Every level to TransferDst (the last frame's reads are done before: an execution dependency).
    barrier(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
            },
            0, ProbeLevels);
    // Halving blits with a linear filter: each level averages 2 x 2 texels of the one above.
    s32 w = static_cast<s32>(extent.width), h = static_cast<s32>(extent.height);
    // The first level's source: the scene less the inset on each side (none if that would leave
    // less than half of either side).
    const bool inset_fits = inset.x >= 0 && inset.y >= 0 && 4 * inset.x < w && 4 * inset.y < h;
    const s32 ix = inset_fits ? inset.x : 0, iy = inset_fits ? inset.y : 0;
    for (u32 level = 0; level < ProbeLevels; ++level) {
        const s32 nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
        const s32 x0 = level == 0 ? ix : 0, y0 = level == 0 ? iy : 0;
        for (u32 i = 0; i < count; ++i) {
            const vk::ImageBlit blit{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, level == 0 ? 0u : level - 1,
                                   level == 0 ? layers[i] : 0u, 1},
                .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{x0, y0, 0},
                                                          vk::Offset3D{w - x0, h - y0, 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, 1},
                .dstOffsets =
                    std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, vk::Offset3D{nw, nh, 1}},
            };
            const vk::Image probe = probes[i]->probe;
            cmdbuf.blitImage(level == 0 ? src : probe, vk::ImageLayout::eTransferSrcOptimal, probe,
                             vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        }
        // This level becomes the next one's source.
        barrier(vk::ImageMemoryBarrier2{
                    .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
                    .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                },
                level, 1);
        w = nw;
        h = nh;
    }
    barrier(vk::ImageMemoryBarrier2{
                .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                .srcAccessMask =
                    vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            },
            0, ProbeLevels);
    for (u32 i = 0; i < count; ++i) {
        probes[i]->probe_valid = true;
    }
}

// The shader's specialized depth mapping (A z + B) for a camera's near and far planes: the one
// pipeline key that follows the camera.
static std::array<float, 2> DepthTerms(const VrInFrame::Camera& camera) {
    const float n = camera.near_z;
    const float f = camera.far_z;
    return {f / (f - n), -n * f / (f - n)};
}

void VrInFrame::Warm(vk::Format color_format, vk::Format depth_fmt, float near_z, float far_z) {
    if (!failed) {
        Camera camera{}; // the usual planes, else this camera's
        if (near_z > 0.0f && far_z > near_z) {
            camera.near_z = near_z;
            camera.far_z = far_z;
        }
        const auto depth = DepthTerms(camera);
        PipelineFor(color_format, depth_fmt, false, depth);
        PipelineFor(color_format, depth_fmt, true, depth);
        TranslucentFor(color_format, depth_fmt, depth); // on a worker
    }
}

bool VrInFrame::EnsureDepth(const Target& t) {
    const auto retire_depth = [&] {
        scheduler.DeferOperation([image = std::move(depth),
                                  a = std::move(depth_attachment_view),
                                  s = std::move(depth_sampled_view)]() mutable {
            s.reset();
            a.reset();
            image.Destroy();
        }); // leaves `depth` empty
        depth_format = vk::Format::eUndefined;
        depth_extent = {};
    };
    if (t.depth_in_place) {
        // A copy left from frames before S1 goes once S1 has drawn in place for a second (a
        // frame that falls back now and then does not make it again each time).
        if (depth && ++in_place_lists >= 90) {
            retire_depth();
            LOG_INFO(Render_Vulkan, "VR inframe: in place in S1's depth; private depth copy freed");
        }
        return EnsureCover(t.depth_extent);
    }
    in_place_lists = 0;
    if (depth && depth_format == t.depth_format && depth_extent == t.depth_extent) {
        return EnsureCover(t.depth_extent);
    }
    if (depth) {
        retire_depth();
    }
    depth = VideoCore::UniqueImage(device, instance.GetAllocator());
    depth.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = t.depth_format,
        .extent = {t.depth_extent.width, t.depth_extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                 vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const vk::ImageAspectFlags all =
        HasStencil(t.depth_format)
            ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
            : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth};
    auto attachment = device.createImageViewUnique({
        .image = depth,
        .viewType = vk::ImageViewType::e2D,
        .format = t.depth_format,
        .subresourceRange = {all, 0, 1, 0, 1},
    });
    auto sampled = device.createImageViewUnique({
        .image = depth,
        .viewType = vk::ImageViewType::e2D,
        .format = t.depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    });
    if (attachment.result != vk::Result::eSuccess || sampled.result != vk::Result::eSuccess) {
        return Fail("depth copy views");
    }
    depth_attachment_view = std::move(attachment.value);
    depth_sampled_view = std::move(sampled.value);
    depth_format = t.depth_format;
    depth_extent = t.depth_extent;
    LOG_INFO(Render_Vulkan, "VR inframe: private depth copy {}x{} {}", depth_extent.width,
             depth_extent.height, vk::to_string(depth_format));
    return EnsureCover(t.depth_extent);
}

bool VrInFrame::EnsureCover(vk::Extent2D extent) {
    if (cover && cover_extent == extent) {
        return true;
    }
    if (cover) {
        scheduler.DeferOperation(
            [image = std::move(cover), view = std::move(cover_view)]() mutable {
                view.reset();
                image.Destroy();
            });
    }
    cover = VideoCore::UniqueImage(device, instance.GetAllocator());
    cover.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = CoverFormat,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto cv = device.createImageViewUnique({
        .image = cover,
        .viewType = vk::ImageViewType::e2D,
        .format = CoverFormat,
        .subresourceRange = ColorRange,
    });
    if (cv.result != vk::Result::eSuccess) {
        cover_extent = {};
        return Fail("coverage view");
    }
    cover_view = std::move(cv.value);
    cover_extent = extent;
    return true;
}

bool VrInFrame::Upload(const RecorderProxy& cmdbuf, vk::Image image, const void* data, size_t size,
                       const vk::BufferImageCopy& region, bool first_use) {
    Buffer staging{};
    if (!CreateBuffer(staging, size, vk::BufferUsageFlagBits::eTransferSrc)) {
        return false;
    }
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(instance.GetAllocator(), staging.allocation, 0, VK_WHOLE_SIZE);
    const vk::ImageMemoryBarrier2 to_dst{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = first_use ? vk::ImageLayout::eUndefined
                               : vk::ImageLayout::eShaderReadOnlyOptimal,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_dst,
    });
    cmdbuf.copyBufferToImage(staging.buffer, image, vk::ImageLayout::eTransferDstOptimal, region);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_read,
    });
    DestroyBufferLater(staging);
    ++stats_uploads;
    return true;
}

vk::ImageView VrInFrame::IconView(const RecorderProxy& cmdbuf, u32 icon_id) {
    if (!bc7) {
        return {};
    }
    if (const auto it = icons.find(icon_id); it != icons.end()) {
        it->second.last_used = frame;
        return *it->second.view;
    }
    const auto icon = VR::Assets::GetIcon(static_cast<s32>(icon_id));
    if (!icon) {
        return {}; // loading (or missing): a plain card meanwhile
    }
    if (icons.size() >= MaxIcons) {
        auto oldest = std::ranges::min_element(icons, {}, [](const auto& e) {
            return e.second.last_used;
        });
        if (oldest->second.last_used == frame) {
            return {};
        }
        scheduler.DeferOperation([texture = std::move(oldest->second)]() mutable {
            texture.view.reset();
            texture.image.Destroy();
        });
        icons.erase(oldest);
    }
    Texture t;
    t.image = VideoCore::UniqueImage(device, instance.GetAllocator());
    t.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eBc7SrgbBlock,
        .extent = {icon->width, icon->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto view = device.createImageViewUnique({
        .image = t.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eBc7SrgbBlock,
        .subresourceRange = ColorRange,
    });
    if (view.result != vk::Result::eSuccess) {
        Fail("icon view");
        return {};
    }
    t.view = std::move(view.value);
    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {icon->width, icon->height, 1},
    };
    if (!Upload(cmdbuf, t.image, icon->bc7.data(), icon->bc7.size(), region, true)) {
        return {};
    }
    t.last_used = frame;
    const vk::ImageView v = *t.view;
    icons.emplace(icon_id, std::move(t));
    return v;
}

void VrInFrame::DestroyModelLater(GpuModel& model) {
    DestroyBufferLater(model.vertices);
    DestroyBufferLater(model.indices);
    DestroyBufferLater(model.animation);
    if (model.shared_key != 0) {
        const auto it = shared_images.find(model.shared_key);
        if (it != shared_images.end() && --it->second.users == 0) {
            scheduler.DeferOperation([image = std::move(it->second.image),
                                      view = std::move(it->second.view)]() mutable {
                view.reset();
                image.Destroy();
            });
            shared_images.erase(it);
        }
        model.shared_key = 0;
        model.shared_view = vk::ImageView{};
    }
    if (model.surface)
        scheduler.DeferOperation(
            [image = std::move(model.surface), view = std::move(model.surface_view)]() mutable {
                view.reset();
                image.Destroy();
            });
    if (model.image) {
        scheduler.DeferOperation([image = std::move(model.image),
                                  view = std::move(model.view)]() mutable {
            view.reset();
            image.Destroy();
        });
    }
}

const VrInFrame::GpuModel* VrInFrame::ModelOf(const RecorderProxy& cmdbuf, s32 model_id,
                                            u32 reveal_token) {
    if (const auto it = models.find(model_id); it != models.end()) {
        it->second.last_used = frame;
        it->second.reveal_token = reveal_token;
        return &it->second;
    }
    if (model_missing.contains(model_id)) {
        return nullptr;
    }
    const auto model = VR::Assets::GetModel(model_id);
    if (!model) {
        return nullptr; // loading (the first call asked for it), or missing
    }
    if (model_uploads >= MaxModelUploadsPerFrame) {
        return nullptr; // the next frame's
    }
    // A runtime model (a severed piece): this eye looks for dropped ones from now on.
    runtime_held = runtime_held || model_id >= VR::Assets::RuntimeModelBase;
    if (models.size() >= MaxModels) {
        auto oldest = models.end();
        for (auto it = models.begin(); it != models.end(); ++it) {
            const auto& cached = it->second;
            if (cached.last_used == frame ||
                (cached.reveal_token != 0 && VR::Sever::g_reveals.Pinned(cached.reveal_token)))
                continue;
            if (oldest == models.end() || cached.last_used < oldest->second.last_used) oldest = it;
        }
        if (oldest == models.end()) {
            return nullptr;
        }
        DestroyModelLater(oldest->second);
        models.erase(oldest);
    }
    ++model_uploads; // a full/pinned cache must not spend the budget without an upload
    GpuModel g;
    g.reveal_token = reveal_token;
    g.single_surface = model->single_surface;
    g.animation_bones = model->animation_bones;
    const size_t vbytes = model->vertices.size() * sizeof(VR::Assets::ModelVertex);
    const bool compact_indices = !model->animation.empty() && model->vertices.size() <= 65536;
    const size_t ibytes = model->indices.size() * (compact_indices ? sizeof(u16) : sizeof(u32));
    static_assert(sizeof(VR::Assets::ModelVertex) == sizeof(O::Vertex));
    if (!CreateBuffer(g.vertices, vbytes, vk::BufferUsageFlagBits::eVertexBuffer) ||
        !CreateBuffer(g.indices, ibytes, vk::BufferUsageFlagBits::eIndexBuffer)) {
        DestroyModelLater(g);
        model_missing[model_id] = true;
        LOG_WARNING(Render_Vulkan, "VR inframe: model s{:05}: no buffers", model_id);
        return nullptr;
    }
    std::memcpy(g.vertices.mapped, model->vertices.data(), vbytes);
    if (compact_indices) {
        g.index_type = vk::IndexType::eUint16;
        auto* dst = static_cast<u16*>(g.indices.mapped);
        for (size_t i = 0; i < model->indices.size(); ++i)
            dst[i] = static_cast<u16>(model->indices[i]);
    } else {
        std::memcpy(g.indices.mapped, model->indices.data(), ibytes);
    }
    vmaFlushAllocation(instance.GetAllocator(), g.vertices.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(instance.GetAllocator(), g.indices.allocation, 0, VK_WHOLE_SIZE);
    g.index_count = static_cast<u32>(model->indices.size());
    g.vertex_count = static_cast<u32>(model->vertices.size());
    // The diffuse texture with the game's mip chain (a shared one: already up, its user).
    const auto& t = model->diffuse;
    bool shared_hit = false; // (its texture already up: shared, not uploaded again)
    if (t.share_key != 0) {
        if (const auto it = shared_images.find(t.share_key); it != shared_images.end()) {
            shared_hit = true;
            ++it->second.users;
            g.shared_key = t.share_key;
            g.shared_view = *it->second.view;
            g.textured = true;
        }
    }
    using TF = VR::Assets::ModelTexture::Format;
    const vk::Format format = t.format == TF::Bc1     ? vk::Format::eBc1RgbaSrgbBlock
                              : t.format == TF::Bc3   ? vk::Format::eBc3SrgbBlock
                              : t.format == TF::Bc7   ? vk::Format::eBc7SrgbBlock
                              : t.format == TF::Rgba8 ? vk::Format::eR8G8B8A8Srgb
                                                      : vk::Format::eUndefined;
    const bool sampled =
        format != vk::Format::eUndefined &&
        (instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures &
         vk::FormatFeatureFlagBits::eSampledImage) != vk::FormatFeatureFlags{};
    if (!g.textured && sampled && t.levels > 0) {
        g.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        if (!g.image.TryCreate(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = format,
                .extent = {t.width, t.height, 1},
                .mipLevels = t.levels,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined,
            })) {
            DestroyModelLater(g);
            model_missing[model_id] = true;
            LOG_WARNING(Render_Vulkan, "VR inframe: model s{:05}: texture allocation failed",
                        model_id);
            return nullptr;
        }
        auto view = device.createImageViewUnique({
            .image = g.image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, t.levels, 0, 1},
        });
        if (view.result != vk::Result::eSuccess) {
            DestroyModelLater(g);
            Fail("model texture view");
            return nullptr;
        }
        g.view = std::move(view.value);
        Buffer staging{};
        const std::vector<u8>& bytes = t.Bytes();
        if (!CreateBuffer(staging, bytes.size(), vk::BufferUsageFlagBits::eTransferSrc)) {
            DestroyModelLater(g);
            model_missing[model_id] = true;
            return nullptr;
        }
        std::memcpy(staging.mapped, bytes.data(), bytes.size());
        vmaFlushAllocation(instance.GetAllocator(), staging.allocation, 0, VK_WHOLE_SIZE);
        std::vector<vk::BufferImageCopy> regions;
        for (u32 level = 0; level < t.levels; ++level) {
            regions.push_back({
                .bufferOffset = t.level_offset[level],
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, level, 0, 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {std::max(t.width >> level, 1u), std::max(t.height >> level, 1u),
                                1},
            });
        }
        const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, t.levels, 0, 1};
        const vk::ImageMemoryBarrier2 to_dst{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = g.image,
            .subresourceRange = all,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_dst,
        });
        cmdbuf.copyBufferToImage(staging.buffer, g.image, vk::ImageLayout::eTransferDstOptimal,
                                 regions);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = g.image,
            .subresourceRange = all,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_read,
        });
        DestroyBufferLater(staging);
        g.textured = true;
        ++stats_uploads;
        if (t.share_key != 0) {
            // Shared from now on: the next model with this texture uses this image.
            SharedImage& s = shared_images[t.share_key];
            s.image = std::move(g.image);
            s.view = std::move(g.view);
            s.users = 1;
            g.shared_key = t.share_key;
            g.shared_view = *s.view;
        }
    }
    if (!model->animation.empty()) {
        const size_t bytes = model->animation.size() * sizeof(float);
        if (!CreateBuffer(g.animation, bytes, vk::BufferUsageFlagBits::eStorageBuffer)) {
            DestroyModelLater(g);
            model_missing[model_id] = true; // Do not retry a failed upload every frame.
            return nullptr;
        }
        std::memcpy(g.animation.mapped, model->animation.data(), bytes);
        vmaFlushAllocation(instance.GetAllocator(), g.animation.allocation, 0, VK_WHOLE_SIZE);
        const auto& surface = model->surface;
        g.surface = VideoCore::UniqueImage(device, instance.GetAllocator());
        if (!g.surface.TryCreate(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eBc3UnormBlock,
                .extent = {surface.width, surface.height, 1},
                .mipLevels = surface.levels,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout = vk::ImageLayout::eUndefined})) {
            DestroyModelLater(g);
            model_missing[model_id] = true;
            LOG_WARNING(Render_Vulkan, "VR inframe: model s{:05}: surface allocation failed",
                        model_id);
            return nullptr;
        }
        auto surface_view = device.createImageViewUnique(
            {.image = g.surface,
             .viewType = vk::ImageViewType::e2D,
             .format = vk::Format::eBc3UnormBlock,
             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, surface.levels, 0, 1}});
        if (surface_view.result != vk::Result::eSuccess) {
            DestroyModelLater(g);
            model_missing[model_id] = true;
            return nullptr;
        }
        g.surface_view = std::move(surface_view.value);
        Buffer staging{};
        const std::vector<u8>& surface_bytes = surface.Bytes();
        if (!CreateBuffer(staging, surface_bytes.size(), vk::BufferUsageFlagBits::eTransferSrc)) {
            DestroyModelLater(g);
            model_missing[model_id] = true;
            return nullptr;
        }
        std::memcpy(staging.mapped, surface_bytes.data(), surface_bytes.size());
        vmaFlushAllocation(instance.GetAllocator(), staging.allocation, 0, VK_WHOLE_SIZE);
        std::vector<vk::BufferImageCopy> regions;
        for (u32 l = 0; l < surface.levels; l++)
            regions.push_back({.bufferOffset = surface.level_offset[l],
                               .imageSubresource = {vk::ImageAspectFlagBits::eColor, l, 0, 1},
                               .imageExtent = {surface.width >> l, surface.height >> l, 1}});
        const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, surface.levels, 0,
                                            1};
        const vk::ImageMemoryBarrier2 dst{.srcStageMask = vk::PipelineStageFlagBits2::eNone,
                                          .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                          .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                          .oldLayout = vk::ImageLayout::eUndefined,
                                          .newLayout = vk::ImageLayout::eTransferDstOptimal,
                                          .image = g.surface,
                                          .subresourceRange = all};
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &dst});
        cmdbuf.copyBufferToImage(staging.buffer, g.surface, vk::ImageLayout::eTransferDstOptimal,
                                 regions);
        const vk::ImageMemoryBarrier2 read{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                           .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                           .dstStageMask =
                                               vk::PipelineStageFlagBits2::eFragmentShader,
                                           .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
                                           .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                                           .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                           .image = g.surface,
                                           .subresourceRange = all};
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &read});
        DestroyBufferLater(staging);
    }
    g.last_used = frame;
    LOG_INFO(Render_Vulkan,
             "VR inframe: model s{:05} uploaded ({} triangles, texture {}x{} {} levels{})",
             model_id, g.index_count / 3, t.width, t.height, t.levels,
             !g.textured ? ", NOT textured" : shared_hit ? ", shared" : "");
    return &models.emplace(model_id, std::move(g)).first->second;
}

int VrInFrame::LabelRowOf(const RecorderProxy& cmdbuf, const F::Label& label) {
    const std::string text{label.utf8.data()};
    if (text.empty()) {
        return -1;
    }
    for (u32 i = 0; i < label_rows.size(); ++i) {
        if (label_rows[i].valid && label_rows[i].text == text) {
            label_rows[i].last_used = frame;
            return static_cast<int>(i);
        }
    }
    static thread_local u32 uploads_frame_count = 0;
    static thread_local u64 uploads_frame = 0;
    if (uploads_frame != frame) {
        uploads_frame = frame;
        uploads_frame_count = 0;
    }
    if (uploads_frame_count >= MaxUploadsPerFrame) {
        return -1;
    }
    VR::Assets::LabelBitmap bitmap{};
    if (!VR::Assets::RasterizeLabel(text.c_str(), label.px, bitmap)) {
        return -1;
    }
    bitmap.width = std::min(bitmap.width, AtlasWidth);
    bitmap.height = std::min(bitmap.height, AtlasRow);
    u32 row = 0;
    for (u32 i = 1; i < label_rows.size(); ++i) {
        if (!label_rows[i].valid ||
            (label_rows[row].valid && label_rows[i].last_used < label_rows[row].last_used)) {
            row = i;
        }
        if (!label_rows[row].valid) {
            break;
        }
    }
    if (label_rows[row].valid && label_rows[row].last_used == frame) {
        return -1; // every row is in use this frame
    }
    // The whole row (cleared to 0 beyond the text) so a shorter label leaves no stale texels.
    std::vector<u8> pixels(static_cast<size_t>(AtlasWidth) * AtlasRow * 2, 0);
    const u32 src_width = static_cast<u32>(bitmap.rg.size() / 2 / std::max(bitmap.height, 1u));
    for (u32 y = 0; y < bitmap.height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * AtlasWidth * 2,
                    bitmap.rg.data() + static_cast<size_t>(y) * src_width * 2,
                    static_cast<size_t>(bitmap.width) * 2);
    }
    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = AtlasWidth,
        .bufferImageHeight = AtlasRow,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, static_cast<s32>(row * AtlasRow), 0},
        .imageExtent = {AtlasWidth, AtlasRow, 1},
    };
    if (!Upload(cmdbuf, label_atlas, pixels.data(), pixels.size(), region, false)) {
        return -1;
    }
    ++uploads_frame_count;
    label_rows[row] = {text, bitmap.width, bitmap.height, frame, true};
    return static_cast<int>(row);
}

void VrInFrame::CollectTimes() {
    if (!query_pool) {
        return;
    }
    for (u32 slot = 0; slot < TimingSlots; ++slot) {
        if (!timing_pending[slot] || slot == timing_slot) {
            continue;
        }
        std::array<u64, 2> ticks{};
        const vk::Result r = device.getQueryPoolResults(
            *query_pool, slot * 2, 2, sizeof(ticks), ticks.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (r == vk::Result::eSuccess) {
            timing_pending[slot] = false;
            if (ticks[1] > ticks[0]) {
                stats_gpu_us +=
                    static_cast<double>(ticks[1] - ticks[0]) * timestamp_period_ns * 1e-3;
                ++stats_gpu_samples;
            }
        }
    }
}

bool VrInFrame::Record(const RecorderProxy& cmdbuf, const Target& t, const Camera& camera,
                       const F::RenderList& list) {
    ++frame;
    model_uploads = 0;
    // (Dropped pieces are swept at the frame's end, never here: a piece retired between the two
    // eyes' draws would go from one eye a frame before the other.)
    if (failed || (list.count == 0 && list.world_gray <= 0.0f && list.world_storm <= 0.0f)) {
        return false;
    }
    return RecordList(cmdbuf, t, camera, list);
}

void VrInFrame::SweepDropped() {
    // Runtime models dropped (a severed piece gone): their GPU copies freed now, not when the
    // cache next fills (their ids are never put again).
    if (runtime_held && VR::Assets::DroppedModels() != dropped_models) {
        const u32 dropped = VR::Assets::DroppedModels();
        dropped_models = dropped;
        std::vector<s32> gone;
        for (const auto& [id, model] : models) {
            if (id >= VR::Assets::RuntimeModelBase) {
                gone.push_back(id);
            }
        }
        for (const auto& [id, missing] : model_missing) {
            if (id >= VR::Assets::RuntimeModelBase) {
                gone.push_back(id);
            }
        }
        const std::size_t held = gone.size();
        VR::Assets::KeepDroppedModels(gone);
        for (const s32 id : gone) {
            if (const auto it = models.find(id); it != models.end()) {
                DestroyModelLater(it->second);
                models.erase(it);
            }
            model_missing.erase(id);
        }
        runtime_held = held > gone.size(); // (some still put)
    }
}

bool VrInFrame::RecordList(const RecorderProxy& cmdbuf, const Target& t, const Camera& camera,
                           const F::RenderList& list) {
    const s64 start = NowNs();
    const auto depth_terms = DepthTerms(camera);
    const vk::Pipeline pipeline = PipelineFor(t.color_format, t.depth_format, false, depth_terms);
    if (!pipeline || !EnsureDepth(t)) {
        return false;
    }
    // The x-ray pass (Item::xray) only when a list has any; without its pipeline they are left
    // out (the hovered card as before: behind the fist).
    bool any_xray = false;
    for (u32 i = 0; i < list.count; ++i) {
        any_xray = any_xray || list.items[i].xray;
    }
    const vk::Pipeline xray_pipeline =
        any_xray ? PipelineFor(t.color_format, t.depth_format, true, depth_terms) : vk::Pipeline{};
    // Asked every frame (built on a worker the first time): ready before a flash needs it.
    const vk::Pipeline translucent_pipeline =
        TranslucentFor(t.color_format, t.depth_format, depth_terms);
    if (!label_atlas_ready) {
        // Undefined -> cleared -> sampled, once.
        const vk::ImageMemoryBarrier2 to_dst{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = label_atlas,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_dst,
        });
        cmdbuf.clearColorImage(label_atlas, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                               ColorRange);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = label_atlas,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_read,
        });
        label_atlas_ready = true;
    }
    if (!white_ready) {
        // Undefined -> white -> sampled, once (an untextured model samples it).
        const vk::ImageMemoryBarrier2 to_dst{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = white,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_dst,
        });
        cmdbuf.clearColorImage(white, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearColorValue{std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}},
                               ColorRange);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = white,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_read,
        });
        white_ready = true;
    }

    // Textures and label sizes first (uploads are outside the render pass).
    struct Prepared {
        DrawConstants push{};
        vk::ImageView view{};
        vk::Sampler sampler{};
        const GpuModel* model{nullptr}; // MeshId::Model: its own buffers
        bool skip{false};
    };
    std::array<Prepared, F::MaxRenderItems> prepared{};
    std::array<int, F::MaxLabels> label_row{};
    label_row.fill(-2);
    const auto row_of = [&](int label) {
        if (label < 0 || label >= static_cast<int>(list.label_count)) {
            return -1;
        }
        if (label_row[static_cast<size_t>(label)] == -2) {
            label_row[static_cast<size_t>(label)] =
                LabelRowOf(cmdbuf, list.labels[static_cast<size_t>(label)]);
        }
        return label_row[static_cast<size_t>(label)];
    };
    const float tan_half = std::tan(0.5f * camera.vfov);
    const float p11 = tan_half > 1e-4f ? 1.0f / tan_half : 1.0f;
    // The eye's projection, pushed (the shader's proj): the game's P00 / P11 times the eye's
    // clip scale, and the clip offset. The depth mapping is the pipeline's (DepthTerms).
    const std::array<float, 4> proj{p11 / std::max(camera.aspect, 1e-3f) * camera.clip[0],
                                    p11 * camera.clip[1], camera.clip[2], camera.clip[3]};
    const float exposure = list.exposure;
    u32 drawn = 0;
    u32 optical_hidden = 0;
    std::array<u32, 16> waiting_groups{}; // a group with a model not up yet: none of it drawn
    std::size_t waiting = 0;
    const auto group_waits = [&](u32 group) {
        return std::find(waiting_groups.begin(), waiting_groups.begin() + waiting, group) !=
               waiting_groups.begin() + waiting;
    };
    // Held/body-slot models get the upload budget before severed pieces. Keep their result
    // for preparation below (including a miss), so each is requested only once. ModelOf marks
    // cached entries used this frame: the later piece uploads cannot evict these pointers.
    for (u32 i = 0; i < list.count; ++i) {
        const F::Item& item = list.items[i];
        if (item.mesh == F::MeshId::Model && item.group == 0 &&
            !(item.hide_from_monocular_eye &&
              camera.monocular_eye == static_cast<s32>(t.depth_layer))) {
            prepared[i].model = ModelOf(cmdbuf, static_cast<s32>(item.tex_key));
        }
    }
    // Pending cuts are upload-only, including outside the frustum. Count every model of
    // each cut; a truncated list must never acknowledge a partial piece/stump. Commands
    // are recorded before the later frame that hides the limb, on the same scheduler.
    // No GPU fence wait: pinned models and queue ordering keep the handoff non-blocking.
    struct UploadGroup { u32 token{}, count{}; bool preload{}; };
    std::array<UploadGroup, VR::Sever::RevealMailbox::Slots> uploading{};
    std::size_t uploading_count = 0;
    for (u32 i = 0; i < list.count; ++i) {
        const auto& item = list.items[i];
        if (item.mesh != F::MeshId::Model || item.group == 0) continue;
        // A first draw is acknowledged only while its cut still holds its reveal slot: the slot
        // goes back once the cut is drawn (bloodborne_sever.cpp RevealDone), and a list built
        // before that but drawn after it (the game runs a frame ahead) is an ordinary group,
        // never one waiting on a slot that is gone (a piece blinking out for a frame).
        const bool tracked = item.upload_only ||
                             (item.reveal_first && VR::Sever::g_reveals.Live(item.group));
        if (!group_waits(item.group) &&
            ModelOf(cmdbuf, static_cast<s32>(item.tex_key), tracked ? item.group : 0) == nullptr &&
            waiting < waiting_groups.size()) waiting_groups[waiting++] = item.group;
        // Fold tracking into the existing group-upload pass. Settled/ordinary items
        // neither access the mailbox nor acquire another lock.
        if (!tracked) continue;
        auto end = uploading.begin() + uploading_count;
        auto it = std::find_if(uploading.begin(), end,
                               [&](const auto& g) { return g.token == item.group; });
        if (it == end) {
            if (uploading_count == uploading.size()) continue;
            it = uploading.begin() + uploading_count++;
            it->token = item.group;
            it->preload = item.upload_only;
        }
        ++it->count;
    }
    for (std::size_t i = 0; i < uploading_count; ++i) {
        const auto& g = uploading[i];
        if (VR::Sever::g_reveals.Waiting(g.token, g.count) && !group_waits(g.token) &&
            waiting < waiting_groups.size()) waiting_groups[waiting++] = g.token;
        // Refresh readiness during the first draw too: switching mono/native stereo
        // after hiding must not leave the reveal waiting forever on the old eye mask.
        // Both the replacement meshes and AddGush use the regular pipeline, already
        // checked at entry. Unrelated translucent-effect compilation must not block a cut.
        if (!group_waits(g.token))
            VR::Sever::g_reveals.Uploaded(g.token, g.count, t.depth_layer, camera.model_eye_mask);
    }
    // Left-handed play (core/vr/mirror_world.h): the finished eyes are flipped left to right.
    const bool mirror = VR::MirrorWorld();
    for (u32 i = 0; i < list.count; ++i) {
        const F::Item& item = list.items[i];
        Prepared& p = prepared[i];
        if (item.upload_only) {
            p.skip = true;
            continue;
        }
        if (item.group != 0 && group_waits(item.group)) {
            p.skip = true;
            continue;
        }
        if (item.hide_from_monocular_eye && camera.monocular_eye == static_cast<s32>(t.depth_layer)) {
            p.skip = true;
            ++optical_hidden;
            continue;
        }
        p.push.mv = item.mv;
        p.push.mv[3] -= camera.eye_x;
        p.push.color = item.color;
        p.push.emissive = item.emissive;
        p.push.uv = item.uv;
        p.push.proj = proj;
        // z, w: 1 / the frame's size (the probe's coordinates); z < 0: read it swapped (B G R);
        // both 0: no probe this frame (a neutral scene).
        const float inv_w = probe_valid ? 1.0f / static_cast<float>(t.extent.width) : 0.0f;
        const float inv_h = probe_valid ? 1.0f / static_cast<float>(t.extent.height) : 0.0f;
        // The cards' round silhouettes are anti-aliased in the shader (EdgeDisc / EdgeRing).
        const u32 edge = item.candle                    ? Candle
                         : item.mesh == F::MeshId::Plate ? EdgeDisc
                         : item.mesh == F::MeshId::Ring  ? EdgeRing
                                                         : 0u;
        // Bits 6 and up: Item::depth_pull in 1e-4 game units (vr_inframe.vert).
        const u32 pull = static_cast<u32>(std::clamp(item.depth_pull * 1e4f, 0.0f, 200000.0f));
        p.push.extra = {static_cast<float>(static_cast<u32>(item.shade) + edge + (pull << 6)),
                        exposure, t.swapped ? -inv_w : inv_w, inv_h};
        if (item.shade == F::Shade::Steel) {
            // The needle's least width on screen is in pixels of this frame (vr_inframe.vert).
            p.push.uv[1] = 1.0f / static_cast<float>(std::max(t.extent.height, 1u));
        }
        p.view = *label_view;
        p.sampler = *sampler;
        if (item.mesh == F::MeshId::Model) {
            if (item.group != 0) {
                // The same pin as the upload pass (a first draw keeps it until it is visible).
                p.model = ModelOf(cmdbuf, static_cast<s32>(item.tex_key),
                                  item.reveal_first ? item.group : 0);
            }
            if (p.model == nullptr) {
                p.skip = true;
                continue;
            }
            if (p.model->animation.buffer) {
                p.push.uv = {std::isfinite(item.model_frame)
                                 ? std::clamp(item.model_frame, 0.0f, 254.0f)
                                 : 0.0f,
                             static_cast<float>(p.model->vertex_count),
                             item.soft_glove ? -3.0f : p.model->single_surface ? -2.0f : -1.0f,
                             static_cast<float>(3 * p.model->animation_bones)};
                if (p.model->single_surface && !item.soft_glove &&
                    std::isfinite(item.model_blend_frame) && item.model_blend_frame >= 0.0f) {
                    // Keep the 128-byte push layout: z selects scaled-bone skinning and
                    // second frame; fractional w carries half the crossfade weight.
                    p.push.uv[2] = -4.0f - std::clamp(item.model_blend_frame, 0.0f, 254.0f);
                    p.push.uv[3] += std::isfinite(item.model_blend_weight)
                        ? .5f * std::clamp(item.model_blend_weight, 0.0f, 1.0f) : 0.0f;
                }
            }
            if (item.tex == F::Tex::Model) {
                p.view = p.model->textured ? p.model->View() : *white_view;
                p.sampler = *model_sampler;
            }
        }
        if (item.tex == F::Tex::None && item.shade == F::Shade::Model) {
            p.view = *white_view;
        }
        if (item.tex == F::Tex::Icon) {
            p.view = IconView(cmdbuf, item.tex_key);
            if (!p.view) {
                p.skip = true;
                continue;
            }
        } else if (item.tex == F::Tex::Label) {
            const int row = row_of(static_cast<int>(item.tex_key));
            if (row < 0) {
                p.skip = true;
                continue;
            }
            const LabelRow& r = label_rows[static_cast<size_t>(row)];
            const float v0 = static_cast<float>(row * AtlasRow) / AtlasHeight;
            p.push.uv = {0.0f, v0, static_cast<float>(r.width) / AtlasWidth,
                         v0 + static_cast<float>(r.height) / AtlasHeight};
            ++stats_labels;
        }
        if (item.fit_label >= 0) {
            const int row = row_of(item.fit_label);
            if (row < 0) {
                p.skip = true;
                continue;
            }
            const LabelRow& r = label_rows[static_cast<size_t>(row)];
            const float aspect =
                static_cast<float>(r.width) / static_cast<float>(std::max(r.height, 1u));
            const float k = aspect + item.fit_pad;
            p.push.mv[0] *= k;
            p.push.mv[4] *= k;
            p.push.mv[8] *= k;
        }
        // Left-handed play: what is meant to be read (a label, a count, an item's icon: centred
        // quads) is drawn mirrored here, its X axis reversed about its own centre, so it reads
        // right once the eye is flipped; the hold ring with it (it fills clockwise). No culling
        // here and normals follow the transform (vr_inframe.vert), so nothing else changes.
        if (mirror && ((item.mesh == F::MeshId::Quad &&
                        (item.shade == F::Shade::Text || item.tex == F::Tex::Icon)) ||
                       item.shade == F::Shade::HoldRing)) {
            p.push.mv[0] = -p.push.mv[0];
            p.push.mv[4] = -p.push.mv[4];
            p.push.mv[8] = -p.push.mv[8];
        }
        ++drawn;
    }
    // The backdrop: A Call Beyond's gray world, or Tonitrus' strike's flash (which grays no item).
    const bool gray_world = (list.world_gray > 0.0f || list.world_storm > 0.0f) &&
                            translucent_pipeline && probe_valid;
    if (drawn == 0 && optical_hidden == 0 && !gray_world) {
        return false;
    }

    if (query_pool) {
        CollectTimes();
        timing_slot = (timing_slot + 1) % TimingSlots;
        cmdbuf.resetQueryPool(*query_pool, timing_slot * 2, 2);
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *query_pool,
                               timing_slot * 2);
    }
    // The scene depth into the private copy (depth only; the game's image is only read). In place
    // (S1's own depth): no copy, its layer becomes the attachment after the probe's read.
    const vk::ImageAspectFlags depth_aspects =
        HasStencil(t.depth_format) ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                                   : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth};
    vk::ImageMemoryBarrier2 to_attachment{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                        vk::PipelineStageFlagBits2::eLateFragmentTests,
        .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .image = depth,
        .subresourceRange = {depth_aspects, 0, 1, 0, 1},
    };
    depth_copied = !t.depth_in_place;
    if (t.depth_in_place) {
        if (camera.collect_probe) {
            RecordProbe(cmdbuf, t, camera, list);
        }
        // From the scale's layout (its writes were made visible to transfer reads, and this
        // chains on them); the probe's read is the only access since.
        to_attachment.srcAccessMask = vk::AccessFlagBits2::eNone;
        to_attachment.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        to_attachment.image = t.depth_image;
        to_attachment.subresourceRange = {depth_aspects, 0, 1, t.depth_layer, 1};
    } else {
        const vk::ImageMemoryBarrier2 copy_in{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = depth,
            .subresourceRange = {depth_aspects, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &copy_in,
        });
        const vk::ImageCopy copy{
            .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, t.depth_layer, 1},
            .srcOffset = {0, 0, 0},
            .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .dstOffset = {0, 0, 0},
            .extent = {t.depth_extent.width, t.depth_extent.height, 1},
        };
        cmdbuf.copyImage(t.depth_image, vk::ImageLayout::eTransferSrcOptimal, depth,
                         vk::ImageLayout::eTransferDstOptimal, copy);
        if (camera.collect_probe) {
            RecordProbe(cmdbuf, t, camera, list);
        }
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_attachment,
    });

    // The coverage starts at 0 every frame (Undefined: last frame's warp copy is long done).
    const vk::ImageMemoryBarrier2 cover_in{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eColorAttachmentRead,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = cover,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &cover_in,
    });
    const std::array<vk::RenderingAttachmentInfo, 2> color_attachments{{
        {
            .imageView = t.color_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
        {
            .imageView = *cover_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{}}},
        },
    }};
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = t.depth_in_place ? t.depth_in_place : *depth_attachment_view,
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::Extent2D area{std::min(t.extent.width, t.depth_extent.width),
                            std::min(t.extent.height, t.depth_extent.height)};
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, area},
        .layerCount = 1,
        .colorAttachmentCount = static_cast<u32>(color_attachments.size()),
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = &depth_attachment,
    });
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    const vk::Viewport viewport{0.0f,
                                0.0f,
                                static_cast<float>(t.extent.width),
                                static_cast<float>(t.extent.height),
                                t.zmin,
                                t.zmax};
    cmdbuf.setViewport(0, viewport);
    cmdbuf.setScissor(0, vk::Rect2D{{0, 0}, area});
    const vk::DeviceSize zero = 0;
    const GpuModel* bound = nullptr; // the buffers bound: a model's, or (nullptr) the library's
    bool library_bound = false;
    const GpuModel* animation_bound = nullptr;
    bool animation_descriptors_ready = false;
    // Two passes: the items, then the x-ray copies (their own pipeline; the viewport and the
    // scissor are dynamic state and stay set).
    vk::Pipeline bound_pipeline = pipeline;
    Prepared backdrop{};
    if (gray_world) {
        backdrop.view = *probe_view;
        backdrop.sampler = *sampler;
        backdrop.push.color[3] = list.world_gray;
        backdrop.push.color[0] = list.world_bright; // its lift (the frag's gray backdrop)
        backdrop.push.color[1] = list.world_flash;  // its flare once raised
        backdrop.push.color[2] = list.world_storm;  // Tonitrus' strike: lightning's flash
        // Reuse this eye's scene-light probe. Hardware blending preserves the original
        // full-resolution scene in proportion to the color still left during the fade.
        const bool linear = probe_format == vk::Format::eR8G8B8A8Srgb ||
                            probe_format == vk::Format::eB8G8R8A8Srgb;
        backdrop.push.extra = {linear ? 1.0f : 0.0f, t.swapped ? 1.0f : 0.0f,
                               1.0f / t.extent.width, 1.0f / t.extent.height};
    }
    for (int n = gray_world ? -1 : 0; n < static_cast<int>(2 * list.count); ++n) {
        const bool background = n < 0;
        const u32 i = background ? 0 : static_cast<u32>(n) % list.count;
        const bool xray_pass = !background && static_cast<u32>(n) >= list.count;
        if (xray_pass && n == list.count) {
            if (!xray_pipeline) {
                break; // none (or no pipeline for them): one pass
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, xray_pipeline);
            bound_pipeline = xray_pipeline;
        }
        const Prepared& p = background ? backdrop : prepared[i];
        if (p.skip || (!background && list.items[i].xray != xray_pass)) {
            continue;
        }
        const auto wanted_pipeline = background ? translucent_pipeline : xray_pass ? xray_pipeline
            : list.items[i].translucent ? translucent_pipeline : pipeline;
        if (!wanted_pipeline) continue;
        if (wanted_pipeline != bound_pipeline) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, wanted_pipeline);
            bound_pipeline = wanted_pipeline;
        }
        if (p.model != nullptr && p.model != bound) {
            cmdbuf.bindVertexBuffers(0, p.model->vertices.buffer, zero);
            cmdbuf.bindIndexBuffer(p.model->indices.buffer, 0, p.model->index_type);
            bound = p.model;
            library_bound = false;
        } else if (p.model == nullptr && !library_bound) {
            cmdbuf.bindVertexBuffers(0, vertices.buffer, zero);
            cmdbuf.bindIndexBuffer(indices.buffer, 0, vk::IndexType::eUint32);
            library_bound = true;
            bound = nullptr;
        }
        const vk::DescriptorImageInfo image_info{
            .sampler = p.sampler,
            .imageView = p.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const vk::DescriptorImageInfo probe_info{
            .sampler = *sampler,
            .imageView = probe_valid ? *probe_view : *label_view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        std::array<vk::WriteDescriptorSet, 4> writes{{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &image_info,
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &probe_info,
            },
        }};
        const GpuModel* animated = p.model && p.model->animation.buffer ? p.model : nullptr;
        vk::DescriptorBufferInfo animation_info{};
        vk::DescriptorImageInfo surface_info{};
        u32 write_count = 2;
        if (!animation_descriptors_ready || animated != animation_bound) {
            animation_info = vk::DescriptorBufferInfo{
                .buffer = animated ? animated->animation.buffer : vertices.buffer,
                .offset = 0,
                .range = animated ? VK_WHOLE_SIZE : 16};
            surface_info = vk::DescriptorImageInfo{
                .sampler = *model_sampler,
                .imageView = animated ? *animated->surface_view : *white_view,
                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
            writes[2] = vk::WriteDescriptorSet{.dstBinding = 2,
                                               .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eStorageBuffer,
                                               .pBufferInfo = &animation_info};
            writes[3] =
                vk::WriteDescriptorSet{.dstBinding = 3,
                                       .descriptorCount = 1,
                                       .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                       .pImageInfo = &surface_info};
            write_count = 4;
            animation_bound = animated;
            animation_descriptors_ready = true;
        }
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0,
                                    {write_count, writes.data()});
        ScratchPush::Invalidate();
        cmdbuf.pushConstants(*pipeline_layout,
                             vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                             0, sizeof(DrawConstants), &p.push);
        if (background) {
            cmdbuf.draw(3, 1, 0, 256); // fullscreen at near depth, no depth or coverage write
            continue;
        }
        const u32 gray = gray_world && !list.items[i].keep_charge_color
                             ? static_cast<u32>(std::lround(255 * list.world_gray)) : 0;
        if (p.model != nullptr) {
            cmdbuf.drawIndexed(p.model->index_count, 1, 0, 0, gray);
            continue;
        }
        const MeshRange& r = ranges[static_cast<size_t>(list.items[i].mesh)];
        cmdbuf.drawIndexed(r.index_count, 1, r.first_index, r.vertex_offset, gray);
    }
    cmdbuf.endRendering();
    if (query_pool) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *query_pool,
                               timing_slot * 2 + 1);
        timing_pending[timing_slot] = true;
    }
    // The copy, ours included, for the DIBR prep pass (compute) or nothing (AER).
    vk::ImageMemoryBarrier2 to_read = to_attachment;
    to_read.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    to_read.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    to_read.dstStageMask =
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader;
    to_read.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    to_read.oldLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    to_read.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    // The coverage, for the DIBR capture's copy (VrStereoWarp::CaptureItemMask).
    const vk::ImageMemoryBarrier2 cover_out{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = cover,
        .subresourceRange = ColorRange,
    };
    // In place: S1's depth stays an attachment (nothing reads it after; the next frame's scale
    // starts it from Undefined).
    const std::array<vk::ImageMemoryBarrier2, 2> after{cover_out, to_read};
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = t.depth_in_place ? 1u : static_cast<u32>(after.size()),
        .pImageMemoryBarriers = after.data(),
    });
    ++stats_lists;
    stats_items += drawn;
    for (std::size_t i = 0; i < uploading_count; ++i) {
        const auto& g = uploading[i];
        if (!g.preload && !group_waits(g.token))
            VR::Sever::g_reveals.Drawn(g.token, g.count, t.depth_layer, camera.model_eye_mask);
    }
    const double cpu = static_cast<double>(NowNs() - start) * 1e-3;
    stats_cpu_us += cpu;
    stats_cpu_max_us = std::max(stats_cpu_max_us, cpu);
    return true;
}

// The depth probe (F::Probe, the gauge's fit to the arm): each point projected like our items
// (the record's camera); the game's depth in the box round all of them (at most ProbeBox texels
// square, a point outside it is not read) copied into a readback slot in ONE copy (75 one-texel
// copies cost the pass about 60 us), published once the GPU is past it (CollectProbes). Only a
// 32-bit float depth (the game's D32 S8) is read.
void VrInFrame::RecordProbe(const RecorderProxy& cmdbuf, const Target& t, const Camera& camera,
                            const F::RenderList& list) {
    CollectProbes();
    const F::Probe& probe = list.probe;
    if (probe.id == 0 || probe.count == 0 ||
        (t.depth_format != vk::Format::eD32SfloatS8Uint &&
         t.depth_format != vk::Format::eD32Sfloat)) {
        return;
    }
    constexpr vk::DeviceSize SlotBytes = ProbeBox * ProbeBox * sizeof(float);
    if (!probe_buffer.buffer && !CreateReadback(probe_buffer, ProbeSlots * SlotBytes)) {
        return;
    }
    // A free slot (none: this probe is skipped, the next frame asks again).
    u32 slot = ProbeSlots;
    for (u32 i = 0; i < ProbeSlots; ++i) {
        if (!probe_slots[i].pending) {
            slot = i;
            break;
        }
    }
    if (slot == ProbeSlots) {
        return;
    }
    ProbeSlot& ps = probe_slots[slot];
    ps.result = {};
    ps.result.id = probe.id;
    ps.result.record = list.record;
    ps.result.count = std::min(probe.count, F::MaxProbe);
    const float tan_half = std::tan(0.5f * camera.vfov);
    const float p11 = tan_half > 1e-4f ? 1.0f / tan_half : 1.0f;
    const float p00 = p11 / std::max(camera.aspect, 1e-3f);
    // Each point's texel, then the box round them, centred on their middle when too large.
    s32 bx0 = std::numeric_limits<s32>::max(), by0 = bx0, bx1 = -1, by1 = -1;
    for (u32 i = 0; i < ps.result.count; ++i) {
        const auto& q = probe.points[i];
        ps.result.point_z[i] = q[2];
        ps.texel[i] = false;
        if (!(q[2] > camera.near_z)) {
            continue;
        }
        const float nx = camera.clip[0] * p00 * (q[0] - camera.eye_x) / q[2] + camera.clip[2];
        const float ny = camera.clip[1] * -p11 * q[1] / q[2] + camera.clip[3];
        const float x = (nx * 0.5f + 0.5f) * static_cast<float>(t.extent.width);
        const float y = (ny * 0.5f + 0.5f) * static_cast<float>(t.extent.height);
        if (!(x >= 0.0f && y >= 0.0f && x < static_cast<float>(t.depth_extent.width) &&
              y < static_cast<float>(t.depth_extent.height))) {
            continue;
        }
        ps.texel[i] = true;
        ps.tx[i] = static_cast<s32>(x);
        ps.ty[i] = static_cast<s32>(y);
        bx0 = std::min(bx0, ps.tx[i]);
        by0 = std::min(by0, ps.ty[i]);
        bx1 = std::max(bx1, ps.tx[i]);
        by1 = std::max(by1, ps.ty[i]);
    }
    if (bx1 >= bx0 && by1 >= by0) {
        const auto fit = [](s32& lo, s32& hi, s32 limit) {
            if (hi - lo + 1 > static_cast<s32>(ProbeBox)) {
                const s32 mid = (lo + hi) / 2;
                lo = std::clamp(mid - static_cast<s32>(ProbeBox) / 2, 0,
                                std::max(limit - static_cast<s32>(ProbeBox), 0));
                hi = std::min(lo + static_cast<s32>(ProbeBox) - 1, limit - 1);
            }
        };
        fit(bx0, bx1, static_cast<s32>(t.depth_extent.width));
        fit(by0, by1, static_cast<s32>(t.depth_extent.height));
        ps.box = {bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1};
        for (u32 i = 0; i < ps.result.count; ++i) {
            ps.texel[i] = ps.texel[i] && ps.tx[i] >= bx0 && ps.tx[i] <= bx1 && ps.ty[i] >= by0 &&
                          ps.ty[i] <= by1;
        }
        const vk::BufferImageCopy region{
            .bufferOffset = slot * SlotBytes,
            .bufferRowLength = static_cast<u32>(ps.box[2]),
            .bufferImageHeight = static_cast<u32>(ps.box[3]),
            .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, t.depth_layer, 1},
            .imageOffset = {bx0, by0, 0},
            .imageExtent = {static_cast<u32>(ps.box[2]), static_cast<u32>(ps.box[3]), 1},
        };
        cmdbuf.copyImageToBuffer(t.depth_image, vk::ImageLayout::eTransferSrcOptimal,
                                 probe_buffer.buffer, region);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &to_host,
        });
    }
    // Depth -> view z: d = zmin + (zmax - zmin) (A + B / z), A = f / (f - n), B = -n f / (f - n).
    const float nz = camera.near_z;
    const float fz = camera.far_z;
    ps.a = fz / (fz - nz);
    ps.b = -nz * fz / (fz - nz);
    ps.zmin = t.zmin;
    ps.zmax = t.zmax;
    ps.tick = scheduler.CurrentTick();
    ps.pending = true;
}

// Probe slots the GPU is past: their depths as view z, published (the newest one wins).
void VrInFrame::CollectProbes() {
    if (!probe_buffer.buffer) {
        return;
    }
    for (u32 slot = 0; slot < ProbeSlots; ++slot) {
        ProbeSlot& ps = probe_slots[slot];
        if (!ps.pending || !scheduler.IsFree(ps.tick)) {
            continue;
        }
        ps.pending = false;
        constexpr vk::DeviceSize SlotBytes = ProbeBox * ProbeBox * sizeof(float);
        vmaInvalidateAllocation(instance.GetAllocator(), probe_buffer.allocation, slot * SlotBytes,
                                SlotBytes);
        const float* depths =
            static_cast<const float*>(probe_buffer.mapped) + slot * ProbeBox * ProbeBox;
        const float range = ps.zmax - ps.zmin;
        for (u32 i = 0; i < ps.result.count; ++i) {
            ps.result.scene_z[i] = -1.0f;
            if (!ps.texel[i] || std::abs(range) < 1e-9f) {
                continue;
            }
            const s32 at = (ps.ty[i] - ps.box[1]) * ps.box[2] + (ps.tx[i] - ps.box[0]);
            const float ndc = (depths[at] - ps.zmin) / range;
            const float den = ndc - ps.a;
            if (std::abs(den) < 1e-12f || !(ndc >= 0.0f && ndc <= 1.0f)) {
                continue;
            }
            const float z = ps.b / den;
            ps.result.scene_z[i] = z > 0.0f ? z : -1.0f;
        }
        F::PublishProbe(ps.result);
    }
}

bool VrInFrame::CreateReadback(Buffer& out, vk::DeviceSize size) {
    const VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = std::max<vk::DeviceSize>(size, 16),
        .usage = static_cast<VkBufferUsageFlags>(vk::BufferUsageFlagBits::eTransferDst),
    };
    const VmaAllocationCreateInfo alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer{};
    VmaAllocationInfo info_out{};
    if (vmaCreateBuffer(instance.GetAllocator(), &info, &alloc, &buffer, &out.allocation,
                        &info_out) != VK_SUCCESS) {
        return false;
    }
    out.buffer = vk::Buffer{buffer};
    out.mapped = info_out.pMappedData;
    return out.mapped != nullptr;
}

void VrInFrame::LogStats() {
    const s64 now = NowNs();
    if (now - stats_since_ns < 10'000'000'000LL) {
        return;
    }
    const auto in = VR::InFrame::GetStats();
    if (stats_lists > 0) {
        LOG_INFO(Render_Vulkan,
                 "VR inframe: STATS {} lists drawn, {:.1f} items each, {} labels, {} uploads | "
                 "GPU {:.1f} us avg ({} samples) | record CPU {:.1f} us avg, {:.1f} max | lists "
                 "submitted {} taken {}",
                 stats_lists, static_cast<double>(stats_items) / stats_lists, stats_labels,
                 stats_uploads, stats_gpu_samples > 0 ? stats_gpu_us / stats_gpu_samples : 0.0,
                 stats_gpu_samples, stats_cpu_us / stats_lists, stats_cpu_max_us, in.submitted,
                 in.taken);
    }
    stats_since_ns = now;
    stats_lists = stats_items = stats_labels = stats_uploads = stats_gpu_samples = 0;
    stats_gpu_us = stats_cpu_us = stats_cpu_max_us = 0.0;
}

} // namespace Vulkan
