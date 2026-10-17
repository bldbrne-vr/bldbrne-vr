// SPDX-License-Identifier: GPL-2.0-or-later

// VR overlay meshes and draw lists (video_core/renderer_vulkan/vr_overlay_meshes.*), no Vulkan:
// every procedural mesh is watertight with outward winding and consistent unit normals, the vial
// has its designed size, the reversed-Z projection maps the FOV edges and depth as Vulkan
// expects (and keeps CCW-outside faces front facing), the vial sits in the grip as designed,
// the vial animator turns arm / X again / X held / injection / tracking loss into the right
// visuals, and draw lists come out in pipeline order with a depth prepass for every blended
// front-surface item.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

namespace {

using namespace Vulkan::VrOverlay;

constexpr float Dt = 1.0f / 90.0f;

// Welds vertices closer than 1 um (rings duplicated for creases share positions).
std::vector<u32> Weld(const std::vector<Vertex>& vertices) {
    constexpr float cell = 1e-5f, tol = 1e-6f;
    std::unordered_map<long long, std::vector<u32>> grid;
    const auto key = [](long long x, long long y, long long z) {
        return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL);
    };
    std::vector<Vec3> reps;
    std::vector<u32> id(vertices.size());
    for (u32 i = 0; i < vertices.size(); ++i) {
        const Vec3 p = vertices[i].position;
        const long long cx = std::llround(std::floor(p.x / cell));
        const long long cy = std::llround(std::floor(p.y / cell));
        const long long cz = std::llround(std::floor(p.z / cell));
        u32 found = ~0u;
        for (int dx = -1; dx <= 1 && found == ~0u; ++dx) {
            for (int dy = -1; dy <= 1 && found == ~0u; ++dy) {
                for (int dz = -1; dz <= 1 && found == ~0u; ++dz) {
                    const auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) {
                        continue;
                    }
                    for (const u32 r : it->second) {
                        if (std::abs(reps[r].x - p.x) <= tol && std::abs(reps[r].y - p.y) <= tol &&
                            std::abs(reps[r].z - p.z) <= tol) {
                            found = r;
                            break;
                        }
                    }
                }
            }
        }
        if (found == ~0u) {
            found = static_cast<u32>(reps.size());
            reps.push_back(p);
            grid[key(cx, cy, cz)].push_back(found);
        }
        id[i] = found;
    }
    return id;
}

TEST(VrOverlayMeshes, EveryMeshIsValid) {
    u32 total = 0;
    for (u32 m = 0; m < MeshCount; ++m) {
        const MeshId id = static_cast<MeshId>(m);
        const MeshData mesh = BuildMesh(id);
        SCOPED_TRACE(MeshName(id));
        ASSERT_FALSE(mesh.indices.empty());
        ASSERT_EQ(mesh.indices.size() % 3, 0u);
        for (const u32 i : mesh.indices) {
            ASSERT_LT(i, mesh.vertices.size());
        }
        for (const Vertex& v : mesh.vertices) {
            ASSERT_NEAR(Length(v.normal), 1.0f, 1e-3f);
            ASSERT_TRUE(std::isfinite(v.position.x + v.position.y + v.position.z));
        }
        const bool closed = id != MeshId::LaserBeam && id != MeshId::Vignette;
        EXPECT_EQ(mesh.closed, closed);
        const std::vector<u32> weld = Weld(mesh.vertices);
        std::map<std::pair<u32, u32>, int> edges;
        u32 degenerate = 0, flipped = 0;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const u32 a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
            const Vec3 n = Cross(mesh.vertices[b].position - mesh.vertices[a].position,
                                 mesh.vertices[c].position - mesh.vertices[a].position);
            if (Length(n) < 1e-14f || weld[a] == weld[b] || weld[b] == weld[c] ||
                weld[a] == weld[c]) {
                ++degenerate;
                continue;
            }
            for (const u32 v : {a, b, c}) {
                flipped += Dot(n, mesh.vertices[v].normal) < 0.0f ? 1 : 0;
            }
            ++edges[{weld[a], weld[b]}];
            ++edges[{weld[b], weld[c]}];
            ++edges[{weld[c], weld[a]}];
        }
        EXPECT_EQ(degenerate, 0u);
        EXPECT_EQ(flipped, 0u) << "vertex normals against their face";
        if (closed) {
            u32 bad = 0;
            for (const auto& [edge, count] : edges) {
                const auto twin = edges.find({edge.second, edge.first});
                bad += (count != 1 || twin == edges.end() || twin->second != 1) ? 1 : 0;
            }
            EXPECT_EQ(bad, 0u) << "open, non-manifold or inconsistently wound edges";
            EXPECT_GT(SignedVolume(mesh), 0.0) << "winding must face outward";
        }
        total += static_cast<u32>(mesh.indices.size() / 3);
    }
    EXPECT_LT(total, 40000u);
}

