// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include "sever_mesh.h"

namespace VR::Sever {
namespace {
Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 Mul(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
double Dot(Vec3 a, Vec3 b) {
    return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
}
Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
bool Finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Vec3 Unit(Vec3 v) {
    const double length = std::sqrt(Dot(v, v));
    if (!std::isfinite(length) || length < 1e-12)
        throw Error::InvalidInput;
    return Mul(v, static_cast<float>(1 / length));
}
Vertex Mix(const Vertex& a, const Vertex& b, float t) {
    Vertex v;
    v.position = Add(Mul(a.position, 1 - t), Mul(b.position, t));
    // Opposite normals (a two-sided sheet's back-to-back vertices) blend to nothing: the nearer
    // end's normal instead.
    const Vec3 blend = Add(Mul(a.normal, 1 - t), Mul(b.normal, t));
    v.normal = Dot(blend, blend) > 1e-12 ? Unit(blend) : Unit(t < 0.5f ? a.normal : b.normal);
    for (unsigned i = 0; i < 2; ++i)
        v.uv[i] = a.uv[i] * (1 - t) + b.uv[i] * t;
    unsigned count = 0;
    const auto add = [&](Influence w, float scale) {
        w.weight *= scale;
        if (w.weight == 0)
            return;
        unsigned i = 0;
        while (i < count && v.skin[i].bone != w.bone)
            ++i;
        if (i == count) {
            if (count == v.skin.size())
                throw Error::InvalidInput;
            v.skin[count++] = {w.bone, 0};
        }
        v.skin[i].weight += w.weight;
    };
    for (auto w : a.skin)
        add(w, 1 - t);
    for (auto w : b.skin)
        add(w, t);
    float sum = 0;
    for (auto w : v.skin)
        sum += w.weight;
    if (!std::isfinite(sum) || sum <= 0)
        throw Error::InvalidInput;
    for (auto& w : v.skin)
        w.weight /= sum;
    return v;
}
struct Node {
    Vec3 position;
    std::array<unsigned, 4> adjacent{};
    unsigned degree{}; // (skin mode: past 2, a branch; the first four neighbours kept)
    Vec3 toward{};     // (skin mode) its crossing edges' pull, from the body's end to the piece's
    std::array<std::uint32_t, 2> edge{}; // the source edge it was first found on
};

// A cap's frame: its middle, its own plane (Newell's normal, turned by `toward` to face the piece
// side) and its flesh's texture space (sever_mesh.h CapRing: the cut's bone at (0, 0), the ring's
// mean radius 1).
struct CapFrame {
    Vec3 middle{}, l{}, u{}, v{};
    double bone_u{}, bone_v{}, scale{};
    bool reversed{}; // the ring runs clockwise about l: taken backwards
    // Cut::shared_flesh: uv across the bone's line (from `from`, along su and sv), in metres.
    bool shared{};
    Vec3 from{}, su{}, sv{};
};
bool Frame(std::span<const Vec3> ring, Vec3 toward, const Cut& c, CapFrame& f) {
    const std::size_t n = ring.size();
    if (n < 3)
        return false;
    for (const Vec3& p : ring)
        f.middle = Add(f.middle, Mul(p, 1.f / n));
    Vec3 normal{};
    for (std::size_t i = 0; i < n; ++i)
        normal = Add(normal, Cross(Sub(ring[i], f.middle), Sub(ring[(i + 1) % n], f.middle)));
    const double eps2 = double(c.epsilon) * c.epsilon;
    double length = std::sqrt(Dot(normal, normal));
    if (!Finite(f.middle) || !Finite(normal) || !std::isfinite(length))
        return false;
    if (length * length <= eps2 * eps2) {
        // No area in any plane (a ring folded flat on itself): it faces its pull.
        normal = toward;
        length = std::sqrt(Dot(normal, normal));
        if (!Finite(normal) || !std::isfinite(length) || length < 1e-12)
            return false;
    }
    f.reversed = Dot(normal, toward) < 0;
    f.l = Mul(normal, static_cast<float>((f.reversed ? -1.0 : 1.0) / length));
    const Vec3 axis = std::abs(f.l.x) < .9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 across = Cross(f.l, axis);
    f.u = Mul(across, static_cast<float>(1 / std::sqrt(Dot(across, across))));
    f.v = Cross(f.l, f.u);
    double radius = 0;
    for (const Vec3& p : ring) {
        const Vec3 d = Sub(p, f.middle);
        radius += std::sqrt(Dot(d, d)) / n;
    }
    if (!(radius > c.epsilon))
        return false;
    f.scale = 1 / radius;
    if (c.shared_flesh) {
        const Vec3 line = Sub(c.bone_to, c.bone_from);
        const double length = std::sqrt(Dot(line, line));
        if (length > 1e-6 && Finite(line)) {
            const Vec3 d = Mul(line, static_cast<float>(1 / length));
            const Vec3 across = Cross(d, std::abs(d.x) < .9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
            f.su = Unit(across);
            f.sv = Cross(d, f.su);
            f.from = c.bone_from;
            f.shared = true;
            return true;
        }
    }
    // The bone on this cap: where the cut bone's line crosses its plane (sever_mesh.h
    // Cut::bone_from), else the cut's origin projected onto it.
    Vec3 bone = Sub(c.origin, f.middle);
    const Vec3 line = Sub(c.bone_to, c.bone_from);
    const double line_length = std::sqrt(Dot(line, line));
    const double along = Dot(line, f.l);
    if (line_length > 1e-6 && std::abs(along) > 0.2 * line_length) {
        const double t = Dot(Sub(f.middle, c.bone_from), f.l) / along;
        const Vec3 at = Add(c.bone_from, Mul(line, static_cast<float>(t)));
        if (Finite(at))
            bone = Sub(at, f.middle);
    }
    f.bone_u = Dot(bone, f.u);
    f.bone_v = Dot(bone, f.v);
    // Off the cap (even-odd, in its plane): no bone on it, plain meat on the tile.
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec3 a = Sub(ring[i], f.middle), b = Sub(ring[j], f.middle);
        const double ax = Dot(a, f.u), ay = Dot(a, f.v), bx = Dot(b, f.u), by = Dot(b, f.v);
        if ((ay > f.bone_v) != (by > f.bone_v) &&
            f.bone_u < (bx - ax) * (f.bone_v - ay) / (by - ay) + ax)
            inside = !inside;
    }
    if (!inside) {
        const double d = std::sqrt(f.bone_u * f.bone_u + f.bone_v * f.bone_v);
        const double off = OffBoneRadii * radius;
        f.bone_u = d > 1e-9 ? f.bone_u / d * off : off;
        f.bone_v = d > 1e-9 ? f.bone_v / d * off : 0.0;
    }
    return true;
}
// The fan from the middle (index n, after the ring's n points): a triangle a point.
// The fan from the middle (index n, at `p`'s origin): each triangle turned counter-clockwise (a
// ring that crosses itself winds back in places: there the plain fan would face into the body).
void FanOf(std::span<const std::array<double, 2>> p, std::vector<std::array<unsigned, 3>>& out) {
    const auto n = static_cast<unsigned>(p.size());
    out.clear();
    for (unsigned i = 0; i < n; ++i) {
        const unsigned j = (i + 1) % n;
        if (p[i][0] * p[j][1] - p[i][1] * p[j][0] >= 0)
            out.push_back({n, i, j});
        else
            out.push_back({n, j, i});
    }
}
// One side's cap: the ring's points counter-clockwise about l (and its middle after them, for a
// fan), its triangles' indices into them; skinned to that side's cap bone, facing away from it.
void Emit(const CapFrame& f, std::span<const Vec3> ring, bool fan,
          std::span<const std::array<unsigned, 3>> triangles, const Cut& c, bool upper,
          Mesh& mesh) {
    const double eps2 = double(c.epsilon) * c.epsilon;
    Vertex cap;
    cap.normal = Mul(f.l, upper ? -1.f : 1.f);
    cap.skin[0] = {upper ? c.piece_cap_bone : c.body_cap_bone, 1};
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto put = [&](Vec3 p) {
        cap.position = p;
        if (f.shared) {
            const Vec3 d = Sub(p, f.from);
            cap.uv = {static_cast<float>(Dot(d, f.su)), static_cast<float>(Dot(d, f.sv))};
        } else {
            const Vec3 d = Sub(p, f.middle);
            cap.uv = {static_cast<float>((Dot(d, f.u) - f.bone_u) * f.scale),
                      static_cast<float>((Dot(d, f.v) - f.bone_v) * f.scale)};
        }
        mesh.vertices.push_back(cap);
    };
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i)
        put(ring[f.reversed ? n - 1 - i : i]);
    if (fan)
        put(f.middle);
    for (const auto& t : triangles) {
        const Vec3 a = mesh.vertices[first + t[0]].position,
                   b = mesh.vertices[first + t[1]].position,
                   d = mesh.vertices[first + t[2]].position;
        const Vec3 x = Cross(Sub(b, a), Sub(d, a));
        if (Dot(x, x) <= eps2 * eps2)
            continue; // a flat corner's triangle: no area, dropped
        mesh.triangles.push_back(
            {upper ? std::array<std::uint32_t, 3>{first + t[0], first + t[2], first + t[1]}
                   : std::array<std::uint32_t, 3>{first + t[0], first + t[1], first + t[2]},
             c.cap_material});
    }
}

constexpr std::uint32_t Missing = 0xffffffffu;
bool Same(Vec3 a, Vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
std::uint64_t PositionHash(Vec3 p) {
    const auto bits = [](float x) {
        return x == 0.0f ? 0ull : std::uint64_t{std::bit_cast<std::uint32_t>(x)}; // -0 is 0
    };
    std::uint64_t h = bits(p.x) * 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 29)) + bits(p.y) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 31)) + bits(p.z) * 0x94D049BB133111EBull;
    return h ^ (h >> 32);
}
// A hole over a sheet (a hem, a flap: the surface round it lies in the hole's own plane) is no
// wound to close: its rim's normals lie along the hole's normal, on average past 45 degrees.
// A tube's opening (a collar, a neck) has them across it.
bool Sheet(std::span<const Vec3> points, std::span<const std::uint32_t> rim,
           std::span<const Vertex> vertices, Vec3 toward, bool anatomy) {
    const std::size_t n = points.size();
    Vec3 middle{}, normal{};
    for (const Vec3& p : points)
        middle = Add(middle, Mul(p, 1.f / n));
    for (std::size_t i = 0; i < n; ++i)
        normal = Add(normal, Cross(Sub(points[i], middle), Sub(points[(i + 1) % n], middle)));
    double length = std::sqrt(Dot(normal, normal));
    if (!(length > 1e-12)) {
        normal = toward;
        length = std::sqrt(Dot(normal, normal));
        if (!(length > 1e-12))
            return true;
    }
    double along = 0;
    for (const std::uint32_t w : rim) {
        const Vec3 m = vertices[w].normal;
        along += std::abs(Dot(m, normal)) / (length * std::sqrt(Dot(m, m)));
    }
    if (along <= 0.70710678 * rim.size())
        return false;
    // A smooth shading normal at one border vertex is poor evidence for a sheet on a
    // known skin mesh. A rim with measurable depth around its own plane is a curved opening.
    if (anatomy) {
        double radius2 = 0, height = 0;
        for (Vec3 p : points) {
            const Vec3 d = Sub(p, middle);
            radius2 = std::max(radius2, Dot(d, d));
            height = std::max(height, std::abs(Dot(d, normal)));
        }
        constexpr double flat_ratio = 0.025;
        if (height * height > flat_ratio * flat_ratio * radius2 * length * length)
            return false;
    }
    return true;
}
// The holes the open chains leave (skin mode, capped; sever_mesh.h Hole): on each side, a chain
// closed by the source's own border, walked from its far end (and through any other chain that
// border meets) back to its start. Welded by exact position (a texture seam's two vertices are
// one). Only meshes with open chains pay: O(V + T) once, then each hole's length. A border that
// branches, or a walk past the side's budget of points, leaves that hole open.
void FindHoles(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
               std::span<const double> distances, std::span<const Node> nodes,
               std::span<const std::array<unsigned, 2>> ends, std::size_t limit, bool anatomy,
               Result& out) {
    const auto count = static_cast<std::uint32_t>(vertices.size());
    std::vector<std::uint32_t> weld(count);
    {
        std::size_t size = 16;
        while (size < 2 * std::size_t{count})
            size *= 2;
        std::vector<std::uint32_t> table(size, Missing);
        for (std::uint32_t i = 0; i < count; ++i) {
            const Vec3 p = vertices[i].position;
            std::size_t h = PositionHash(p) & (size - 1);
            while (table[h] != Missing && !Same(vertices[table[h]].position, p))
                h = (h + 1) & (size - 1);
            if (table[h] == Missing)
                table[h] = i;
            weld[i] = table[h];
        }
    }
    // Each welded vertex's triangles: around[first[w] .. first[w + 1]).
    std::vector<std::uint32_t> first(std::size_t{count} + 1, 0), around(3 * triangles.size());
    for (const Triangle& t : triangles)
        for (const auto i : t.index)
            ++first[weld[i]];
    std::uint32_t sum = 0;
    for (std::uint32_t w = 0; w < count; ++w)
        first[w] = sum += first[w];
    first[count] = sum;
    for (std::uint32_t t = 0; t < triangles.size(); ++t)
        for (const auto i : triangles[t].index)
            around[--first[weld[i]]] = t;
    // Follow the incoming border through its own fan of triangles. Two separate sheets may
    // touch at a welded point; counting all its border neighbours would stop both walks there.
    const auto next_border = [&](std::uint32_t w, std::uint32_t back) {
        struct Neighbour {
            std::uint32_t vertex;
            unsigned degree{};
            std::array<unsigned, 2> links{};
        };
        std::array<Neighbour, 32> fan;
        unsigned size = 0, incoming = 0;
        for (std::uint32_t k = first[w]; k < first[w + 1]; ++k) {
            std::array<unsigned, 2> pair;
            unsigned count = 0;
            for (const auto i : triangles[around[k]].index) {
                const std::uint32_t x = weld[i];
                if (x == w)
                    continue;
                unsigned j = 0;
                while (j < size && fan[j].vertex != x)
                    ++j;
                if (j == size) {
                    if (size == fan.size())
                        return Missing;
                    fan[size++] = {x};
                }
                if (x == back)
                    incoming = j;
                pair[count++] = j;
            }
            if (count != 2)
                return Missing;
            for (unsigned j = 0; j != 2; ++j) {
                auto& n = fan[pair[j]];
                if (n.degree < 2)
                    n.links[n.degree] = pair[1 - j];
                ++n.degree;
            }
        }
        if (!size || fan[incoming].vertex != back || fan[incoming].degree != 1)
            return Missing;
        unsigned prev = incoming, at = fan[incoming].links[0];
        for (unsigned steps = 0; steps < size; ++steps) {
            const auto& n = fan[at];
            if (n.degree == 1)
                return n.vertex;
            if (n.degree != 2)
                break;
            const unsigned next = n.links[n.links[0] == prev ? 1 : 0];
            prev = at;
            at = next;
        }
        return Missing;
    };
    const auto up = [&](std::uint32_t w) { return distances[w] > 0; };
    // Each chain's ends' border edges, welded.
    const std::size_t chains = out.chains.size();
    std::vector<std::array<std::array<std::uint32_t, 2>, 2>> edge_of(chains);
    for (std::size_t k = 0; k < chains; ++k)
        for (unsigned e = 0; e < 2; ++e)
            edge_of[k][e] = {weld[nodes[ends[k][e]].edge[0]], weld[nodes[ends[k][e]].edge[1]]};
    const auto end_on = [&](std::uint32_t a, std::uint32_t b, std::size_t& k, unsigned& e) {
        for (k = 0; k < chains; ++k)
            for (e = 0; e < 2; ++e)
                if ((edge_of[k][e][0] == a && edge_of[k][e][1] == b) ||
                    (edge_of[k][e][0] == b && edge_of[k][e][1] == a))
                    return true;
        return false;
    };
    std::vector<std::uint8_t> done(chains);
    std::vector<std::uint32_t> rim;
    for (const bool upper : {false, true}) {
        std::fill(done.begin(), done.end(), std::uint8_t{0});
        std::size_t budget = limit;
        for (std::size_t k0 = 0; k0 < chains; ++k0) {
            if (done[k0])
                continue;
            Hole hole;
            hole.piece = upper;
            rim.clear();
            std::size_t k = k0;
            unsigned from = 0; // chain k, entered at its end `from`
            bool closed = false;
            for (std::size_t guard = 0; guard <= chains && !closed; ++guard) {
                done[k] = 1;
                hole.chains.push_back(static_cast<std::uint32_t>(k));
                const auto& points = out.chains[k].points;
                if (from == 0)
                    hole.points.insert(hole.points.end(), points.begin(), points.end());
                else
                    hole.points.insert(hole.points.end(), points.rbegin(), points.rend());
                hole.toward = Add(hole.toward, out.chains[k].toward);
                // Off its far end onto this side's end of that border edge, then along the
                // border until it crosses the cut again, where the next chain goes on.
                const auto e = edge_of[k][1 - from];
                if (up(e[0]) == up(e[1]))
                    break;
                std::uint32_t at = up(e[0]) == upper ? e[0] : e[1];
                std::uint32_t back = at == e[0] ? e[1] : e[0];
                bool onward = false;
                while (hole.points.size() <= budget) {
                    hole.points.push_back(vertices[at].position);
                    rim.push_back(at);
                    const std::uint32_t next = next_border(at, back);
                    if (next == Missing)
                        break;
                    if (up(next) == upper) {
                        back = at;
                        at = next;
                        continue;
                    }
                    std::size_t k2;
                    unsigned e2;
                    if (!end_on(at, next, k2, e2))
                        break;
                    if (k2 == k0 && e2 == 0) {
                        closed = true;
                        break;
                    }
                    if (done[k2])
                        break;
                    k = k2;
                    from = e2;
                    onward = true;
                    break;
                }
                if (!onward)
                    break;
            }
            if (!closed || hole.points.size() > budget ||
                Sheet(hole.points, rim, vertices, hole.toward, anatomy))
                continue;
            budget -= hole.points.size();
            out.holes.push_back(std::move(hole));
        }
    }
}

