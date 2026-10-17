// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"
namespace Vulkan::VrOverlay {
// A 10.4 cm leather belt pouch around a rigid 2 x 2 apothecary insert.
// Local -Z is its front; a short folded flap keeps all four vial tops accessible.
// One static mesh/draw: aux.x selects leather (0), brass (1), lining (2), blood glass (3).
// No textures, transparency sorting, cloth simulation, or per-vial physics.
inline MeshData ApothecaryCaseMesh() {
    MeshData mesh{};
    auto add = [&](MeshData part, Mat4 transform, float material) {
        for (auto &v : part.vertices)
            v.aux.x = material;
        TransformMesh(part, transform);
        AppendMesh(mesh, part);
    };
    auto box = [&](Vec3 centre, Vec3 half, float radius, float material, float angle = 0.0f) {
        add(RoundedBox(half, radius, 1), Mat4::Translation(centre) * Mat4::RotationX(angle),
            material);
    };
    // Rounded leather shell: soft binding outside, a rigid divided insert inside.
    box({0, -.108f, 0}, {.052f, .007f, .055f}, .006f, 0);
    box({0, -.043f, -.052f}, {.052f, .066f, .006f}, .005f, 0);
    box({0, -.038f, .052f}, {.052f, .071f, .006f}, .005f, 0);
    for (float x : {-.046f, .046f})
        box({x, -.043f, 0}, {.006f, .067f, .052f}, .005f, 0);
    // Short folded flap stays within the back fittings, so the case can sit flush on a belt.
    box({0, .049f, .030f}, {.052f, .035f, .005f}, .004f, 0, 1.10f);
    box({0, .047f, .029f}, {.045f, .028f, .0015f}, .001f, 2, 1.10f);
    box({0, -.098f, 0}, {.043f, .003f, .045f}, .002f, 2);
    // A cross divider gives two rows of two, with clearance around the syringe caps.
    box({0, -.040f, 0}, {.002f, .058f, .046f}, .0015f, 2);
    for (float x : {-.023f, .023f})
        box({x, -.040f, 0}, {.021f, .058f, .002f}, .0015f, 2);
    // Narrow stitched reinforcements and compact, tarnished belt fittings.
    for (float x : {-.037f, .037f}) {
        box({x, -.043f, -.059f}, {.008f, .066f, .002f}, .0015f, 0);
        box({x, .020f, .060f}, {.009f, .029f, .003f}, .002f, 0);
        box({x, .038f, .064f}, {.011f, .009f, .0025f}, .002f, 1);
        box({x, .038f, .061f}, {.007f, .005f, .003f}, .002f, 0);
        for (float y : {-.095f, .012f}) {
            const std::array<ProfilePoint, 4> profile{
                {{0, 0}, {.0032f, 0}, {.0032f, .0015f}, {0, .0025f}}};
            add(Lathe(profile, 10), Mat4::Translation({x, y, -.062f}) * Mat4::RotationX(-Pi * .5f),
                1);
        }
    }
    for (float x : {-.029f, .029f})
        box({x, .031f, .053f}, {.010f, .005f, .006f}, .003f, 1);
    box({0, -.001f, -.060f}, {.013f, .012f, .002f}, .003f, 1);
    box({0, -.009f, -.064f}, {.004f, .014f, .002f}, .001f, 1);
    return mesh;
}
// Open single-item leather holder; unit bounds, fitted to the item's actual dimensions.
inline MeshData QuickHolsterMesh() {
    MeshData mesh{};
    auto box = [&](Vec3 centre, Vec3 half, float material) {
        auto part = RoundedBox(half, .025f, 1);
        for (auto &v : part.vertices) v.aux.x = material;
        TransformMesh(part, Mat4::Translation(centre));
        AppendMesh(mesh, part);
    };
    box({0,0,.46f},{.5f,.5f,.04f},0);
    box({0,-.46f,0},{.5f,.04f,.5f},0);
    for (float x : {-.46f,.46f}) box({x,-.16f,0},{.04f,.34f,.5f},0);
    box({0,-.23f,-.46f},{.5f,.27f,.04f},0);
    for (float x : {-.35f,.35f}) box({x,-.10f,-.51f},{.045f,.045f,.015f},1);
    // Keep local coordinates in metres so the shared leather shader has the same grain
    // scale as the vial case (the caller fits this nominal 10 cm holder to the item).
    TransformMesh(mesh, Mat4::Scale({.1f,.1f,.1f}));
    return mesh;
}
} // namespace Vulkan::VrOverlay