TEST(VrOverlayMeshes, LibraryPacksEveryMesh) {
    const MeshLibrary lib = BuildLibrary();
    u32 indices = 0;
    for (u32 m = 0; m < MeshCount; ++m) {
        const MeshRange& r = lib.ranges[m];
        EXPECT_EQ(r.first_index, indices);
        EXPECT_GT(r.index_count, 0u);
        indices += r.index_count;
        for (u32 i = 0; i < r.index_count; ++i) {
            ASSERT_LT(lib.indices[r.first_index + i], r.vertex_count);
        }
        EXPECT_LE(static_cast<size_t>(r.vertex_offset) + r.vertex_count, lib.vertices.size());
    }
    EXPECT_EQ(indices, lib.indices.size());
}

TEST(VrOverlayMeshes, VialDimensions) {
    float lo = 1.0f, hi = -1.0f, radius = 0.0f;
    for (const MeshId id : {MeshId::VialNeedle, MeshId::VialHub, MeshId::VialFerrule,
                            MeshId::VialGlass, MeshId::VialBlood, MeshId::VialStopper,
                            MeshId::VialBand}) {
        for (const Vertex& v : BuildMesh(id).vertices) {
            lo = std::min(lo, v.position.y);
            hi = std::max(hi, v.position.y);
            radius = std::max(radius, std::sqrt(v.position.x * v.position.x +
                                                v.position.z * v.position.z));
        }
    }
    EXPECT_NEAR(lo, -0.0640f, 5e-4f); // needle tip
    EXPECT_NEAR(hi, 0.0472f, 5e-4f);  // cap
    EXPECT_NEAR(radius, 0.0100f, 5e-4f);
}

Vec3 Ndc(const Mat4& p, Vec3 v) {
    const Vec4 c = p * Vec4{v.x, v.y, v.z, 1.0f};
    return {c.x / c.w, c.y / c.w, c.z / c.w};
}

TEST(VrOverlayMath, ReversedZProjection) {
    const float l = -54.0f * DegToRad, r = 40.0f * DegToRad, u = 44.0f * DegToRad,
                d = -55.0f * DegToRad;
    const Mat4 p = ProjectionReversedZ(l, r, u, d, 0.03f);
    EXPECT_NEAR(Ndc(p, {std::tan(l) * 2.0f, 0.0f, -2.0f}).x, -1.0f, 1e-5f);
    EXPECT_NEAR(Ndc(p, {std::tan(r) * 2.0f, 0.0f, -2.0f}).x, 1.0f, 1e-5f);
    EXPECT_NEAR(Ndc(p, {0.0f, std::tan(u) * 2.0f, -2.0f}).y, -1.0f, 1e-5f); // top = -1
    EXPECT_NEAR(Ndc(p, {0.0f, std::tan(d) * 2.0f, -2.0f}).y, 1.0f, 1e-5f);
    EXPECT_NEAR(Ndc(p, {0.0f, 0.0f, -0.03f}).z, 1.0f, 1e-6f);
    EXPECT_GT(Ndc(p, {0.0f, 0.0f, -1.0f}).z, Ndc(p, {0.0f, 0.0f, -1.001f}).z);
    EXPECT_LT(Ndc(p, {0.0f, 0.0f, -1000.0f}).z, 1e-4f);
    // A triangle wound CCW seen from the camera is front facing (Vulkan: a > 0 with
    // VK_FRONT_FACE_COUNTER_CLOCKWISE, a = -1/2 sum(x_i y_i+1 - x_i+1 y_i)).
    const Vec3 a = Ndc(p, {-0.1f, -0.1f, -1.0f}), b = Ndc(p, {0.1f, -0.1f, -1.0f}),
               c = Ndc(p, {0.0f, 0.1f, -1.0f});
    const float sum = a.x * b.y - b.x * a.y + b.x * c.y - c.x * b.y + c.x * a.y - a.x * c.y;
    EXPECT_GT(-0.5f * sum, 0.0f);
}

