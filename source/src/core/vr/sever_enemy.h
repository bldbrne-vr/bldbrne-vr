// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/vr/sever_mesh.h"
#include "core/vr/sever_policy.h"

// Beast Claw dismemberment, a whole enemy's cut. An enemy
// is several meshes (the game files, 2026-11-12: c2000's head 5k vertices, body 17.5k, arms,
// legs, a weapon, a mantle and a hood). Most never need cutting:
//  - Classify sorts a mesh by its skin: wholly the body's (it stays as it is), wholly the
//    piece's (it comes off whole, uncut), or mixed (it is cut). Per model and cut bone: the live
//    adapter may cache it. Only the mixed meshes are copied into a Snapshot.
//  - CutEnemy cuts every mixed mesh along the same skin ring (sever_mesh.h Cut::by_skin), where
//    the game blends the piece into the body, turned by the claw's swing near that blend:
//    flesh capped (the stump), cloth split without a cap (a hood has no inside to show). Rings
//    that close only across meshes are capped there (JoinChains); a hole the cut opens into a
//    mesh's own open edge (a collar a head mesh covered) is fanned shut (sever_mesh.h Fill).
//    The cut is refused only when a flesh hole it leaves could not be closed and nothing else
//    was; flesh that only comes apart (two sealed shells the skin ring runs between) needs no
//    cap. If any mesh's ring runs through a vertex, every mesh is cut again with the same small
//    nudge: one cut, one ring.
// Pure: owned data in, owned data out; no guest memory.
namespace VR::Sever {

// One cut's ceilings, per mesh (sever_mesh.h's own). The game files (217 enemies): the largest
// single mesh is 13.4k vertices at the median, 37.5k at the 90th percentile; 6 are over 65,536
// (those meshes: no cut). Memory is bounded by Jobs' budget and slots, not by these.
inline constexpr Limits EnemyLimits{
    .input_vertices = 65536,
    .input_triangles = 131072,
    .output_vertices = 65536 + 2048,
    .output_triangles = 131072 + 2048,
    .boundary_vertices = 1024, // every ring of a mesh together (c2000's head: 228 in 19 rings)
};
inline constexpr std::size_t MaxParts = 16;             // mixed meshes one cut may cross
inline constexpr std::uint64_t MaxCutVertices = 131072; // all of them together
// The skin ring's turn toward the swing (sever_mesh.h Cut::skin_tilt): a neck's blend runs
// about a hand's width, so this follows a slash closely inside it.
inline constexpr float SkinTiltPerM = 10.0f;
// The nudge (weight) when a ring runs exactly through a vertex: once each way.
inline constexpr float SkinNudge = 0.001f;

enum class Side : std::uint8_t { Body, Piece, Mixed, Empty };

// A mesh's side by its skin: a vertex is the piece's when at least `threshold` of its weight is
// on the detached bones (Split's mask). Bones past the mask count as the body's.
inline Side Classify(std::span<const Vertex> vertices, std::span<const std::uint8_t> detached,
                     float threshold = 0.5f) {
    bool piece = false, body = false;
    for (const Vertex& v : vertices) {
        float w = 0.0f;
        for (const Influence& i : v.skin) {
            if (i.weight > 0.0f && i.bone < detached.size() && detached[i.bone]) {
                w += i.weight;
            }
        }
        (w >= threshold ? piece : body) = true;
        if (piece && body) {
            return Side::Mixed;
        }
    }
    return piece ? Side::Piece : body ? Side::Body : Side::Empty;
}

// A mesh the ring missed (all one side once the swing turned it): the piece's when most of its
// vertices are, else the body's.
inline Side Majority(std::span<const Vertex> vertices, std::span<const std::uint8_t> detached,
                     float threshold = 0.5f) {
    std::size_t piece = 0;
    for (const Vertex& v : vertices) {
        float w = 0.0f;
        for (const Influence& i : v.skin) {
            if (i.weight > 0.0f && i.bone < detached.size() && detached[i.bone]) {
                w += i.weight;
            }
        }
        piece += w >= threshold;
    }
    return 2 * piece > vertices.size() ? Side::Piece : Side::Body;
}

// A side made rigid: each vertex keeps only its own side's bones (the piece's the detached ones,
// the body's the rest), renormalized. A vertex near the cut blends both; left as it is, the flying
// piece would drag the body's neck after it, and the body would hold the head's. A vertex with
// nothing of its side (none: the cut puts it on the side most of its weight is) goes to the cap
// bone. The cap's own vertices are skinned to it already.
inline void MakeRigid(Mesh& m, std::span<const std::uint8_t> detached, bool piece,
                      std::uint16_t cap_bone) {
    for (Vertex& v : m.vertices) {
        float sum = 0.0f;
        for (Influence& i : v.skin) {
            const bool theirs = i.bone < detached.size() && detached[i.bone] != 0;
            if (i.weight > 0.0f && theirs != piece) {
                i = {};
            }
            sum += i.weight;
        }
        if (!(sum > 0.0f)) {
            v.skin = {};
            v.skin[0] = {cap_bone, 1.0f};
            continue;
        }
        for (Influence& i : v.skin) {
            i.weight /= sum;
        }
    }
}

// Cloth by its material's name (the game's: "mantle_cloth", "hood_cloth", "c2000_c1090_cloth").
constexpr bool ClothMaterial(std::string_view material) {
    return Detail::Has(material, "cloth");
}

// FLVER group names carry the layer even where the material is only a display mask ("#14#").
// Match whole name components, including single-letter appearance variants like "HeadB".
constexpr WoundLayer CutLayer(std::string_view group, std::string_view material) {
    WoundLayer layer = WoundLayer::Unknown;
    for (auto name : {group, material}) {
        while (!name.empty()) {
            std::size_t n = 0;
            while (n < name.size() && Detail::Lower(name[n]) >= 'a' && Detail::Lower(name[n]) <= 'z')
                ++n;
            auto token = name.substr(0, n);
            name.remove_prefix(std::min(n + 1, name.size()));
            if (token.empty()) continue;
            if (token.back() == 'A' || token.back() == 'B')
                token.remove_suffix(1);
            const auto is = [&](std::string_view word) {
                return token.size() == word.size() && Detail::Has(token, word);
            };
            if (is("hair") || is("fur") || is("bandage") || is("shirt") || is("coat") || is("vest"))
                return WoundLayer::Covering;
            if (is("body") || is("head") || is("arm") || is("leg"))
                layer = WoundLayer::Anatomy;
        }
    }
    return layer;
}

// One mixed mesh, posed (the frozen pose, model space, metres) and owned.
struct Part {
    std::uint16_t mesh{0}; // its index in the model (the renderer adapter's)
    bool cloth{false};     // split without a cap
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    WoundLayer layer{WoundLayer::Unknown};
};

// What a cut is given: the mixed meshes, the detached bones and the cut (by_skin is set by
// CutEnemy). Owned, already validated by the live adapter.
struct Snapshot {
    std::vector<Part> parts;
    std::vector<std::uint8_t> detached_bones;
    Cut cut{};
};

struct PartCut {
    std::uint16_t mesh{0};
    bool capped{false};
    Mesh body, piece;
};

struct EnemyCut {
    Error error{Error::InvalidInput};
    std::vector<PartCut> parts;
    // The stumps of rings that close only across meshes (the cut on a seam: the head mesh's
    // chain and the body mesh's joined), skinned to the cap bones like any cap.
    Mesh body_cap, piece_cap;
    std::uint32_t boundary_vertices{0}; // the capped stumps' rings together
    std::uint32_t loops{0};       // rings capped (sever_mesh.h), joined ones included
    std::uint32_t joined{0};      // of the loops, those closed across meshes
    std::uint32_t filled{0};      // holes fanned shut, a side each (Fill)
    std::uint32_t open_chains{0}; // chains still open on a side: branched, or a hole not closed
    float bias{0.0f};                       // the nudge the cut took (0: none)
    explicit operator bool() const {
        return error == Error::None;
    }
};

// Chain ends this close meet: a seam's shared vertices give the same crossing to both meshes.
inline constexpr float JoinM = 0.0002f;

// A ring closed across meshes: its points in order, its pull, the chains it joined.
struct JoinedRing {
    std::vector<Vec3> points;
    Vec3 toward{};
    std::vector<std::uint32_t> chains;
};

// The open chains of every mesh joined end to end into closed rings. Chains that close no ring
// stay open (their holes may still be filled).
inline std::vector<JoinedRing> JoinChains(const std::vector<OpenChain>& chains) {
    const auto meets = [](Vec3 a, Vec3 b) {
        const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return x * x + y * y + z * z <= JoinM * JoinM;
    };
    std::vector<JoinedRing> rings;
    std::vector<std::uint8_t> used(chains.size(), 0);
    JoinedRing ring;
    for (std::size_t i = 0; i < chains.size(); ++i) {
        if (used[i] || chains[i].points.empty()) {
            continue;
        }
        used[i] = 1;
        ring.points = chains[i].points;
        ring.toward = chains[i].toward;
        ring.chains.assign(1, static_cast<std::uint32_t>(i));
        bool closed = ring.points.size() > 2 && meets(ring.points.front(), ring.points.back());
        for (std::size_t guard = 0; guard < chains.size() && !closed; ++guard) {
            bool grew = false;
            for (std::size_t j = 0; j < chains.size() && !grew; ++j) {
                if (used[j] || chains[j].points.empty()) {
                    continue;
                }
                const auto& next = chains[j].points;
                const bool forward = meets(ring.points.back(), next.front());
                if (!forward && !meets(ring.points.back(), next.back())) {
                    continue;
                }
                // Its first point is the ring's last (the same seam crossing): skipped.
                if (forward) {
                    ring.points.insert(ring.points.end(), next.begin() + 1, next.end());
                } else {
                    ring.points.insert(ring.points.end(), next.rbegin() + 1, next.rend());
                }
                ring.toward = {ring.toward.x + chains[j].toward.x,
                               ring.toward.y + chains[j].toward.y,
                               ring.toward.z + chains[j].toward.z};
                ring.chains.push_back(static_cast<std::uint32_t>(j));
                used[j] = 1;
                grew = true;
            }
            if (!grew) {
                break;
            }
            closed = ring.points.size() > 2 && meets(ring.points.front(), ring.points.back());
        }
        if (!closed || ring.points.size() < 4) {
            continue;
        }
        ring.points.pop_back(); // (its last point is its first)
        if (ring.points.size() <= EnemyLimits.boundary_vertices) {
            rings.push_back(std::move(ring));
        }
        ring = {};
    }
    return rings;
}

inline std::uint64_t Bytes(const Mesh& m) {
    return static_cast<std::uint64_t>(m.vertices.capacity()) * sizeof(Vertex) +
           static_cast<std::uint64_t>(m.triangles.capacity()) * sizeof(Triangle);
}
inline std::uint64_t Bytes(const Part& p) {
    return static_cast<std::uint64_t>(p.vertices.capacity()) * sizeof(Vertex) +
           static_cast<std::uint64_t>(p.triangles.capacity()) * sizeof(Triangle);
}
inline std::uint64_t Bytes(const Snapshot& s) {
    std::uint64_t n = s.parts.capacity() * sizeof(Part) + s.detached_bones.capacity();
    for (const Part& p : s.parts) {
        n += Bytes(p);
    }
    return n;
}
inline std::uint64_t Bytes(const EnemyCut& c) {
    std::uint64_t n = c.parts.capacity() * sizeof(PartCut) + Bytes(c.body_cap) + Bytes(c.piece_cap);
    for (const PartCut& p : c.parts) {
        n += Bytes(p.body) + Bytes(p.piece);
    }
    return n;
}

// One side's caps of one mesh at most (vertices, and as many triangles): the boundary limit's
// points, and a middle for every ring or hole fanned (three points each at least).
inline constexpr std::uint64_t CapVertices =
    EnemyLimits.boundary_vertices + EnemyLimits.boundary_vertices / 3 + 1;

// The memory a cut of `parts` meshes, `vertices` / `triangles` in all, holds at its peak: its
// snapshot (with a bone mask), the cutter's working arrays for the mesh it is on (a distance and
// two index maps a vertex; for its holes, a weld, its hash table and each vertex's triangles: at
// most the largest mesh, counted as all) and every result, sized before it is filled (never
// grown, never trimmed). Each mesh's result is its source plus what the cut adds: a capped cut
// crosses at most twice its boundary limit's triangles, each adding two vertices and up to two
// triangles a side, and its caps a side. The shared caps: rings joined (both sides) and holes
// filled (each side its own), each within the boundary limit a mesh.
inline std::uint64_t EstimateBytes(std::uint64_t parts, std::uint64_t vertices,
                                   std::uint64_t triangles) {
    const std::uint64_t crossing = 2ull * EnemyLimits.boundary_vertices;
    // A side: its own rings and the rings joined across meshes come from its ring points (the
    // boundary limit), its holes from its own budget (the same limit): two CapVertices a side.
    const std::uint64_t caps = 4 * CapVertices;
    const std::uint64_t added = parts * (4 * crossing + caps);
    const std::uint64_t snapshot = parts * (sizeof(Part) + sizeof(PartCut)) +
                                   vertices * sizeof(Vertex) + triangles * sizeof(Triangle) +
                                   MaxBones;
    const std::uint64_t working = vertices * (sizeof(double) + 2 * sizeof(std::uint32_t) +
                                              10 * sizeof(std::uint32_t)) +
                                  triangles * 3 * sizeof(std::uint32_t);
    const std::uint64_t result =
        (vertices + added) * sizeof(Vertex) + (triangles + added) * sizeof(Triangle);
    return snapshot + working + result;
}

// A cloth mesh (uncapped) is not held to the boundary limit: its cut may cross any of its
// triangles. What its result may hold past EstimateBytes' share for a mesh (each side reserves its
// own and two vertices and two triangles a crossed triangle, within the output limits; Split).
inline std::uint64_t ClothExtraBytes(const Snapshot& s) {
    const std::uint64_t share = 4ull * 2ull * EnemyLimits.boundary_vertices + 4 * CapVertices;
    std::uint64_t n = 0;
    for (const Part& p : s.parts) {
        if (!p.cloth) {
            continue;
        }
        const std::uint64_t v = p.vertices.size(), t = p.triangles.size();
        const std::uint64_t vmax = std::min<std::uint64_t>(v + 4 * t, 2ull * EnemyLimits.output_vertices);
        const std::uint64_t tmax = std::min<std::uint64_t>(5 * t, 2ull * EnemyLimits.output_triangles);
        n += (vmax > v + share ? vmax - v - share : 0) * sizeof(Vertex) +
             (tmax > t + share ? tmax - t - share : 0) * sizeof(Triangle);
    }
    return n;
}

namespace Detail {
inline bool BoneLine(const Cut& c) {
    const float x = c.bone_to.x - c.bone_from.x, y = c.bone_to.y - c.bone_from.y,
                z = c.bone_to.z - c.bone_from.z;
    const float l2 = x * x + y * y + z * z;
    return std::isfinite(l2) && l2 > 1e-12f;
}

// A shared flesh frame's caps (uv in metres across the bone) scaled to the flesh's units (the
// bone at (0, 0), the limb's radius 1: sever_flesh.h): by the innermost cap the bone passes
// through, its points' mean distance from the bone (the skin's, not a coat's over it). No cap
// holds the bone (its line misses them all): the largest cap's. Both sides alike (one cut).
// O(cap vertices + cap triangles).
inline void ScaleFlesh(EnemyCut& out, std::uint32_t cap_material) {
    struct Group {
        double area{0}, rim{0};
        unsigned points{0};
        bool bone{false};
    };
    std::vector<Group> groups;
    std::vector<std::uint32_t> root;
    std::vector<std::int32_t> slot;
    const auto groups_of = [&](const Mesh& m) {
        // A cap's vertices are its own (Emit): its groups by union over its triangles.
        root.resize(m.vertices.size());
        for (std::uint32_t i = 0; i < root.size(); ++i) {
            root[i] = i;
        }
        const auto find = [&](std::uint32_t x) {
            while (root[x] != x) {
                x = root[x] = root[root[x]];
            }
            return x;
        };
        for (const Triangle& t : m.triangles) {
            if (t.material == cap_material) {
                root[find(t.index[1])] = find(t.index[0]);
                root[find(t.index[2])] = find(t.index[0]);
            }
        }
        slot.assign(m.vertices.size(), -1);
        std::vector<std::uint8_t> counted(m.vertices.size(), 0);
        for (const Triangle& t : m.triangles) {
            if (t.material != cap_material) {
                continue;
            }
            const std::uint32_t r = find(t.index[0]);
            if (slot[r] < 0) {
                slot[r] = static_cast<std::int32_t>(groups.size());
                groups.emplace_back();
            }
            Group& g = groups[static_cast<std::size_t>(slot[r])];
            for (const auto i : t.index) {
                if (!counted[i]) {
                    counted[i] = 1;
                    const auto& uv = m.vertices[i].uv;
                    g.rim += std::sqrt(double(uv[0]) * uv[0] + double(uv[1]) * uv[1]);
                    ++g.points;
                }
            }
            const auto& a = m.vertices[t.index[0]].uv;
            const auto& b = m.vertices[t.index[1]].uv;
            const auto& c = m.vertices[t.index[2]].uv;
            const double c0 = double(b[0] - a[0]) * -a[1] - double(b[1] - a[1]) * -a[0];
            const double c1 = double(c[0] - b[0]) * -b[1] - double(c[1] - b[1]) * -b[0];
            const double c2 = double(a[0] - c[0]) * -c[1] - double(a[1] - c[1]) * -c[0];
            g.bone = g.bone || (c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0);
            g.area += 0.5 * std::abs(double(b[0] - a[0]) * (c[1] - a[1]) -
                                     double(b[1] - a[1]) * (c[0] - a[0]));
        }
    };
    std::vector<Mesh*> meshes;
    for (PartCut& p : out.parts) {
        meshes.push_back(&p.body);
        meshes.push_back(&p.piece);
    }
    meshes.push_back(&out.body_cap);
    meshes.push_back(&out.piece_cap);
    for (const Mesh* m : meshes) {
        groups_of(*m);
    }
    double inner = 0, inner_area = 0, largest = 0, largest_area = -1;
    for (const Group& g : groups) {
        const double radius = g.points > 0 ? g.rim / g.points : 0; // (its fan's middle too)
        if (!(radius > 1e-4)) {
            continue;
        }
        if (g.bone && (inner == 0 || g.area < inner_area)) {
            inner = radius;
            inner_area = g.area;
        }
        if (g.area > largest_area) {
            largest = radius;
            largest_area = g.area;
        }
    }
    const double radius = inner > 0 ? inner : largest;
    if (!(radius > 1e-4) || !std::isfinite(radius)) {
        return;
    }
    const auto k = static_cast<float>(1 / radius);
    for (Mesh* m : meshes) {
        std::vector<std::uint8_t> done(m->vertices.size(), 0);
        for (const Triangle& t : m->triangles) {
            if (t.material != cap_material) {
                continue;
            }
            for (const auto i : t.index) {
                if (!done[i]) {
                    done[i] = 1;
                    m->vertices[i].uv[0] *= k;
                    m->vertices[i].uv[1] *= k;
                }
            }
        }
    }
}
struct SeamCaps {
    Mesh body, piece;
    std::uint32_t joined{}, filled{}, boundary_vertices{}, open_chains{};
    bool skin_closed{};
};

inline SeamCaps CloseSeams(const std::vector<OpenChain>& chains, const std::vector<Hole>& holes,
                           const Cut& c) {
    SeamCaps result;
    // The cut on a seam: each mesh's chain ends on its border; joined, they may close a
    // ring, capped once. Every other hole a chain leaves is fanned shut on its side,
    // unless one of its chains went into a ring. The shared caps are sized first.
    const std::vector<JoinedRing> rings = JoinChains(chains);
    std::vector<std::uint8_t> in_ring(chains.size(), 0);
    for (const JoinedRing& ring : rings) {
        for (const auto k : ring.chains) {
            in_ring[k] = 1;
        }
    }
    const auto unjoined = [&](const Hole& hole) {
        return std::none_of(hole.chains.begin(), hole.chains.end(),
                            [&](std::uint32_t k) { return in_ring[k] != 0; });
    };
    std::size_t body = 0, piece = 0;
    for (const JoinedRing& ring : rings) {
        body += ring.points.size() + 1;
        piece += ring.points.size() + 1;
    }
    for (const Hole& hole : holes) {
        if (unjoined(hole)) {
            (hole.piece ? piece : body) += hole.points.size() + 1;
        }
    }
    result.body.vertices.reserve(body);
    result.body.triangles.reserve(body);
    result.piece.vertices.reserve(piece);
    result.piece.triangles.reserve(piece);
    // Each chain's sides closed (1 the body's, 2 the piece's).
    std::vector<std::uint8_t> sealed(chains.size(), 0);
    for (const JoinedRing& ring : rings) {
        if (CapRing(ring.points, ring.toward, c, result.body, result.piece)) {
            ++result.joined;
            result.boundary_vertices += static_cast<std::uint32_t>(ring.points.size());
            bool skin_ring = true;
            for (const auto k : ring.chains) {
                sealed[k] = 3;
                skin_ring &= chains[k].anatomy;
            }
            result.skin_closed |= skin_ring;
        }
    }
    for (const Hole& hole : holes) {
        if (unjoined(hole) && Fill(hole.points, hole.toward, c, hole.piece,
                                   hole.piece ? result.piece : result.body)) {
            ++result.filled;
            result.boundary_vertices += static_cast<std::uint32_t>(hole.points.size());
            for (const auto k : hole.chains) {
                sealed[k] |= hole.piece ? 2 : 1;
            }
        }
    }
    for (const auto sides : sealed) {
        result.open_chains += sides != 3;
    }
    return result;
}

} // namespace Detail

// The standard cut (Jobs::RunNext's): every mixed mesh along the same skin ring. A mesh the
// ring misses entirely (all one side after the tilt) goes whole to that side.
inline EnemyCut CutEnemy(const Snapshot& s) {
    EnemyCut out;
    if (s.parts.empty() || s.parts.size() > MaxParts) {
        return out;
    }
    // Its caps in one flesh frame when the bone's line is known (sever_mesh.h Cut::shared_flesh),
    // scaled at the end (Detail::ScaleFlesh).
    const bool shared_flesh = Detail::BoneLine(s.cut);
    // Cut coverings last: only omit their flesh when the skin underneath produced an actual
    // closed ring on both halves. Keep output and draw order in the original mesh order.
    std::array<std::size_t, MaxParts> order;
    std::size_t count = 0;
    std::size_t first_covering = 0;
    for (const bool covering : {false, true}) {
        if (covering) first_covering = count;
        for (std::size_t i = 0; i < s.parts.size(); ++i)
            if ((s.parts[i].layer == WoundLayer::Covering) == covering)
                order[count++] = i;
    }
    for (const float bias : {0.0f, SkinNudge, -SkinNudge}) {
        out = EnemyCut{};
        out.parts.resize(s.parts.size());
        out.bias = bias;
        bool nudge = false;
        bool capped_any = false; // a flesh mesh's own ring capped
        bool separated = false;  // a flesh mesh came apart
        std::uint32_t branched = 0;
        Error failed = Error::None;
        std::vector<OpenChain> chains; // every flesh mesh's, for rings that close across meshes
        std::vector<Hole> holes;       // their chains: indices into `chains`
        Cut cut = s.cut;
        cut.shared_flesh = shared_flesh;
        cut.by_skin = true;
        cut.bias = bias;
        bool skin_closed = false, has_skin_chains = false;
        Detail::SeamCaps seams;
        std::size_t resolved_chains = 0;
        for (std::size_t step = 0; step < count; ++step) {
            const std::size_t index = order[step];
            const Part& p = s.parts[index];
            if (step == first_covering && !skin_closed && has_skin_chains) {
                // The skin may only close across meshes. Resolve it before deciding whether
                // clothing needs its own filling. Reuse these caps if no new chains appear.
                seams = Detail::CloseSeams(chains, holes, cut);
                resolved_chains = chains.size();
                skin_closed = seams.skin_closed;
            }
            Cut c = cut;
            c.cap = !p.cloth && !(skin_closed && p.layer == WoundLayer::Covering);
            c.layer = p.cloth ? WoundLayer::Unknown : p.layer;
            Result r = Split(p.vertices, p.triangles, s.detached_bones, c, EnemyLimits);
            if (r.error == Error::OnPlaneVertex) {
                nudge = true;
                break;
            }
            if (r.error == Error::NoIntersection) {
                // Wholly one side once the swing turned the ring: it goes whole with that side
                // (the cut's own: by its tilted field, not by the vertices' weights alone).
                PartCut whole{p.mesh, false, {}, {}};
                Mesh& to = r.whole_piece ? whole.piece : whole.body;
                to.vertices = p.vertices;
                to.triangles = p.triangles;
                out.parts[index] = std::move(whole);
                continue;
            }
            if (!r) {
                failed = r.error;
                break;
            }
            const bool capped = c.cap && r.loops > 0;
            skin_closed |= capped && c.layer == WoundLayer::Anatomy;
            capped_any |= capped;
            separated |= c.cap && !r.body.triangles.empty() && !r.piece.triangles.empty();
            out.boundary_vertices += r.boundary_vertices;
            out.loops += r.loops;
            branched += r.open_chains - static_cast<std::uint32_t>(r.chains.size());
            const auto base = static_cast<std::uint32_t>(chains.size());
            has_skin_chains |= c.layer == WoundLayer::Anatomy && !r.chains.empty();
            for (OpenChain& chain : r.chains) {
                chain.anatomy = c.layer == WoundLayer::Anatomy;
                chains.push_back(std::move(chain));
            }
            for (Hole& hole : r.holes) {
                for (auto& k : hole.chains) {
                    k += base;
                }
                holes.push_back(std::move(hole));
            }
            out.parts[index] = {p.mesh, capped, std::move(r.body), std::move(r.piece)};
        }
        if (nudge) {
            continue; // every mesh again, with the same nudge
        }
        if (failed == Error::None && !chains.empty()) {
            if (resolved_chains != chains.size()) {
                seams = {}; // release tentative caps before joining fallback garment chains
                seams = Detail::CloseSeams(chains, holes, cut);
            }
            out.body_cap = std::move(seams.body);
            out.piece_cap = std::move(seams.piece);
            out.joined = seams.joined;
            out.filled = seams.filled;
            out.loops += seams.joined;
            out.boundary_vertices += seams.boundary_vertices;
            out.open_chains += seams.open_chains;
        }
        out.open_chains += branched;
        const bool stump = capped_any || out.joined > 0 || out.filled > 0;
        if (failed == Error::None && !stump && !(separated && out.open_chains == 0)) {
            // Nothing closed and a flesh hole left open (or no flesh came apart): never shown.
            failed = Error::OpenOrMultipleContours;
        }
        if (failed != Error::None) {
            EnemyCut refused;
            refused.error = failed;
            return refused;
        }
        out.error = Error::None;
        if (shared_flesh) {
            Detail::ScaleFlesh(out, s.cut.cap_material);
        }
        return out;
    }
    EnemyCut refused;
    refused.error = Error::OnPlaneVertex;
    return refused;
}

} // namespace VR::Sever
