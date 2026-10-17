// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "common/types.h"

// Procedural meshes and draw lists for the VR overlay renderer (vr_overlay.h): the Blood Vial
// held in the left hand while an injection is armed, stylized (ringless) Quest 3 Touch Plus
// controllers, translucent ghost hands, the gun laser and the heal vignette. Everything is
// generated in code from lathes, lofts, rounded boxes and capsules: no model files and no
// third-party assets.
//
// Pure C++ (no Vulkan, no OpenXR) so it can be unit-tested and rendered offline; the overlay
// renderer uploads BuildLibrary() once and draws DrawItems with the shaders in
// host_shaders/vr_overlay.vert/.frag, whose push-constant and uniform layouts are mirrored by
// DrawConstants and FrameConstants below.
//
// Conventions: meters, right-handed, world = OpenXR LOCAL space (+Y up, -Z forward). Matrices
// are column-major (m[col * 4 + row]) as GLSL expects. Triangles are counter-clockwise seen from
// outside; the projection flips Y (Vulkan), so outside faces stay front faces with
// VK_FRONT_FACE_COUNTER_CLOCKWISE. Grip frame (OpenXR grip pose): -Z runs along the handle from
// the little finger to the thumb, +X points right on both hands in a thumbs-up fist (into the
// right palm, out of the left palm), +Y points back toward the wrist.
namespace Vulkan::VrOverlay {

// ---- math ---------------------------------------------------------------------------------

struct Vec2 {
    float x{}, y{};
};
struct Vec3 {
    float x{}, y{}, z{};
};
struct Vec4 {
    float x{}, y{}, z{}, w{};
};
// Same memory layout as XrQuaternionf / VR::Quat and XrPosef / VR::Pose.
struct Quat {
    float x{0.0f}, y{0.0f}, z{0.0f}, w{1.0f};
};
struct Pose {
    Quat orientation{};
    Vec3 position{};
};

constexpr float Pi = 3.14159265358979f;
constexpr float DegToRad = Pi / 180.0f;

constexpr Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 operator-(Vec3 a) {
    return {-a.x, -a.y, -a.z};
}
constexpr Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
constexpr Vec3 operator*(float s, Vec3 a) {
    return {a.x * s, a.y * s, a.z * s};
}
constexpr Vec3 Mul(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
constexpr float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
constexpr Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) {
    return std::sqrt(Dot(a, a));
}
inline Vec3 Normalize(Vec3 a) {
    const float l = Length(a);
    return l > 1e-20f ? a * (1.0f / l) : Vec3{0.0f, 0.0f, 0.0f};
}
constexpr Vec3 Lerp(Vec3 a, Vec3 b, float t) {
    return a + (b - a) * t;
}
constexpr Vec4 ToVec4(Vec3 v, float w) {
    return {v.x, v.y, v.z, w};
}

Quat QuatMul(const Quat& a, const Quat& b);
Quat QuatFromAxisAngle(Vec3 axis, float radians);
Vec3 Rotate(const Quat& q, Vec3 v);
// Yaw (radians, about +Y) of the forward (-Z) axis of `q`; 0 when looking down -Z.
float HeadYaw(const Quat& q);

struct Mat4 {
    std::array<float, 16> m{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    float& At(u32 row, u32 col) {
        return m[col * 4 + row];
    }
    float At(u32 row, u32 col) const {
        return m[col * 4 + row];
    }
    Vec3 Column(u32 col) const {
        return {m[col * 4 + 0], m[col * 4 + 1], m[col * 4 + 2]};
    }

    static Mat4 Identity() {
        return {};
    }
    static Mat4 Translation(Vec3 t);
    static Mat4 Scale(Vec3 s);
    static Mat4 Scale(float s) {
        return Scale(Vec3{s, s, s});
    }
    static Mat4 Rotation(const Quat& q);
    static Mat4 RotationAxis(Vec3 axis, float radians);
    static Mat4 RotationX(float radians);
    static Mat4 RotationY(float radians);
    static Mat4 RotationZ(float radians);
    // Columns x, y, z (should be orthonormal and right-handed) and the origin.
    static Mat4 FromBasis(Vec3 x, Vec3 y, Vec3 z, Vec3 origin);
    static Mat4 FromPose(const Pose& pose);

    Vec3 TransformPoint(Vec3 p) const;
    Vec3 TransformVector(Vec3 v) const;
    Vec4 operator*(const Vec4& v) const;
};

Mat4 operator*(const Mat4& a, const Mat4& b);
// Inverse of a rotation + translation (no scale).
Mat4 RigidInverse(const Mat4& a);
// General inverse (returns identity if singular).
Mat4 Inverse(const Mat4& a);

// Reversed-Z perspective with an infinite far plane for Vulkan (NDC y down, depth 1 at
// `near_m`, falling to 0 at infinity), from OpenXR FOV angles (radians; left and down are
// negative). Depth test GREATER_OR_EQUAL, depth clear 0.
Mat4 ProjectionReversedZ(float angle_left, float angle_right, float angle_up, float angle_down,
                         float near_m);

// ---- meshes -------------------------------------------------------------------------------

struct Vertex {
    Vec3 position;
    Vec3 normal;
    // x: angle fraction around the lathe axis (0 for non-lathe meshes); y: normalized length
    // coordinate (lathe profile arc length, 0 at the first profile point).
    Vec2 aux;
};
static_assert(sizeof(Vertex) == 32);

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    bool closed{true}; // watertight (every edge shared by exactly two triangles)
};

// A lathe profile point (r = radius, y = height). A crease splits the normal there; an explicit
// normal (nr, ny not both 0) overrides the one derived from the profile. The profile runs from
// the bottom to the top with the solid on the left of the direction of travel (outward normal
// = (dy, -dr)); r = 0 at an end closes it with a pole.
struct ProfilePoint {
    float r{}, y{};
    bool crease{false};
    float nr{0.0f}, ny{0.0f};
};

// Revolves `profile` around +Y. closed_loop joins the last point back to the first (a ring
// with a closed cross-section, traversed counter-clockwise in the (r, y) plane).
MeshData Lathe(std::span<const ProfilePoint> profile, u32 segments, bool closed_loop = false);

// A superellipse ring for Loft: center + axis_u * a * c(t) + axis_v * b * s(t), where
// c, s = signed |cos|^(2/e), |sin|^(2/e).
struct LoftRing {
    Vec3 center;
    Vec3 axis_u, axis_v;
    float a{}, b{};
    float exponent{2.0f};
};
// Sweeps rings in order and closes both ends with poles (start_pole / end_pole). Normals are
// area weighted; the winding is made outward automatically.
MeshData Loft(std::span<const LoftRing> rings, u32 segments, Vec3 start_pole, Vec3 end_pole);

// Capsule from the origin along -Z: a sphere of radius r0 at the origin, one of radius r1 at
// z = -length, joined by their tangent cone. aux.y runs 0 -> 1 from the r0 end.
MeshData TaperedCapsule(float length, float r0, float r1, u32 segments = 16, u32 arc_steps = 5);
MeshData Sphere(float radius, u32 segments = 24, u32 rings = 12);
// Box with rounded edges and corners (radius <= every half extent).
MeshData RoundedBox(Vec3 half_extents, float radius, u32 steps = 4);
// Flat-topped cylinder along +Y from y = 0 to y = height with a rounded top edge.
MeshData Puck(float radius, float height, float bevel, u32 segments = 24);

// Positions by `transform`, normals by its inverse transpose (winding is kept, so the
// transform must not mirror).
void TransformMesh(MeshData& mesh, const Mat4& transform);
void AppendMesh(MeshData& dst, const MeshData& src);
// Signed volume (> 0 for outward winding of a closed mesh).
double SignedVolume(const MeshData& mesh);

enum class MeshId : u32 {
    // Blood Vial (vial space: +Y from the needle tip, y = -0.064, to the cap, y = +0.0472).
    VialNeedle,
    VialHub,
    VialFerrule,
    VialGlass,
    VialBlood,
    VialStopper,
    VialBand,
    VialGlow,
    // Touch Plus controller parts (right-hand grip frame; left hands place them mirrored).
    ControllerHandle,
    ControllerHead,
    ControllerStickWell,
    ControllerStickShaft,
    ControllerStickCap,
    ControllerButton,
    ControllerSystemButton,
    ControllerTrigger,
    ControllerGripButton,
    // Ghost hand parts (every part is symmetric; handedness lives in the placement).
    HandPalm,
    HandForearm,
    HandThumb0,
    HandThumb1,
    HandThumb2,
    HandIndex0,
    HandIndex1,
    HandIndex2,
    HandMiddle0,
    HandMiddle1,
    HandMiddle2,
    HandRing0,
    HandRing1,
    HandRing2,
    HandPinky0,
    HandPinky1,
    HandPinky2,
    // Laser (unit frustum along -Z, radius 1 -> 2, open) and its hit dot (unit sphere).
    LaserBeam,
    LaserDot,
    // Debug axis arrow along +Y, length 1.
    AxisArrow,
    // The bow's arrow (core/vr/bow.h): its shaft and head along -Z from the nock at the origin,
    // length 1.
    Arrow,
    // Heal vignette: a flat ring in the z = 0 plane facing +Z, one unit in front of a viewer at
    // z = 1 (x, y = tan of the angle from the view axis, VignetteInnerDeg .. VignetteOuterDeg);
    // aux.x = that angle / 90 degrees, aux.y = the glow's opacity profile (0 inside, 1 toward
    // the edges of the view). Open.
    Vignette,
    Count,
};
constexpr u32 MeshCount = static_cast<u32>(MeshId::Count);

const char* MeshName(MeshId id);
MeshData BuildMesh(MeshId id);

struct MeshRange {
    u32 first_index{};
    u32 index_count{};
    s32 vertex_offset{};
    u32 vertex_count{};
};

// Every mesh packed into one vertex and one (32-bit, per-mesh 0-based) index buffer.
struct MeshLibrary {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::array<MeshRange, MeshCount> ranges{};
    std::array<bool, MeshCount> closed{};
};
MeshLibrary BuildLibrary();

// ---- materials and draw items -------------------------------------------------------------

// Pipeline variants, recorded in this order (each eye).
enum class Pass : u32 {
    Holdout,    // cull back, depth GE + write, no color: hides what is behind (DepthOnly pipeline)
    Opaque,     // cull back, depth GE + write, no blending
    Liquid,     // cull none, depth GE + write, premultiplied
    DepthOnly,  // cull back, depth GE + write, no color (prepass of TransEqual)
    TransEqual, // cull back, depth EQUAL, premultiplied (only the front-most surface)
    GlassBack,  // cull front, depth GE, premultiplied
    GlassFront, // cull back, depth GE, premultiplied
    Additive,   // cull front (back faces: halos), depth GE, ONE/ONE color, alpha kept
    Screen,     // cull none, no depth test, premultiplied, last: head-locked screen effects
    Count,
};
constexpr u32 PassCount = static_cast<u32>(Pass::Count);

// Fragment shading modes (vr_overlay.frag).
enum class Shade : u32 {
    Lit = 0,
    Glass = 1,
    Liquid = 2,
    Ghost = 3,
    Glow = 4,
    Beam = 5,
    Unlit = 6,
    // base.rgb at base.a * profile ^ params.z, premultiplied, no lighting or exposure; params.w
    // 0: the mesh's profile (aux.y), 1: a ramp from params.x to params.y degrees off the view
    // axis (squared smoothstep), 2: a band between them
    Vignette = 7,
};

struct Material {
    Vec4 base{1.0f, 1.0f, 1.0f, 1.0f}; // linear rgb, a = opacity
    Vec4 emissive{};                    // rgb, w = rim strength
    // x roughness, y metallic, z rim / falloff power, w liquid level (mesh-space y; glow:
    // fades above it, -1000 = off)
    Vec4 params{0.5f, 0.0f, 3.0f, 0.0f};
    float fade{1.0f};     // multiplies the premultiplied output (fades a whole item)
    float aux_fade{0.0f}; // ghost: opacity *= 1 - aux.y * aux_fade
};

struct DrawItem {
    MeshId mesh{};
    Pass pass{};
    Shade shade{};
    Mat4 model{}; // world <- mesh
    Material material{};
};

// Push constants (vr_overlay.vert/.frag DrawData, std430): exactly 128 bytes.
struct DrawConstants {
    std::array<float, 16> model;
    Vec4 base;
    Vec4 emissive;
    Vec4 params;
    Vec4 extra; // x eye, y shade mode, z fade, w aux fade
};
static_assert(sizeof(DrawConstants) == 128);
DrawConstants MakeDrawConstants(const DrawItem& item, u32 eye);

struct Lighting {
    Vec3 key_dir{0.0f, 1.0f, 0.0f}; // toward the light (world)
    Vec3 key_color{};
    Vec3 fill_dir{0.0f, -1.0f, 0.0f};
    Vec3 fill_color{};
    Vec3 sky{};
    Vec3 ground{};
    Vec3 rim{};
    float exposure{1.0f};
};
// Moonlight key from above / behind the viewer and a warm low fill from the front, both turned
// with the head yaw so held objects always read.
Lighting DefaultLighting(float head_yaw_radians, float exposure = 1.0f);

// Uniform buffer (FrameData, std140): 288 bytes.
struct FrameConstants {
    std::array<std::array<float, 16>, 2> view_proj;
    std::array<Vec4, 2> eye_pos;
    Vec4 key_dir;
    Vec4 key_color;
    Vec4 fill_dir;
    Vec4 fill_color;
    Vec4 sky; // w = exposure
    Vec4 ground;
    Vec4 rim;
    // What the headset shows (the union of both eyes' located FOV, degrees off the view axis):
    // x left, y right, z up, w down; 0 = unknown (the shader assumes a Quest 3's). The fitted
    // vignettes (VignetteVisual::fit_inner) are drawn against it.
    Vec4 field;
};
static_assert(sizeof(FrameConstants) == 288);
FrameConstants MakeFrameConstants(const std::array<Mat4, 2>& view_proj,
                                  const std::array<Vec3, 2>& eye_pos, const Lighting& lighting);

// Stable sort by pass (the record order).
void SortDrawItems(std::vector<DrawItem>& items);

// ---- scene assembly -----------------------------------------------------------------------

enum class Hand : u32 { Left = 0, Right = 1 };

constexpr float VialLevelFull = 0.0180f;   // blood level (vial-space y), about 73 % full
constexpr float VialLevelEmpty = -0.0228f; // the bottom of the blood volume

struct VialVisual {
    float scale{1.0f};
    float alpha{1.0f};
    float level{VialLevelFull};
    float glow{0.12f};   // emissive pulse level
    float plunge{0.0f};  // meters the vial moves along its needle (injection)
    Vec3 shake{};        // meters (denied injection)
};

// Where the vial sits in the grip: world_from_vial = world_from_grip * T(offset) * R(rotation)
// * Rx(-90 deg), so the cap points out of the thumb side of the fist and the needle out of the
// little-finger side (ice-pick hold, the stabbing gesture).
struct VialAttach {
    Vec3 offset_m{0.0f, 0.0f, -0.008f};
    Vec3 rotation_deg{0.0f, 0.0f, 0.0f}; // applied X, then Y, then Z (grip axes)
};
Mat4 VialWorldFromGrip(const Mat4& world_from_grip, const VialAttach& attach = {});
void AppendVial(std::vector<DrawItem>& out, const Mat4& world_from_vial, const VialVisual& vial);
// The vial-space y of the liquid plane for `level` (the upright fill height): a plane through
// the axis at the same fraction from the lower end keeps the volume of a cylinder, so an
// inverted vial fills from the cap end.
float VialLiquidPlane(const Mat4& world_from_vial, float level);
// Optional: closed-fist finger capsules drawn depth-only before the vial, so the overlay leaves
// holes where the game's fingers wrap the vial (the overlay is otherwise always on top).
void AppendVialHoldout(std::vector<DrawItem>& out, Hand hand, const Mat4& world_from_grip,
                       float scale = 1.0f);

struct ControllerVisual {
    float trigger{0.0f};
    float squeeze{0.0f};
    Vec2 stick{};              // -1..1 (x right, y up)
    bool button_lower{false};  // A / X
    bool button_upper{false};  // B / Y
    bool system{false};        // Meta / menu
    bool stick_click{false};
    float alpha{1.0f};
};
// Touch Plus layout: face buttons outboard, thumbstick inboard (flip with buttons_inboard).
void AppendController(std::vector<DrawItem>& out, Hand hand, const Mat4& world_from_grip,
                      const ControllerVisual& controller, bool buttons_inboard = false);

struct GhostHandVisual {
    float trigger{0.0f}; // index curl
    float squeeze{0.0f}; // middle, ring and little finger curl
    float alpha{0.35f};
};
void AppendGhostHand(std::vector<DrawItem>& out, Hand hand, const Mat4& world_from_grip,
                     const GhostHandVisual& ghost);

struct LaserVisual {
    float length_m{3.0f};
    float radius_m{0.0010f}; // at the hand; twice that at the far end
    float alpha{1.0f};
    bool hit_dot{false};
    float dot_radius_m{0.006f};
};
void AppendLaser(std::vector<DrawItem>& out, const Mat4& world_from_aim, const LaserVisual& laser);

// The bow's arrow, `length_m` long along -Z of `world_from_nock` (the nock at its origin).
void AppendArrow(std::vector<DrawItem>& out, const Mat4& world_from_nock, float length_m,
                 float alpha = 1.0f);

// A spark (core/vr/sparks.h): a streak glowing `color` (linear rgb) at `alpha`, brightest and
// `radius_m` thick at its `head`, fading out back to its `tail` (the laser's beam, additive).
// A soft, palm-sized halo aligned with the handle at the off-hand contact.
void AppendGripGuide(std::vector<DrawItem>& out, Vec3 centre, Vec3 axis, float alpha = 0.18f,
                     float half_length_m = 0.055f);

void AppendSpark(std::vector<DrawItem>& out, Vec3 head, Vec3 tail, float radius_m, Vec3 color,
                 float alpha);

// Debug: X red, Y green, Z blue arrows of `length_m`.
void AppendAxes(std::vector<DrawItem>& out, const Mat4& world_from_frame, float length_m = 0.05f);

// Heal and hit vignettes: a soft glow at the edges of the view, head-locked at `distance_m` (its
// stereo depth) around the head's view axis. Clear within VignetteInnerDeg of it, rising smoothly
// to `alpha` at VignetteFullDeg (about the edge of a Quest 3's view) and beyond. The rise is the
// ring's profile to the power `profile_power` (1: the heal vignette; 3, the hit vignette: only the
// very edges, half of `alpha` at 50 instead of 45 degrees, 1% instead of 20% at 40).
constexpr float VignetteInnerDeg = 26.0f;
constexpr float VignetteFullDeg = 56.0f;
constexpr float VignetteOuterDeg = 85.0f;
struct VignetteVisual {
    Vec3 color{0.10f, 0.89f, 0.17f}; // linear rgb
    float alpha{0.35f};
    float distance_m{2.0f};
    float profile_power{1.0f};
    // > 0: its own profile by the angle off the view axis (degrees, VignetteInnerDeg at least):
    // clear inside inner_deg, full from full_deg (the low-HP dark, vitals_edge); with `band` only
    // between them, soft at both ends (the rally's amber rim).
    float inner_deg{0.0f};
    float full_deg{0.0f};
    bool band{false};
    // > 0: weighted toward (wedge_x, wedge_y) around the view axis (x right, y up, unit): the
    // directional hit flash (core/vr/hit_direction.h RingWeight; wedge_focus 0 .. 1, 0 = the plain
    // ring, and the wedge's power).
    float wedge_focus{0.0f};
    float wedge_x{0.0f};
    float wedge_y{1.0f};
    float wedge_power{4.0f};
    // > 0: fitted to what the headset shows (FrameConstants::field, the stagger vignette and the
    // hit flash with stagger_vignette_fit / hit_vignette_fit): r = 1 at the displayed field's edge
    // in every direction (per-axis angles over that side's half-angle), clear inside fit_inner,
    // full from fit_full (a smoothstep, then profile_power and the wedge as before). Replaces the
    // degree profiles above.
    float fit_inner{0.0f};
    float fit_full{1.0f};
};
void AppendVignette(std::vector<DrawItem>& out, const Pose& head, const VignetteVisual& vignette);

// ---- stereo match with the game layer -------------------------------------------------------

// The DIBR warp's near-field compression (vr_stereo_warp_math.h CompressNear, dibr_knee_m /
// dibr_near_clamp_m) as a factor on the disparity of a point `depth_m` in front of the eyes: 1
// beyond knee_m, smaller nearer, never showing it nearer than min_m. 1 when off (knee_m <= 0,
// min_m <= 0 or min_m >= knee_m) or for depth_m <= 0.
float NearDisparityScale(float depth_m, float knee_m, float min_m);

// ---- vial animation -------------------------------------------------------------------------

// Event bits for VialAnimator::Update (set by the input code for the frame they happen in).
enum VialEvent : u32 {
    VialEventArmed = 1u << 0,
    VialEventDisarmed = 1u << 1,      // X again
    VialEventTimedOut = 1u << 2,      // auto-disarm
    VialEventInjectSent = 1u << 3,    // instant injection requested
    VialEventInjectHealed = 1u << 4,  // the game healed (or a dry run)
    VialEventInjectDenied = 1u << 5,  // full HP, no vials, busy, expired...
    VialEventHandedToGame = 1u << 6,  // X held / fallback: the game shows its own vial
};

// Turns the input's armed flag and events into the vial's visual state: a quick pop-in when
// armed, a pulse while armed (faster and dimmer in the last second before an auto-disarm), a
// quick shrink-out when disarmed, the plunge / drain of an injection and a shake when the game
// refuses it. The armed flag is the truth: losing it without an event (a context reset) fades.
class VialAnimator {
public:
    // remaining_s: seconds until the auto-disarm (negative: no timeout). tracked: the left
    // hand's pose is valid. Returns whether the vial is drawn (out is valid then).
    bool Update(float dt, bool armed, float remaining_s, u32 events, bool tracked,
                VialVisual& out);
    void Reset() {
        *this = VialAnimator{};
    }

    static constexpr float AppearSeconds = 0.12f;
    static constexpr float LeaveSeconds = 0.10f;

private:
    enum class Phase : u8 { Hidden, Appear, Armed, Leave, Inject, Denied };
    Phase phase{Phase::Hidden};
    float time{0.0f};       // in the current phase
    float pulse{0.0f};      // pulse phase (cycles)
    float leave_from{1.0f}; // alpha when Leave started
    float level{VialLevelFull};
    float level_from{VialLevelFull};
    float plunge{0.0f};
    float untracked{0.0f};
    float track_fade{1.0f};
    bool healed{false};
    bool denied{false};
};

} // namespace Vulkan::VrOverlay