TEST(VrOverlayMath, InversesAndRotations) {
    const Pose pose{QuatFromAxisAngle({0.3f, 1.0f, -0.2f}, 1.1f), {0.2f, 1.5f, -0.4f}};
    const Mat4 m = Mat4::FromPose(pose);
    const Mat4 rigid = RigidInverse(m) * m;
    const Mat4 general = Inverse(m) * m;
    for (int k = 0; k < 16; ++k) {
        EXPECT_NEAR(rigid.m[k], k % 5 == 0 ? 1.0f : 0.0f, 1e-5f);
        EXPECT_NEAR(general.m[k], k % 5 == 0 ? 1.0f : 0.0f, 1e-5f);
    }
    EXPECT_NEAR(Length(Rotate(pose.orientation, {0.1f, -0.7f, 0.4f}) -
                       m.TransformVector({0.1f, -0.7f, 0.4f})),
                0.0f, 1e-5f);
    EXPECT_NEAR(HeadYaw(QuatFromAxisAngle({0.0f, 1.0f, 0.0f}, 0.7f)), 0.7f, 1e-5f);
}

TEST(VrOverlayScene, VialSitsInTheGrip) {
    const Mat4 grip = Mat4::FromPose({QuatFromAxisAngle({1.0f, 0.2f, 0.0f}, 0.6f),
                                      {-0.2f, 1.2f, -0.3f}});
    const Mat4 vial = VialWorldFromGrip(grip);
    // Ice-pick hold: cap out of the thumb side (grip -Z), needle out of the little finger side.
    EXPECT_NEAR(Length(vial.TransformPoint({0.0f, -0.064f, 0.0f}) -
                       grip.TransformPoint({0.0f, 0.0f, 0.056f})),
                0.0f, 1e-4f);
    EXPECT_NEAR(Length(vial.TransformPoint({0.0f, 0.0472f, 0.0f}) -
                       grip.TransformPoint({0.0f, 0.0f, -0.0552f})),
                0.0f, 1e-4f);
}

TEST(VrOverlayScene, LiquidPlaneFollowsGravity) {
    const Mat4 upright{};
    const Mat4 inverted = Mat4::RotationX(Pi);
    EXPECT_NEAR(VialLiquidPlane(upright, VialLevelFull), VialLevelFull, 1e-6f);
    // Upside down the same volume fills from the cap end.
    const float fill = VialLevelFull - VialLevelEmpty;
    EXPECT_NEAR(VialLiquidPlane(inverted, VialLevelFull), 0.0330f - fill, 1e-6f);
    EXPECT_NEAR(VialLiquidPlane(upright, VialLevelEmpty), VialLevelEmpty, 1e-6f);
}

// Runs the animator until it hides (or `limit` seconds); returns the time it took.
float RunUntilHidden(VialAnimator& anim, bool armed, float limit, VialVisual& last) {
    float t = 0.0f;
    VialVisual v;
    while (t < limit) {
        t += Dt;
        if (!anim.Update(Dt, armed, -1.0f, 0, true, v)) {
            return t;
        }
        last = v;
    }
    return t;
}

void Arm(VialAnimator& anim, VialVisual& v, int frames = 30) {
    for (int i = 0; i < frames; ++i) {
        anim.Update(Dt, true, -1.0f, i == 0 ? VialEventArmed : 0u, true, v);
    }
}

TEST(VrOverlayScene, VialAppearsAndXDisarms) {
    VialAnimator anim;
    VialVisual v;
    ASSERT_TRUE(anim.Update(Dt, true, 5.0f, VialEventArmed, true, v)); // visible at once
    EXPECT_GT(v.alpha, 0.0f);
    float peak = 0.0f;
    for (int i = 0; i < 20; ++i) {
        anim.Update(Dt, true, 5.0f, 0, true, v);
        peak = std::max(peak, v.scale);
    }
    EXPECT_FLOAT_EQ(v.alpha, 1.0f);
    EXPECT_FLOAT_EQ(v.scale, 1.0f);
    EXPECT_GT(peak, 1.01f); // a small pop
    // X again: gone within the leave time, and nothing is drawn afterwards (no layer).
    ASSERT_TRUE(anim.Update(Dt, false, -1.0f, VialEventDisarmed, true, v));
    EXPECT_LT(v.alpha, 1.0f);
    VialVisual last;
    EXPECT_LE(RunUntilHidden(anim, false, 1.0f, last), VialAnimator::LeaveSeconds + 2 * Dt);
    EXPECT_FALSE(anim.Update(Dt, false, -1.0f, 0, true, v));
}

