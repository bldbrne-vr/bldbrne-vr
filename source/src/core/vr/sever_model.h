// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/vr/sever_mesh.h"

// Beast Claw dismemberment, an enemy's own model read from its file on disk (the snapshot plan,
// the worker, never the game thread, reads the character's
// dvdroot_ps4/chr/cXXXX.chrbnd.dcx (DCX: zlib; BND4: its first FLVER2) into its skeleton and its
// meshes' LOD0 surfaces in the bind pose. It is the same asset the game draws; the game thread
// only captures the bone matrices at the kill. Every read is bounds-checked: a damaged or unknown
// file is an error, never a crash. Formats as SoulsFormats documents them, checked against the
// game's c2000 (2026-11-12: its weights sum to one, its UVs fall in 0..1 at 1/2048, its normals
// are unit length).
namespace VR::Sever {

struct ModelBone {
    std::string name;
    std::int32_t parent{-1};
    // The bind pose's world transform, row vectors (v' = v * M, M[3] the translation), from
    // the bone's local scale, X Z Y rotation and translation, as the game composes them.
    std::array<float, 16> bind{};
    Vec3 joint{}; // its origin in model space (bind[12..14])
};

struct ModelMesh {
    std::string group;    // its group bone's name ("c2000_Head", "mantle")
    std::string material; // its material's name ("hood_cloth")
    std::string diffuse;  // its material's diffuse texture: the TPF's name (lower case), or ""
    // No positions in the file (the game's cloth simulation moves them: a hood, a mantle's
    // cloth): it cannot be cut here.
    bool simulated{false};
    std::vector<Vertex> vertices;    // the bind pose, model space, metres; weights sum to one
    std::vector<Triangle> triangles; // LOD0, its material index on each
};

struct Model {
    std::uint32_t version{0};
    std::vector<ModelBone> bones;
    std::vector<ModelMesh> meshes;
    std::vector<std::uint8_t> textures; // the bnd's TPF as read (PS4: BC, tiled), or empty
};

enum class ModelError : std::uint8_t {
    None,
    Read,     // the file could not be read
    NotDcx,   // not a DCX (DFLT) container
    Inflate,  // its zlib stream is damaged
    NoFlver,  // no FLVER2 model inside
    Layout,   // a header, table or offset outside the data, or a vertex layout not understood
    TooBig,   // over the reader's ceilings (bones, meshes, vertices, triangles, bytes)
};

const char* ModelErrorName(ModelError e);

// A mesh's display group: its material name's "#NN#" (0..31). The game draws such a mesh only
// while its character's NpcParam modelDispMask bit NN is set (one model, several looks: c1240's
// plain, white and "Gen" fur, a hunter's hats and weapons); -1: none, always drawn.
inline int DispGroup(std::string_view material) {
    const std::size_t at = material.find('#');
    if (at == std::string_view::npos) {
        return -1;
    }
    int value = 0, digits = 0;
    std::size_t i = at + 1;
    for (; i < material.size() && material[i] >= '0' && material[i] <= '9' && digits < 3; ++i) {
        value = value * 10 + (material[i] - '0');
        ++digits;
    }
    if (digits == 0 || digits > 2 || i >= material.size() || material[i] != '#' || value > 31) {
        return -1;
    }
    return value;
}

// The game draws this mesh with this display mask (none known: every mesh, as before).
inline bool Drawn(const ModelMesh& mesh, std::optional<std::uint32_t> mask) {
    const int group = DispGroup(mesh.material);
    return group < 0 || !mask || ((*mask >> group) & 1u) != 0;
}

// NpcParam's modelDispMask0..31 by row id, from a gameparam.parambnd.dcx (DCX, a BND4 of PARAMs):
// bytes 326-327 (masks 0-15) and 330-331 (16-31) of its rows (Paramdex BB NPC_PARAM_ST, 388
// bytes). Layout when the archive, its NpcParam or its rows are not as expected.
ModelError LoadNpcDispMasks(std::span<const std::uint8_t> file,
                            std::unordered_map<std::int32_t, std::uint32_t>& out);

// A DCX (DFLT) file's contents, inflated (at most 256 MiB): e.g. a character's own texture file,
// c<id>_2.tpf.dcx, a PS4 TPF kept beside its chrbnd (24 of the game's characters keep theirs so).
ModelError InflateDcx(std::span<const std::uint8_t> file, std::vector<std::uint8_t>& out);

// Reads a chrbnd.dcx (its bytes, or its file). Ceilings: 256 MiB inflated, 2048 bones, 64 meshes,
// 65,536 vertices and 262,144 triangles a mesh.
ModelError LoadModel(std::span<const std::uint8_t> file, Model& out);
ModelError ReadModel(const std::filesystem::path& path, Model& out);

} // namespace VR::Sever