// Triangulates a simple polygon (its points in order, counter-clockwise, in 2D) by ear clipping:
// `out` gets triples of its indices, counter-clockwise. Flat corners (collinear points) are
// clipped last, as triangles of no area (dropped by the caller). False for a self-crossing or
// degenerate polygon. At most C points (the boundary limit): O(C^2) typically, O(C^3) at worst.
bool EarClip(std::span<const std::array<double, 2>> p,
             std::vector<std::array<unsigned, 3>>& out) {
    const auto cross = [&](unsigned a, unsigned b, unsigned c) {
        return (p[b][0] - p[a][0]) * (p[c][1] - p[a][1]) - (p[b][1] - p[a][1]) * (p[c][0] - p[a][0]);
    };
    std::vector<unsigned> ring(p.size());
    for (unsigned i = 0; i < ring.size(); ++i)
        ring[i] = i;
    double area = 0;
    for (unsigned i = 0; i < ring.size(); ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        area += a[0] * b[1] - b[0] * a[1];
    }
    if (p.size() < 3 || !(area > 0))
        return false;
    const double flat = 1e-14 * std::max(area, 1e-12);
    const auto inside = [&](unsigned a, unsigned b, unsigned c, unsigned q) {
        // Strictly inside abc (a point at a corner, welded or repeated, is not).
        const auto same = [&](unsigned x) {
            return p[q][0] == p[x][0] && p[q][1] == p[x][1];
        };
        if (same(a) || same(b) || same(c))
            return false;
        return cross(a, b, q) > flat && cross(b, c, q) > flat && cross(c, a, q) > flat;
    };
    while (ring.size() > 3) {
        bool clipped = false;
        for (int pass = 0; pass < 2 && !clipped; ++pass) {
            for (unsigned i = 0; i < ring.size() && !clipped; ++i) {
                const unsigned m = static_cast<unsigned>(ring.size());
                const unsigned a = ring[(i + m - 1) % m], b = ring[i], c = ring[(i + 1) % m];
                const double turn = cross(a, b, c);
                if (pass == 0 ? turn <= flat : std::abs(turn) > flat)
                    continue; // first the convex corners, then the flat ones
                bool ear = true;
                for (unsigned q : ring) {
                    if (q != a && q != b && q != c && inside(a, b, c, q)) {
                        ear = false;
                        break;
                    }
                }
                if (!ear)
                    continue;
                out.push_back({a, b, c});
                ring.erase(ring.begin() + i);
                clipped = true;
            }
        }
        if (!clipped)
            return false; // no ear: the ring crosses itself
    }
    out.push_back({ring[0], ring[1], ring[2]});
    return true;
}
Result Run(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
           std::span<const std::uint8_t> region, const Cut& c, const Limits& requested) {
    const Limits hard;
    const Limits limit{std::min(requested.input_vertices, hard.input_vertices),
                       std::min(requested.input_triangles, hard.input_triangles),
                       std::min(requested.output_vertices, hard.output_vertices),
                       std::min(requested.output_triangles, hard.output_triangles),
                       std::min(requested.boundary_vertices, hard.boundary_vertices)};
    if (vertices.size() > limit.input_vertices || triangles.size() > limit.input_triangles)
        throw Error::Budget;
    if (vertices.empty() || triangles.empty() || region.empty() || region.size() > 2048 ||
        !Finite(c.origin) || Dot(c.origin, c.origin) > 1e8 || !Finite(c.normal) ||
        !std::isfinite(c.epsilon) || c.epsilon < 1e-7f || c.epsilon > 1e-3f ||
        !std::isfinite(c.minimum_region_weight) || c.minimum_region_weight <= 0 ||
        c.minimum_region_weight > 1 || c.body_cap_bone >= region.size() ||
        c.piece_cap_bone >= region.size() || !std::isfinite(c.skin_tilt) ||
        !std::isfinite(c.skin_tilt_max) || c.skin_tilt_max < 0 || c.skin_tilt_max > 0.5f ||
        !std::isfinite(c.bias) || std::abs(c.bias) > 0.1f)
        throw Error::InvalidInput;
    for (auto b : region)
        if (b > 1)
            throw Error::InvalidInput;
    if (region[c.body_cap_bone] || !region[c.piece_cap_bone])
        throw Error::InvalidInput;
    const Vec3 n = Unit(c.normal);
    const double eps2 = double(c.epsilon) * c.epsilon;
    std::vector<double> distances;
    distances.reserve(vertices.size());
    bool positive = false, negative = false, on_plane = false, wrong_region = false;
    std::size_t vertices_up = 0; // on the detached side (the rest: the body's)
    for (const auto& v : vertices) {
        if (!Finite(v.position) || !Finite(v.normal) || Dot(v.position, v.position) > 1e8 ||
            Dot(v.normal, v.normal) < 1e-12 || Dot(v.normal, v.normal) > 1e8 ||
            !std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]))
            throw Error::InvalidInput;
        unsigned influences = 0;
        double sum = 0, selected = 0;
        for (auto w : v.skin) {
            if (!std::isfinite(w.weight) || w.weight < 0 || w.weight > 1)
                throw Error::InvalidInput;
            if (w.weight == 0)
                continue;
            if (w.bone >= region.size())
                throw Error::InvalidInput;
            ++influences;
            sum += w.weight;
            selected += region[w.bone] * w.weight;
        }
        if (influences > 4 || std::abs(sum - 1) > 1e-4)
            throw Error::InvalidInput;
        const double plane = Dot(Sub(v.position, c.origin), n);
        // The field cut at 0: the plane's distance, or (skin mode) the skin's weight on the
        // piece past the threshold, turned a little by the plane near the blend.
        const double d =
            c.by_skin ? selected - c.minimum_region_weight + c.bias +
                            std::clamp(double(c.skin_tilt) * plane, -double(c.skin_tilt_max),
                                       double(c.skin_tilt_max))
                      : plane;
        distances.push_back(d);
        positive |= d > c.epsilon;
        negative |= d < -c.epsilon;
        on_plane |= std::abs(d) <= c.epsilon;
        wrong_region |= !c.by_skin && d > c.epsilon && selected + 1e-6 < c.minimum_region_weight;
        vertices_up += d > 0;
    }
    // Validated here once, every source triangle; and counted by side, so the outputs are sized
    // before they are filled (no growth, no copies, nothing to trim).
    // A triangle posed to no area (a pinched seam, a squeezed joint) draws nothing: left out (the
    // rest from here on is `kept`, built only once one is found), not the whole cut refused.
    std::vector<Triangle> kept;
    bool filtering = false;
    std::size_t triangles_up = 0, triangles_down = 0, crossing = 0;
    for (std::size_t k = 0; k < triangles.size(); ++k) {
        const Triangle t = triangles[k];
        for (auto i : t.index)
            if (i >= vertices.size())
                throw Error::InvalidInput;
        const Vec3 cross = Cross(Sub(vertices[t.index[1]].position, vertices[t.index[0]].position),
                                 Sub(vertices[t.index[2]].position, vertices[t.index[0]].position));
        if (Dot(cross, cross) <= eps2 * eps2) {
            if (!filtering) {
                filtering = true;
                kept.reserve(triangles.size() - 1);
                kept.assign(triangles.begin(), triangles.begin() + static_cast<std::ptrdiff_t>(k));
            }
            continue;
        }
        if (filtering)
            kept.push_back(t);
        const unsigned up = unsigned(distances[t.index[0]] > 0) +
                            unsigned(distances[t.index[1]] > 0) +
                            unsigned(distances[t.index[2]] > 0);
        triangles_up += up == 3;
        triangles_down += up == 0;
        crossing += up == 1 || up == 2;
    }
    if (filtering) {
        if (kept.empty())
            throw Error::InvalidInput; // nothing left to draw
        triangles = kept;
    }
    if (on_plane)
        throw Error::OnPlaneVertex;
    if (!positive || !negative) {
        // Wholly one side: no cut, and the side it lies on (CutEnemy sends it there whole).
        Result whole;
        whole.error = Error::NoIntersection;
        whole.whole_piece = positive;
        return whole;
    }
    if (wrong_region)
        throw Error::WrongRegion;
    // Capped, one closed contour of at most boundary_vertices nodes crosses about as many
    // triangles: more than twice that is no single section (hair, many loops). Refused before
    // any output is allocated, which also bounds the outputs' sizes below. (Cloth, uncapped,
    // has no contour to bound: its outputs are bounded by the output limits.)
    // A covering over closed skin omits its lid but still has the original capped-mesh
    // reservation (EstimateBytes). Only explicitly uncapped cloth has the larger allowance.
    if ((c.cap || c.layer == WoundLayer::Covering) && crossing > 2 * std::size_t{limit.boundary_vertices})
        throw Error::Budget;

    Result out;
    {
        // Each side: its source vertices; two new ones per crossing triangle; the caps (their
        // rings, at most one node per crossing triangle and the boundary limit, and a middle per
        // ring fanned: a ring has three points at least). Its triangles: the whole ones, at most
        // two per crossing triangle, and the caps' (a fan: one a point).
        // (Never more than the output limits: past them the cut fails with Budget anyway.)
        const std::size_t ring = std::min<std::size_t>(crossing, limit.boundary_vertices);
        const std::size_t cap = c.cap ? ring + ring / 3 + 1 : 0;
        const auto vertices_for = [&](std::size_t n) {
            return std::min<std::size_t>(n + 2 * crossing + cap, limit.output_vertices);
        };
        const auto triangles_for = [&](std::size_t n) {
            return std::min<std::size_t>(n + 2 * crossing + cap, limit.output_triangles);
        };
        out.body.vertices.reserve(vertices_for(vertices.size() - vertices_up));
        out.piece.vertices.reserve(vertices_for(vertices_up));
        out.body.triangles.reserve(triangles_for(triangles_down));
        out.piece.triangles.reserve(triangles_for(triangles_up));
    }
    std::vector<Node> nodes;
    nodes.reserve(limit.boundary_vertices);
    const auto node = [&](Vec3 p, std::uint32_t a, std::uint32_t b) {
        // Only the small cut contour is welded, not the whole mesh. This supports UV/
        // normal seams without welding surface attributes. Hard limit: 1024 nodes.
        for (unsigned i = 0; i < nodes.size(); ++i)
            if (Dot(Sub(nodes[i].position, p), Sub(nodes[i].position, p)) <= eps2)
                return i;
        if (nodes.size() >= limit.boundary_vertices)
            throw Error::Budget;
        nodes.push_back({p, {}, 0, {}, {a, b}});
        return static_cast<unsigned>(nodes.size() - 1);
    };
    const auto link = [&](unsigned a, unsigned b) {
        if (c.by_skin) {
            // Any number of rings, open chains and branches: sorted out after the walk.
            if (a == b)
                return; // a crossing folded to one point: nothing to join
            for (unsigned k = 0; k < std::min(nodes[a].degree, 4u); ++k) {
                if (nodes[a].adjacent[k] == b) {
                    nodes[a].degree = 5; // a doubled edge (a two-sided sheet): no clean ring
                    return;
                }
            }
            if (nodes[a].degree < 4)
                nodes[a].adjacent[nodes[a].degree] = b;
            ++nodes[a].degree;
            return;
        }
        if (a == b || nodes[a].degree == 2)
            throw Error::OpenOrMultipleContours;
        if (nodes[a].degree && nodes[a].adjacent[0] == b)
            throw Error::OpenOrMultipleContours;
        nodes[a].adjacent[nodes[a].degree++] = b;
    };
    const auto add_vertex = [&](Mesh& mesh, const Vertex& v) {
        if (out.body.vertices.size() + out.piece.vertices.size() >= limit.output_vertices)
            throw Error::Budget;
        mesh.vertices.push_back(v);
        return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
    };
    const auto add_triangle = [&](Mesh& mesh, std::array<std::uint32_t, 3> ids,
                                  std::uint32_t material) {
        if (out.body.triangles.size() + out.piece.triangles.size() >= limit.output_triangles)
            throw Error::Budget;
        const auto a = mesh.vertices[ids[0]].position, b = mesh.vertices[ids[1]].position,
                   d = mesh.vertices[ids[2]].position;
        const auto cross = Cross(Sub(b, a), Sub(d, a));
        if (Dot(cross, cross) <= eps2 * eps2)
            return; // no area left (the plane a hair from a corner): nothing to draw, dropped
        mesh.triangles.push_back({ids, material});
    };
    // Reuse untouched vertices, including their distinct UV/normal seams. Only newly
    // clipped edges and cap vertices need extra storage. No whole-mesh position search.
    constexpr auto missing = 0xffffffffu;
    std::array<std::vector<std::uint32_t>, 2> source_index;
    for (auto& indices : source_index)
        indices.assign(vertices.size(), missing);
    for (auto tri : triangles) {
        // The common case (all but the few round the cut): whole on one side. Its corners are
        // mapped and it is appended as it is; its area was checked with the source's.
        const bool up0 = distances[tri.index[0]] > 0, up1 = distances[tri.index[1]] > 0,
                   up2 = distances[tri.index[2]] > 0;
        if (up0 == up1 && up1 == up2) {
            Mesh& mesh = up0 ? out.piece : out.body;
            auto& mapped = source_index[up0];
            std::array<std::uint32_t, 3> ids;
            for (unsigned i = 0; i < 3; ++i) {
                auto& m = mapped[tri.index[i]];
                if (m == missing)
                    m = add_vertex(mesh, vertices[tri.index[i]]);
                ids[i] = m;
            }
            if (out.body.triangles.size() + out.piece.triangles.size() >= limit.output_triangles)
                throw Error::Budget;
            mesh.triangles.push_back({ids, tri.material});
            continue;
        }
        std::array<Vertex, 3> edge{};
        std::array<bool, 3> crossed{};
        std::array<unsigned, 2> ends{};
        unsigned count = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned j = (i + 1) % 3;
            const auto a = tri.index[i], b = tri.index[j];
            if ((distances[a] > 0) == (distances[b] > 0))
                continue;
            crossed[i] = true;
            // Evaluate an undirected source edge in one order, so adjacent triangles
            // compute exactly the same intersection (even when traversed backwards).
            const auto lo = std::min(a, b), hi = std::max(a, b);
            const float t = static_cast<float>(distances[lo] / (distances[lo] - distances[hi]));
            edge[i] = Mix(vertices[lo], vertices[hi], t);
            if (!c.by_skin) {
                // Exactly on the plane (skin mode: where the field crosses, on the surface).
                edge[i].position =
                    Sub(edge[i].position,
                        Mul(n, static_cast<float>(Dot(Sub(edge[i].position, c.origin), n))));
            }
            if (count >= ends.size())
                throw Error::InvalidInput;
            if (!c.cap) {
                ends[count++] = 0; // (cloth: no contour)
                continue;
            }
            const unsigned at = node(edge[i].position, a, b);
            // A duplicated UV seam can reach the same point with different rounding. Both
            // skin halves must use the cap's welded position; keep their own UVs and normals.
            edge[i].position = nodes[at].position;
            if (c.by_skin) {
                // Which way the piece lies across this ring: from the edge's body end to its
                // piece end (each ring's cap faces away from its own side by it).
                const bool a_up = distances[a] > 0;
                nodes[at].toward =
                    Add(nodes[at].toward, Sub(vertices[a_up ? a : b].position,
                                              vertices[a_up ? b : a].position));
            }
            ends[count++] = at;
        }
        if (count) {
            if (count != 2)
                throw Error::InvalidInput;
            if (c.cap) {
                link(ends[0], ends[1]);
                link(ends[1], ends[0]);
            }
        }
        for (bool upper : {false, true}) {
            Mesh& mesh = upper ? out.piece : out.body;
            std::array<std::uint32_t, 4> polygon{};
            unsigned size = 0;
            for (unsigned i = 0; i < 3; ++i) {
                const auto index = tri.index[i];
                if ((distances[index] > 0) == upper) {
                    auto& mapped = source_index[upper][index];
                    if (mapped == missing)
                        mapped = add_vertex(mesh, vertices[index]);
                    polygon[size++] = mapped;
                }
                if (crossed[i])
                    polygon[size++] = add_vertex(mesh, edge[i]);
            }
            for (unsigned i = 1; i + 1 < size; ++i)
                add_triangle(mesh, {polygon[0], polygon[i], polygon[i + 1]}, tri.material);
        }
    }
    if (!c.cap) {
        // Cloth: split, nothing to close.
        out.boundary_vertices = 0;
        out.error = Error::None;
        return out;
    }
    if (c.by_skin) {
        // Real meshes (c2000's: nineteen small rings in its head, three and two open chains in
        // its body): every clean closed ring is capped on both sides, in its own plane; an open
        // chain or a branch is left open (it ends on the mesh's border, where another mesh goes
        // on). A ring that cannot be capped (it crosses itself) fails the mesh.
        std::vector<int> seen(nodes.size(), -1);
        std::vector<unsigned> stack, members, loop;
        std::vector<std::array<unsigned, 2>> chain_ends; // out.chains' end nodes
        std::vector<Vec3> ring_points;
        for (unsigned start = 0; start < nodes.size(); ++start) {
            if (seen[start] >= 0)
                continue;
            members.clear();
            stack.assign(1, start);
            seen[start] = static_cast<int>(start);
            bool ring = true;
            while (!stack.empty()) {
                const unsigned at = stack.back();
                stack.pop_back();
                members.push_back(at);
                ring = ring && nodes[at].degree == 2;
                for (unsigned k = 0; k < std::min(nodes[at].degree, 4u); ++k) {
                    const unsigned to = nodes[at].adjacent[k];
                    if (seen[to] < 0) {
                        seen[to] = static_cast<int>(start);
                        stack.push_back(to);
                    }
                }
            }
            if (!ring || members.size() < 3) {
                ++out.open_chains;
                // A simple chain (two ends, no branch) is kept, in order, for CutEnemy to join
                // with the next mesh's across a seam.
                unsigned ends = 0, end = start;
                bool branch = false;
                for (unsigned m : members) {
                    ends += nodes[m].degree == 1;
                    branch = branch || nodes[m].degree > 2;
                    if (nodes[m].degree == 1)
                        end = m;
                }
                if (branch || ends != 2)
                    continue;
                OpenChain chain;
                unsigned at = end, from = static_cast<unsigned>(nodes.size());
                for (std::size_t steps = 0; steps < members.size(); ++steps) {
                    chain.points.push_back(nodes[at].position);
                    chain.toward = Add(chain.toward, nodes[at].toward);
                    unsigned next = at;
                    for (unsigned k = 0; k < nodes[at].degree; ++k) {
                        if (nodes[at].adjacent[k] != from) {
                            next = nodes[at].adjacent[k];
                            break;
                        }
                    }
                    if (next == at)
                        break; // the far end
                    from = at;
                    at = next;
                }
                if (chain.points.size() == members.size()) {
                    out.chains.push_back(std::move(chain));
                    chain_ends.push_back({end, at});
                }
                continue;
            }
            loop.clear();
            unsigned at = start, from = static_cast<unsigned>(nodes.size());
            do {
                loop.push_back(at);
                const unsigned next = nodes[at].adjacent[nodes[at].adjacent[0] == from ? 1 : 0];
                from = at;
                at = next;
            } while (at != start && loop.size() <= members.size());
            if (at != start || loop.size() != members.size()) {
                ++out.open_chains;
                continue;
            }
            ring_points.clear();
            Vec3 toward{};
            for (unsigned i : loop) {
                ring_points.push_back(nodes[i].position);
                toward = Add(toward, nodes[i].toward);
            }
            if (!CapRing(ring_points, toward, c, out.body, out.piece))
                throw Error::UnsupportedContour;
            if (out.body.vertices.size() + out.piece.vertices.size() > limit.output_vertices ||
                out.body.triangles.size() + out.piece.triangles.size() > limit.output_triangles)
                throw Error::Budget;
            ++out.loops;
            out.boundary_vertices += static_cast<std::uint32_t>(loop.size());
        }
        if (!out.chains.empty())
            FindHoles(vertices, triangles, distances, nodes, chain_ends,
                      limit.boundary_vertices, c.layer == WoundLayer::Anatomy, out);
        out.error = Error::None;
        return out;
    }
    if (nodes.size() < 3)
        throw Error::OpenOrMultipleContours;
    for (const auto& p : nodes)
        if (p.degree != 2)
            throw Error::OpenOrMultipleContours;
    std::vector<unsigned> loop;
    loop.reserve(nodes.size());
    std::vector<bool> visited(nodes.size());
    unsigned current = 0, previous = static_cast<unsigned>(nodes.size());
    do {
        if (visited[current])
            throw Error::OpenOrMultipleContours;
        visited[current] = true;
        loop.push_back(current);
        const unsigned next =
            nodes[current].adjacent[nodes[current].adjacent[0] == previous ? 1 : 0];
        previous = current;
        current = next;
    } while (current != 0);
    if (loop.size() != nodes.size())
        throw Error::OpenOrMultipleContours;
    Vec3 center{};
    for (auto i : loop)
        center = Add(center, Mul(nodes[i].position, 1.f / loop.size()));
    double area = 0;
    for (unsigned i = 0; i < loop.size(); ++i)
        area += Dot(Cross(Sub(nodes[loop[i]].position, center),
                          Sub(nodes[loop[(i + 1) % loop.size()]].position, center)),
                    n);
    if (std::abs(area) <= eps2)
        throw Error::UnsupportedContour;
    if (area < 0)
        std::reverse(loop.begin(), loop.end());
    if (c.by_skin) {
        // A skin ring follows the artist's weights, so it is rarely exactly convex. The cap is a
        // fan from its middle: it only needs every fan triangle to face the same way round the
        // middle (the ring star-shaped from it). A self-crossing or folded ring still fails.
        for (unsigned i = 0; i < loop.size(); ++i) {
            const auto a = nodes[loop[i]].position;
            const auto b = nodes[loop[(i + 1) % loop.size()]].position;
            if (Dot(Cross(Sub(a, center), Sub(b, center)), n) <= eps2)
                throw Error::UnsupportedContour;
        }
    } else {
        // Every vertex must lie inside every oriented boundary edge. This rejects concave,
        // self-crossing and folded contours rather than filling them with an incorrect hull.
        // Bounded O(C^2), C<=1024; no quadratic scan over source triangles.
        for (unsigned i = 0; i < loop.size(); ++i) {
            const auto a = nodes[loop[i]].position,
                       b = nodes[loop[(i + 1) % loop.size()]].position;
            for (auto j : loop)
                if (Dot(Cross(Sub(b, a), Sub(nodes[j].position, a)), n) < -eps2 * 4)
                    throw Error::UnsupportedContour;
        }
    }
    const Vec3 u = Unit(Cross(n, std::abs(n.x) < .9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0})),
               v = Cross(n, u);
    for (bool upper : {false, true}) {
        Mesh& mesh = upper ? out.piece : out.body;
        Vertex cap;
        cap.position = center;
        cap.normal = Mul(n, upper ? -1.f : 1.f);
        cap.skin[0] = {upper ? c.piece_cap_bone : c.body_cap_bone, 1};
        const auto middle = add_vertex(mesh, cap);
        std::vector<std::uint32_t> ring;
        ring.reserve(loop.size());
        for (auto i : loop) {
            cap.position = nodes[i].position;
            const auto delta = Sub(cap.position, center);
            cap.uv = {static_cast<float>(Dot(delta, u)), static_cast<float>(Dot(delta, v))};
            ring.push_back(add_vertex(mesh, cap));
        }
        for (unsigned i = 0; i < ring.size(); ++i) {
            const auto a = ring[i], b = ring[(i + 1) % ring.size()];
            add_triangle(mesh,
                         upper ? std::array<std::uint32_t, 3>{middle, b, a}
                               : std::array<std::uint32_t, 3>{middle, a, b},
                         c.cap_material);
        }
    }
    out.boundary_vertices = static_cast<std::uint32_t>(loop.size());
    out.error = Error::None;
    return out;
}
// A cap's triangles over its ring (taken counter-clockwise about l). The fan from its middle
// when every fan triangle turns the same way (star-shaped from it: the usual neck or limb), O(n)
// and exact. Otherwise ear clipping in its plane, exact for a concave ring. Otherwise (it crosses
// itself there: a twisted blend) the fan anyway: flesh over flesh, never a gap. True for a fan
// (its middle a vertex).
bool Triangulate(const CapFrame& f, std::span<const Vec3> ring,
                 std::vector<std::array<unsigned, 3>>& out) {
    const std::size_t n = ring.size();
    std::vector<std::array<double, 2>> flat(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 d = Sub(ring[f.reversed ? n - 1 - i : i], f.middle);
        flat[i] = {Dot(d, f.u), Dot(d, f.v)};
    }
    bool star = true;
    for (std::size_t i = 0; i < n && star; ++i) {
        const auto& a = flat[i];
        const auto& b = flat[(i + 1) % n];
        star = a[0] * b[1] - a[1] * b[0] > 0;
    }
    out.clear();
    out.reserve(n);
    if (!star && EarClip(flat, out))
        return false;
    FanOf(flat, out);
    return true;
}
} // namespace
bool CapRing(std::span<const Vec3> ring, Vec3 toward, const Cut& c, Mesh& body, Mesh& piece) {
    CapFrame f;
    if (!Frame(ring, toward, c, f))
        return false;
    std::vector<std::array<unsigned, 3>> triangles;
    const bool fan = Triangulate(f, ring, triangles);
    Emit(f, ring, fan, triangles, c, false, body);
    Emit(f, ring, fan, triangles, c, true, piece);
    return true;
}
bool Fill(std::span<const Vec3> ring, Vec3 toward, const Cut& c, bool piece, Mesh& mesh) {
    CapFrame f;
    if (!Frame(ring, toward, c, f))
        return false;
    std::vector<std::array<unsigned, 3>> triangles;
    const bool fan = Triangulate(f, ring, triangles);
    Emit(f, ring, fan, triangles, c, piece, mesh);
    return true;
}
Result Split(std::span<const Vertex> vertices, std::span<const Triangle> triangles,
             std::span<const std::uint8_t> detached_bones, const Cut& cut, const Limits& limits) {
    try {
        return Run(vertices, triangles, detached_bones, cut, limits);
    } catch (Error error) {
        return {error, {}, {}, 0};
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailed, {}, {}, 0};
    }
}
} // namespace VR::Sever