// X held (or the Triangle fallback): the game shows its own vial, ours goes at once.
TEST(VrOverlayScene, HandedToGameHidesAtOnce) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    EXPECT_FALSE(anim.Update(Dt, false, -1.0f, VialEventHandedToGame, true, v));
}

TEST(VrOverlayScene, TimeoutWarningDims) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    float normal = 0.0f, warning = 0.0f;
    for (int i = 0; i < 90; ++i) {
        anim.Update(Dt, true, 3.0f, 0, true, v);
        normal = std::max(normal, v.glow);
    }
    for (int i = 0; i < 45; ++i) {
        anim.Update(Dt, true, 0.5f - i * Dt, 0, true, v);
        warning = std::max(warning, v.glow);
    }
    EXPECT_LT(warning, normal * 0.85f);
}

TEST(VrOverlayScene, InjectionHealedDrainsThenHides) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    ASSERT_TRUE(anim.Update(Dt, false, -1.0f, VialEventInjectSent, true, v));
    float min_level = 1.0f, plunge = 0.0f, t = Dt;
    bool visible = true;
    while (visible && t < 3.0f) {
        const u32 ev = (t > 0.08f && t < 0.08f + Dt) ? VialEventInjectHealed : 0u;
        visible = anim.Update(Dt, false, -1.0f, ev, true, v);
        if (visible) {
            min_level = std::min(min_level, v.level);
            plunge = std::max(plunge, v.plunge);
        }
        t += Dt;
    }
    EXPECT_NEAR(min_level, VialLevelEmpty, 1e-4f);
    EXPECT_NEAR(plunge, 0.012f, 1e-4f);
    EXPECT_FALSE(visible);
    EXPECT_GT(t, 0.55f);
    EXPECT_LT(t, 0.8f);
}

TEST(VrOverlayScene, InjectionDeniedRestoresAndShakes) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    anim.Update(Dt, false, -1.0f, VialEventInjectSent, true, v);
    for (int i = 0; i < 9; ++i) {
        anim.Update(Dt, false, -1.0f, 0, true, v);
    }
    EXPECT_LT(v.level, VialLevelFull - 0.005f);
    anim.Update(Dt, false, -1.0f, VialEventInjectDenied, true, v);
    float shake = 0.0f, level = 0.0f, t = Dt;
    bool visible = true;
    while (visible && t < 2.0f) {
        visible = anim.Update(Dt, false, -1.0f, 0, true, v);
        if (visible) {
            shake = std::max(shake, std::abs(v.shake.x));
            level = v.level;
        }
        t += Dt;
    }
    EXPECT_GT(shake, 0.001f);
    EXPECT_NEAR(level, VialLevelFull, 1e-4f);
    EXPECT_FALSE(visible);
    // Refused at once (busy): sent and denied in one frame still shows the shake.
    Arm(anim, v);
    EXPECT_TRUE(
        anim.Update(Dt, false, -1.0f, VialEventInjectSent | VialEventInjectDenied, true, v));
}

// Two vials in a row: X right after a heal pops the next vial in at once (the drain of the first
// is cut short), and its own stab plunges and drains it.
TEST(VrOverlayScene, RearmDuringInjectionShowsTheNextVial) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    anim.Update(Dt, false, -1.0f, VialEventInjectSent, true, v);
    anim.Update(Dt, false, -1.0f, VialEventInjectHealed, true, v);
    for (int i = 0; i < 5; ++i) {
        anim.Update(Dt, false, -1.0f, 0, true, v);
    }
    EXPECT_LT(v.level, VialLevelFull - 0.005f); // draining
    // Re-armed 0.1 s into the first injection: a full vial appears within the appear time.
    ASSERT_TRUE(anim.Update(Dt, true, -1.0f, VialEventArmed, true, v));
    for (int i = 0; i < 15; ++i) {
        anim.Update(Dt, true, -1.0f, 0, true, v);
    }
    EXPECT_FLOAT_EQ(v.alpha, 1.0f);
    EXPECT_NEAR(v.level, VialLevelFull, 1e-6f);
    EXPECT_NEAR(v.plunge, 0.0f, 1e-6f);
    // The second stab: plunge and drain again.
    anim.Update(Dt, false, -1.0f, VialEventInjectSent, true, v);
    float plunge = 0.0f;
    for (int i = 0; i < 12; ++i) {
        anim.Update(Dt, false, -1.0f, i == 3 ? VialEventInjectHealed : 0u, true, v);
        plunge = std::max(plunge, v.plunge);
    }
    EXPECT_NEAR(plunge, 0.012f, 1e-4f);
    EXPECT_LT(v.level, VialLevelFull - 0.01f);
    // Re-armed during a refusal's shake: the next vial too.
    VialAnimator denied;
    Arm(denied, v);
    denied.Update(Dt, false, -1.0f, VialEventInjectSent | VialEventInjectDenied, true, v);
    ASSERT_TRUE(denied.Update(Dt, true, -1.0f, VialEventArmed, true, v));
    for (int i = 0; i < 15; ++i) {
        denied.Update(Dt, true, -1.0f, 0, true, v);
    }
    EXPECT_FLOAT_EQ(v.alpha, 1.0f);
    EXPECT_NEAR(v.shake.x, 0.0f, 1e-6f);
}

