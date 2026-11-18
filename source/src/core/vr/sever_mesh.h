// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Experimental geometry only. No guest pointers, renderer hooks, native physics writes,
// or per-frame work. Input positions/normals are a frozen, already-skinned pose, in metres.
// Weights are retained for region validation/interpolation, NOT used to skin these positions
// a second time. A live body render override is a separate, not-yet-implemented adapter.
namespace VR::Sever {
struct Vec3 {
    float x{}, y{}, z{};
};
struct Influence {
    std::uint16_t bone{};
    float weight{};
};
struct Vertex {
    Vec3 position{}, normal{};
    std::array<float, 2> uv{};
    std::array<Influence, 8> skin{}; // input <=4 nonzero; a new edge vertex may need all 8
};
struct Triangle {
    std::array<std::uint32_t, 3> index{};
    std::uint32_t material{};
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
};
enum class WoundLayer : std::uint8_t { Unknown, Anatomy, Covering };
struct Cut {
    Vec3 origin{}, normal{0, 1, 0}; // positive side is the detached piece
    std::uint16_t body_cap_bone{}, piece_cap_bone{};
    // The cut bone's line (its joint, then its child's), for the caps' flesh: each cap's bone
    // where this line crosses that cap's plane. A skin-mode ring sits where the skin blends, up to
    // a bone's length from `origin` (2026-11-16: c8030's forearm stump had its bone 9 cm off, on
    // the rim). Unset (both equal), or nearly along the cap: `origin`, projected onto it.
    Vec3 bone_from{}, bone_to{};
    // Every cap of one cut in one flesh frame (CutEnemy sets it, with the bone's line): uv in
    // metres across the bone (along its line, the bone at (0, 0)), the same for every cap, then
    // scaled by the caller. Layered meshes' caps (skin, clothes over it) overlap: one frame shows
    // them one bone, one texture (2026-11-16: their own frames drew a second bone).
    bool shared_flesh{false};
    std::uint32_t cap_material{0xffffffffu};
    float epsilon{0.00001f};
    float minimum_region_weight{0.5f};
    // A closed surface (a body) is capped on both sides. An open one (cloth: a hood, a mantle,
    // a beard; a sheet has no inside to show) is only split: its section may be open or several
    // pieces, and it gets no cap.
    bool cap{true};
    WoundLayer layer{WoundLayer::Unknown};
    // Skin mode (real enemies: 2026-11-12, c2000's shoulders, mantle and hood rise above its
    // neck joint, so no plane through the neck keeps them off the head). The cut follows the
    // ring where a vertex's weight on the detached bones crosses minimum_region_weight, where
    // the game blends the piece into the body. The plane only turns that ring, by skin_tilt per
    // metre from it, clamped to +-skin_tilt_max of weight: only a vertex already that near the
    // blend can change sides, never a shoulder with a sliver of head weight. Its normal orients
    // the caps. `bias` is added to the field (a nudge). No region guard is needed: the piece is
    // the skin's by construction.
    bool by_skin{false};
    float skin_tilt{0.0f};      // weight per metre from the plane
    float skin_tilt_max{0.25f}; // weight
    float bias{0.0f};           // weight
};
// Hard ceilings also apply to caller-supplied smaller limits. Capped: a single closed convex
// contour only (plane mode); ambiguous, open, coplanar, concave and multiple-loop cuts fail
// intact. A split triangle with no area left (the plane a hair from a corner) is dropped, not a
// failure. Skin mode: boundary_vertices bounds every ring's points together, and separately
// each side's holes' points together.
struct Limits {
    std::uint32_t input_vertices{65536}, input_triangles{131072};
    std::uint32_t output_vertices{524288}, output_triangles{262144}; // both pieces combined
    std::uint32_t boundary_vertices{1024}; // (skin mode: every ring together; c2000's head: 228)
};
enum class Error {
    None,
    InvalidInput,
    Budget,
    NoIntersection,
    OnPlaneVertex,
    WrongRegion,
    OpenOrMultipleContours,
    UnsupportedContour,
    AllocationFailed
};
// An open chain of the cut (skin mode, capped): its points in order, and its pull from the
// body's side to the piece's. It ends on its mesh's border; joined with the next mesh's chain
// across a seam it may close a ring (sever_enemy.h CutEnemy).
struct OpenChain {
    std::vector<Vec3> points;
    Vec3 toward{};
    bool anatomy{}; // set by CutEnemy from the mesh that supplied this chain
};
// A hole an open chain leaves on one side (skin mode, capped): the chain closed by the mesh's
// own border on that side (and any other chains that border meets), in order. Where a head mesh
// ends at the neck, a body mesh's collar, an eye socket: the cut runs into an edge the artist
// left open because another surface covered it. CutEnemy fans it shut (Fill) unless its chains
// closed a ring across meshes. Only across a tube: never over a sheet (its surface lies in the
// hole's own plane: a hem, a flap), which stays open as cloth does.
struct Hole {
    std::vector<Vec3> points;
    Vec3 toward{};                     // its chains' pull, from the body's side to the piece's
    bool piece{false};                 // whose side it opens on
    std::vector<std::uint32_t> chains; // its chains (Result::chains)
};
struct Result {
    Error error{Error::InvalidInput};
    Mesh body, piece;
    std::uint32_t boundary_vertices{};
    // Skin mode, capped: the closed rings capped (each tube the cut crossed: a neck, a strand
    // of hair), and the open chains left open (they end on the mesh's own border, where another
    // mesh goes on, or where the surface branches). The simple ones (no branch) in `chains`,
    // and the holes they leave in `holes`.
    std::uint32_t loops{}, open_chains{};
    std::vector<OpenChain> chains;
    std::vector<Hole> holes;
    // NoIntersection: the side it lies on whole (the cut's own field, its tilt and bias), the
    // piece's (true) or the body's.
    bool whole_piece{false};
    explicit operator bool() const {
        return error == Error::None;
    }
};
// The caps' flesh (sever_flesh.glsl): uv is the cap's own plane in units of its ring's mean
// radius, with the cut's bone (where Cut::bone_from..bone_to crosses it, else Cut::origin
// projected) at (0, 0). A cap the bone does not pass through (a strand, a layer over the skin)
// is plain meat: its (0, 0) OffBoneRadii from its middle, on the bone's side, off the cap.
// Normals are the plane's.
inline constexpr float OffBoneRadii = 1.4f;

// Caps one closed ring of a cut (its points in order) on both sides, appended to `body` and
// `piece` with the cut's cap material, skinned to its cap bones: each cap faces away from its own
// side, by `toward` (the ring's pull from the body's side to the piece's). Triangulated in the
// ring's own plane: a fan from its middle (O(n): a middle vertex, a triangle a point) when the
// ring is star-shaped from it, the usual neck or limb; ear clipping when it is concave; the fan
// anyway when it crosses itself there (a twisted blend: flesh over flesh, never a gap). False,
// nothing appended, only for a ring with no extent and no pull. Split's own rings, and CutEnemy's
// rings joined across meshes.
bool CapRing(std::span<const Vec3> ring, Vec3 toward, const Cut& cut, Mesh& body, Mesh& piece);
// Closes one hole on one side only (the piece's, or the body's), as CapRing closes a ring.
bool Fill(std::span<const Vec3> ring, Vec3 toward, const Cut& cut, bool piece, Mesh& mesh);
// detached_bones is a validated bone-subtree mask (0 or 1 per bone, <=2048 bones).
// Every vertex on the detached side must satisfy the region guard: an arm raised above
// the neck cannot accidentally become another detached part. Failure has empty outputs;
// source is never mutated. Call only on bounded owned snapshots, off the render thread.
Result Split(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
             std::span<const std::uint8_t> detached_bones, const Cut& cut,
             const Limits& limits = {});
} // namespace VR::Sever