// A stab that lands while the last vial is still leaving starts its own injection.
TEST(VrOverlayScene, InjectionDuringLeaveIsShown) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    anim.Update(Dt, false, -1.0f, VialEventDisarmed, true, v); // leaving
    ASSERT_TRUE(anim.Update(Dt, false, -1.0f, VialEventInjectSent, true, v));
    float plunge = 0.0f;
    for (int i = 0; i < 6; ++i) {
        anim.Update(Dt, false, -1.0f, 0, true, v);
        plunge = std::max(plunge, v.plunge);
    }
    EXPECT_NEAR(plunge, 0.012f, 1e-4f);
    EXPECT_FLOAT_EQ(v.alpha, 1.0f);
}

// The overlay follows the DIBR warp's near-field compression (vr_stereo_warp_math.h): exact
// beyond the knee, the same numbers as the warp nearer, never nearer than the clamp, off when the
// settings turn it off.
TEST(VrOverlayStereo, NearDisparityScaleMatchesTheWarp) {
    EXPECT_FLOAT_EQ(NearDisparityScale(0.8f, 0.6f, 0.3f), 1.0f);
    EXPECT_FLOAT_EQ(NearDisparityScale(0.6f, 0.6f, 0.3f), 1.0f);
    // knee 1/0.6, floor 1/0.3: D' = knee + (floor - knee) tanh((D - knee) / (floor - knee)).
    const auto expected = [](float z) {
        const double knee = 1.0 / 0.6, floor = 1.0 / 0.3, r = floor - knee, d = 1.0 / z;
        return static_cast<float>((knee + r * std::tanh((d - knee) / r)) * z);
    };
    for (const float z : {0.5f, 0.4f, 0.35f, 0.3f, 0.2f, 0.1f}) {
        EXPECT_NEAR(NearDisparityScale(z, 0.6f, 0.3f), expected(z), 1e-5f) << z;
        EXPECT_LT(NearDisparityScale(z, 0.6f, 0.3f), 1.0f) << z;
        // Never shown nearer than the clamp: z / scale >= 0.3.
        EXPECT_GE(z / NearDisparityScale(z, 0.6f, 0.3f), 0.3f - 1e-4f) << z;
    }
    EXPECT_NEAR(NearDisparityScale(0.4f, 0.6f, 0.3f), 0.975f, 0.002f);
    EXPECT_FLOAT_EQ(NearDisparityScale(0.3f, 0.0f, 0.3f), 1.0f);  // knee off
    EXPECT_FLOAT_EQ(NearDisparityScale(0.3f, 0.6f, 0.0f), 1.0f);  // clamp off
    EXPECT_FLOAT_EQ(NearDisparityScale(0.3f, 0.3f, 0.4f), 1.0f);  // clamp beyond the knee
    EXPECT_FLOAT_EQ(NearDisparityScale(-0.2f, 0.6f, 0.3f), 1.0f); // behind the eyes
}

TEST(VrOverlayScene, ResetAndTrackingLossFade) {
    VialAnimator anim;
    VialVisual v;
    Arm(anim, v);
    ASSERT_TRUE(anim.Update(Dt, false, -1.0f, 0, true, v)); // armed lost without an event
    EXPECT_LT(v.alpha, 1.0f);
    anim.Reset();
    Arm(anim, v);
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(anim.Update(Dt, true, -1.0f, 0, false, v));
    }
    EXPECT_FLOAT_EQ(v.alpha, 1.0f); // brief tracking loss: kept
    bool visible = true;
    for (int i = 0; i < 30; ++i) {
        visible = anim.Update(Dt, true, -1.0f, 0, false, v);
    }
    EXPECT_FALSE(visible);
}

TEST(VrOverlayScene, DrawListOrder) {
    std::vector<DrawItem> items;
    AppendGhostHand(items, Hand::Left, Mat4{}, {});
    VialVisual fading;
    fading.alpha = 0.5f;
    AppendVial(items, Mat4{}, fading);
    AppendVialHoldout(items, Hand::Left, Mat4{});
    AppendController(items, Hand::Right, Mat4{}, {});
    AppendLaser(items, Mat4{}, {});
    AppendAxes(items, Mat4{});
    SortDrawItems(items);
    for (size_t i = 1; i < items.size(); ++i) {
        EXPECT_LE(static_cast<u32>(items[i - 1].pass), static_cast<u32>(items[i].pass));
    }
    EXPECT_EQ(items.front().pass, Pass::Holdout);
    u32 depth_only = 0, trans = 0;
    for (const DrawItem& item : items) {
        depth_only += item.pass == Pass::DepthOnly ? 1 : 0;
        trans += item.pass == Pass::TransEqual ? 1 : 0;
    }
    EXPECT_EQ(depth_only, trans);
    EXPECT_GT(trans, 0u);
    const DrawConstants c = MakeDrawConstants(items.back(), 1);
    EXPECT_FLOAT_EQ(c.extra.x, 1.0f);
    EXPECT_FLOAT_EQ(c.extra.y, static_cast<float>(static_cast<u32>(items.back().shade)));
}

// weapon_wall's sparks: the laser's beam from the spark's head (its narrow, bright end) back to
// its tail, additive; a dead or still one draws nothing.
TEST(VrOverlayScene, SparkIsAStreakFromItsHeadBack) {
    std::vector<DrawItem> items;
    const Vec3 head{0.1f, 1.2f, -0.5f};
    const Vec3 tail{0.13f, 1.18f, -0.49f};
    AppendSpark(items, head, tail, 0.0007f, {1.0f, 0.8f, 0.4f}, 0.7f);
    ASSERT_EQ(items.size(), 1u);
    const DrawItem& d = items[0];
    EXPECT_EQ(d.mesh, MeshId::LaserBeam);
    EXPECT_EQ(d.pass, Pass::Additive);
    EXPECT_EQ(d.shade, Shade::Beam);
    EXPECT_FLOAT_EQ(d.material.fade, 0.7f);
    const auto same = [](Vec3 a, Vec3 b) { return Length(a - b) < 1e-5f; };
    EXPECT_TRUE(same(d.model.TransformPoint({0.0f, 0.0f, 0.0f}), head));
    EXPECT_TRUE(same(d.model.TransformPoint({0.0f, 0.0f, -1.0f}), tail));
    EXPECT_NEAR(Length(d.model.TransformVector({1.0f, 0.0f, 0.0f})), 0.0007f, 1e-7f);
    EXPECT_NEAR(Length(d.model.TransformVector({0.0f, 1.0f, 0.0f})), 0.0007f, 1e-7f);
    AppendSpark(items, head, head, 0.0007f, {1.0f, 1.0f, 1.0f}, 1.0f); // not moving
    AppendSpark(items, head, tail, 0.0007f, {1.0f, 1.0f, 1.0f}, 0.0f); // dead
    AppendSpark(items, head, {0.1f, 1.5f, -0.5f}, 0.0007f, {1.0f, 1.0f, 1.0f}, 0.5f); // straight up
    ASSERT_EQ(items.size(), 2u);
    EXPECT_TRUE(same(items[1].model.TransformPoint({0.0f, 0.0f, -1.0f}), {0.1f, 1.5f, -0.5f}));
}

TEST(VrOverlayScene, HiddenThingsDrawNothing) {
    std::vector<DrawItem> items;
    VialVisual gone;
    gone.alpha = 0.0f;
    AppendVial(items, Mat4{}, gone);
    ControllerVisual controller;
    controller.alpha = 0.0f;
    AppendController(items, Hand::Left, Mat4{}, controller);
    AppendGhostHand(items, Hand::Right, Mat4{}, {0.0f, 0.0f, 0.0f});
    LaserVisual laser;
    laser.alpha = 0.0f;
    AppendLaser(items, Mat4{}, laser);
    VignetteVisual vignette;
    vignette.alpha = 0.0f;
    AppendVignette(items, Pose{}, vignette);
    EXPECT_TRUE(items.empty()); // an empty list means no overlay layer at all
}

TEST(VrOverlayScene, HealVignetteIsAHeadLockedRingAtTheEdges) {
    // The ring's profile: clear within VignetteInnerDeg of the view axis, full from
    // VignetteFullDeg, never decreasing outward; every vertex at the angle its radius says.
    const MeshData mesh = BuildMesh(MeshId::Vignette);
    float last_angle = 0.0f, last_profile = -1.0f;
    for (const Vertex& v : mesh.vertices) {
        const float angle = std::atan(std::hypot(v.position.x, v.position.y)) / DegToRad;
        EXPECT_GE(angle, VignetteInnerDeg - 0.01f);
        EXPECT_LE(angle, VignetteOuterDeg + 0.01f);
        if (angle > last_angle + 0.5f) {
            EXPECT_GE(v.aux.y, last_profile);
            last_angle = angle;
            last_profile = v.aux.y;
        }
        if (angle <= VignetteInnerDeg + 0.01f) {
            EXPECT_NEAR(v.aux.y, 0.0f, 1e-6f);
        }
        if (angle >= VignetteFullDeg - 0.01f) {
            EXPECT_NEAR(v.aux.y, 1.0f, 1e-6f);
        }
        // aux.x: the angle itself (degrees / 90), for the shader's own profiles.
        EXPECT_NEAR(v.aux.x * 90.0f, angle, 0.01f);
    }
    // Placed around the head: a vertex at 40 degrees to the right of a head looking along -Z
    // (yawed 90 degrees left: along -X) lands 40 degrees off that view axis, 2 m deep.
    std::vector<DrawItem> items;
    Pose head;
    head.orientation = QuatFromAxisAngle({0.0f, 1.0f, 0.0f}, 90.0f * DegToRad);
    head.position = {1.0f, 1.6f, -2.0f};
    AppendVignette(items, head, {.color = {0.1f, 0.9f, 0.2f}, .alpha = 0.3f, .distance_m = 2.0f});
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].pass, Pass::Screen);
    EXPECT_EQ(items[0].shade, Shade::Vignette);
    EXPECT_NEAR(items[0].material.base.w, 0.3f, 1e-6f);
    const float r = std::tan(40.0f * DegToRad);
    const Vec3 p = items[0].model.TransformPoint({r, 0.0f, 0.0f}) - head.position;
    const Vec3 forward{-1.0f, 0.0f, 0.0f};
    EXPECT_NEAR(Dot(p, forward), 2.0f, 1e-4f);
    EXPECT_NEAR(std::acos(Dot(Normalize(p), forward)) / DegToRad, 40.0f, 0.01f);
    // The low-HP dark (vitals_edge): its own ramp by the angle; the rally's rim: a band.
    AppendVignette(items, head,
                   {.color = {0.02f, 0.0f, 0.0f}, .alpha = 0.18f, .inner_deg = 30.0f,
                    .full_deg = 58.0f});
    AppendVignette(items, head,
                   {.color = {0.6f, 0.23f, 0.03f}, .alpha = 0.1f, .inner_deg = 54.0f,
                    .full_deg = 62.0f, .band = true});
    ASSERT_EQ(items.size(), 3u);
    EXPECT_NEAR(items[1].material.params.x, 30.0f, 1e-6f);
    EXPECT_NEAR(items[1].material.params.y, 58.0f, 1e-6f);
    EXPECT_NEAR(items[1].material.params.w, 1.0f, 1e-6f);
    EXPECT_NEAR(items[2].material.params.w, 2.0f, 1e-6f);
    EXPECT_NEAR(items[0].material.params.w, 0.0f, 1e-6f); // the heal vignette: the mesh's own
    items.resize(1);
    // The last pass, after every other item.
    AppendAxes(items, Mat4{});
    SortDrawItems(items);
    EXPECT_EQ(items.back().mesh, MeshId::Vignette);
}

TEST(VrOverlayScene, HitVignetteSharpensTheProfile) {
    // The heal vignette keeps the ring's profile (power 1); the hit vignette cubes it (only the
    // very edges): params.z carries the power to the shader, clamped to a sane range.
    std::vector<DrawItem> items;
    AppendVignette(items, Pose{}, {.alpha = 0.35f});
    AppendVignette(items, Pose{},
                   {.color = {0.6f, 0.0f, 0.0f}, .alpha = 0.4f, .profile_power = 3.0f});
    AppendVignette(items, Pose{}, {.alpha = 0.4f, .profile_power = 100.0f});
    ASSERT_EQ(items.size(), 3u);
    EXPECT_FLOAT_EQ(items[0].material.params.z, 1.0f);
    EXPECT_FLOAT_EQ(items[1].material.params.z, 3.0f);
    EXPECT_FLOAT_EQ(items[2].material.params.z, 8.0f);
    EXPECT_NEAR(items[1].material.base.w, 0.4f, 1e-6f);
    // Half way up the ramp (41 degrees) the ring's profile is a quarter: the heal vignette shows
    // 25% of its strength there, the hit vignette (cubed in the shader) under 2%.
    const MeshData mesh = BuildMesh(MeshId::Vignette);
    u32 found = 0;
    for (const Vertex& v : mesh.vertices) {
        const float angle = std::atan(std::hypot(v.position.x, v.position.y)) / DegToRad;
        if (std::abs(angle - 41.0f) < 0.01f) {
            ++found;
            EXPECT_NEAR(v.aux.y, 0.25f, 1e-4f);
            EXPECT_NEAR(v.aux.y * v.aux.y * v.aux.y, 0.015625f, 1e-4f);
        }
    }
    EXPECT_GT(found, 0u);
}

TEST(VrOverlayScene, DirectionalHitVignetteCarriesItsWedge) {
    // hit_vignette_directional: the wedge's centre, power and focus go to the shader in emissive
    // (x, y, z, w); without a focus emissive stays 0 (the plain ring), as for the heal vignette.
    std::vector<DrawItem> items;
    AppendVignette(items, Pose{}, {.alpha = 0.3f, .profile_power = 3.0f});
    AppendVignette(items, Pose{},
                   {.alpha = 0.3f,
                    .profile_power = 2.0f,
                    .wedge_focus = 1.0f,
                    .wedge_x = 1.0f,
                    .wedge_y = 0.0f,
                    .wedge_power = 4.0f});
    AppendVignette(items, Pose{},
                   {.alpha = 0.3f, .wedge_focus = 3.0f, .wedge_y = -1.0f, .wedge_power = 99.0f});
    ASSERT_EQ(items.size(), 3u);
    EXPECT_FLOAT_EQ(items[0].material.emissive.w, 0.0f);
    EXPECT_FLOAT_EQ(items[1].material.emissive.x, 1.0f);
    EXPECT_FLOAT_EQ(items[1].material.emissive.y, 0.0f);
    EXPECT_FLOAT_EQ(items[1].material.emissive.z, 4.0f);
    EXPECT_FLOAT_EQ(items[1].material.emissive.w, 1.0f);
    EXPECT_FLOAT_EQ(items[1].material.params.z, 2.0f);
    EXPECT_FLOAT_EQ(items[2].material.emissive.y, -1.0f);
    EXPECT_FLOAT_EQ(items[2].material.emissive.z, 16.0f); // clamped
    EXPECT_FLOAT_EQ(items[2].material.emissive.w, 1.0f);  // clamped
}

} // namespace

TEST(VrGripGuide, FaintHandleAlignedHaloAndDisabledIsEmpty) {
    namespace O = Vulkan::VrOverlay;
    std::vector<O::DrawItem> items;
    const O::Vec3 p{0.2f, 1.1f, -0.4f};
    O::AppendGripGuide(items, p, {0, 1, 0});
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].pass, O::Pass::Additive);
    EXPECT_EQ(items[0].shade, O::Shade::Glow);
    EXPECT_NEAR(items[0].material.fade, 0.18f, 1e-6f);
    EXPECT_LT(O::Length(items[0].model.Column(3) - p), 1e-6f);
    EXPECT_LT(O::Length(items[0].model.Column(2) - O::Vec3{0, 0.055f, 0}), 1e-6f);
    O::AppendGripGuide(items, p, {0, 1, 0}, 0.0f);
    O::AppendGripGuide(items, p, {});
    EXPECT_EQ(items.size(), 1u);
}


TEST(VrGripGuide, RailLengthMatchesTheContinuousGrabRegion) {
    namespace O = Vulkan::VrOverlay;
    std::vector<O::DrawItem> items;
    O::AppendGripGuide(items, {}, {0, 0, 1}, 0.12f, 0.4f);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_NEAR(O::Length(items[0].model.Column(2)), 0.4f, 1e-6f);
    EXPECT_NEAR(items[0].material.fade, 0.12f, 1e-6f);
    O::AppendGripGuide(items, {}, {0, 0, 1}, 0.12f, -1.0f);
    EXPECT_EQ(items.size(), 1u);
}
