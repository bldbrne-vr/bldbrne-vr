// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Bloodborne VR fork: when an entry of the on-disk pipeline cache (user/cache/<serial>/: *.key
// pipeline keys, *.meta shader metadata, *.spv shader binaries) is stale. Every key and meta starts
// with a version word and a meta also carries the version of its binary; an entry whose word is not
// this build's is never used: its pipeline is compiled again during play and the entry rewritten.
//
// The word: upstream's own number in bits 0..3, the fork's SCOPED revision in bits 4..7, the fork's
// revision in bits 8..15 and the tag 'VR' (0x5652) above. Each field only ever grows, so two
// different (upstream, scoped, revision) triples never give the same word.
//
// Which number to bump (a stale entry must never be used, and only stale entries should go):
//  - the LAYOUT of what is serialized changes (GraphicsPipelineKey, ComputePipelineKey,
//    GraphicsPipeline::SerializationSupport): VrKeyLayoutRevision; (Shader::Info, the
//    StageSpecialization, RuntimeInfo, FetchShaderData, the SRT info): VrMetaLayoutRevision;
//  - the generated SPIR-V changes for EVERY stage (e.g. the push-constant block, a shared helper):
//    VrRevision (all shader binaries);
//  - only the hardware vertex stage's epilogue changes (ApplyStereoShift: the stereo shift, the HUD
//    scale, the TAA jitter; the geometry stage runs it before every EmitVertex and has the same
//    scope since the asymmetric geometry-stage remap): VrVertexRevision - the fragment and compute
//    shaders and the other stages stay cached;
//  - only the game's own fragment programs change (FragmentRuntimeInfo::vr_cut == 0):
//    VrFragmentRevision - the other stages stay cached; a change to every fragment permutation
//    bumps VrCutRevision too;
//  - only the body-cutaway permutations change (FragmentRuntimeInfo::vr_cut != 0, the VR-owned
//    variants of the hunter's own fragment programs): VrCutRevision - the game's own fragment
//    programs stay cached;
//  - the object-motion permutations (RuntimeInfo::vr_motion, cached since VrRevision 8): the
//    vertex side (EmitVertexMotion) is the hardware vertex stage's epilogue, VrVertexRevision; the
//    fragment side (EmitFragmentMotion, with or without a cut) VrFragmentRevision + VrCutRevision.
// A pipeline is preloaded only when its key and every stage's meta and binary are current; when a
// scoped bump makes one stage stale, the other stages' programs are still preloaded (the pipeline
// itself is created again on first use, without recompiling those programs).
//
// History: VrRevision 2 = FragmentRuntimeInfo::vr_cut (a layout change too), 3 = the aa_mode=taa
// jitter in ApplyStereoShift. Before the scoped revisions every word carried VrRevision; the
// asymmetric projection changes RuntimeInfo layout (meta 4) and the vertex epilogue (vertex 1);
// vertex 2 = asymmetric light rectangles, vertex 3 = eye-facing sprites in native stereo,
// vertex 4 = the light-glow quads per eye, vertex 5 = geometry-stage sprites face the eye;
// meta 10 = Info::vr_screen_rect (compute pixel-rectangle passes per eye); VrRevision 7 + meta 11
// = Shader::VrPushData: the projection, the light tiles and the cut radii are pushed per draw
// (every native graphics stage declares the longer push block; RuntimeInfo keeps their flags,
// RuntimeInfo::native_asymmetric is new and FragmentRuntimeInfo::vr_cut only 1 then), so the
// per-camera, per-tile and per-radius permutations of the caches before it are never used again;
// VrRevision 8 + meta 12 = the object-motion buffers' addresses are pushed too (the push block
// gains them; a motion permutation keeps only RuntimeInfo::vr_motion's flag and is now cached on
// disk with its pipeline, while one with baked addresses, without VrPushData, still never is);
// fragment 1 + cut 1 = FragmentRuntimeInfo::color_buffers keeps only the targets the program
// writes, in the form the recompiler reads them (KeepWrittenColorBuffers: no data format, the
// number format's class, the swizzle as CompMapping::Map sees it; flat and native stereo alike):
// the SPIR-V is the same, but the metas before it hold every bound target as its registers say
// and would never match a selection again (the fragment programs were in the Common scope until
// then). A pipeline key with a fragment program clears the bound targets past its last written
// one too: every such key is stale with its fragment metas, the keys' layout did not change.
// Key 4 + meta 13 = permutation ids (FAST_CPU_PERM_IDS=1; opt-in, Naming::Ids): a program's
// permutation is named HashCombine(program, its index in the session's list) (its meta "<that>",
// its binary "<program>_<index>", the pipeline keys' stage hashes the same), and a later session
// could give that index to another specialization and overwrite the files; with the ids it is
// named by its id, the hash of its serialized StageSpecialization seeded with the program's hash
// (PermutationId), for the meta, the binary and the keys' stage hashes alike. The meta's header
// holds the id, the program's hash and the index (only where the warm-up puts it in the program's
// list, as before). The keys' and metas' layouts did not change, their names' meaning did, so the
// two namings have their own words: the indexed naming (the default, Naming::Indexed) keeps key 3
// + meta 12 and reads and writes the caches before it unchanged; the ids' first session compiles
// everything once. A key of the other naming is kept for its sessions (Reject::OtherNaming), and
// both namings' metas and binaries live side by side (their names differ), so switching back and
// forth recompiles nothing twice. FAST_CPU_CACHE_GC=1 (with the ids only) deletes the metas and
// binaries no permutation of the session is named by (CollectUnnamed: the indexed naming's
// included).
namespace Vulkan::PipelineCachePolicy {

using Word = std::uint32_t;

constexpr Word Tag = 0x5652u;
constexpr Word UpstreamShaderVersion = 3u; // upstream's ShaderBinary / ShaderMeta number
constexpr Word UpstreamKeyVersion = 4u;    // upstream's PipelineKey number

constexpr Word VrKeyLayoutRevision = 4u;  // the stage hashes are permutation ids
constexpr Word VrMetaLayoutRevision = 13u; // named by the permutation id (the header holds it)
// FAST_CPU_PERM_IDS off (the default): the indexed naming's words, the caches' before the ids.
constexpr Word VrKeyLayoutRevisionIndexed = 3u;
constexpr Word VrMetaLayoutRevisionIndexed = 12u; // cached motion permutations (vr_motion's flag)
constexpr Word VrRevision = 8u;           // VrPushData: the motion buffers' addresses pushed too
constexpr Word VrVertexRevision = 5u;     // geometry-stage sprites face the eye (RewriteGeometrySprites)
constexpr Word VrFragmentRevision = 1u;   // only what the SPIR-V reads of the written colour targets
constexpr Word VrCutRevision = 1u;        // the vr_cut permutations: the same colour-target change

// How a permutation is named on disk (the keys' stage hashes, the metas' and binaries' names):
// HashCombine(program, its index), or its id (FAST_CPU_PERM_IDS=1).
enum class Naming : Word { Indexed, Ids };

// What a shader's binary depends on beyond the code every stage shares.
enum class Scope : Word {
    Common,         // compute, hull, export, local
    VertexEpilogue, // the hardware vertex and geometry stages (ApplyStereoShift)
    FragmentCut,    // a fragment permutation with the body cutaway (vr_cut != 0)
    Fragment,       // the game's own fragment programs (vr_cut == 0)
};

constexpr Word Encode(Word upstream, Word scoped, Word revision) {
    return (upstream & 0xFu) | ((scoped & 0xFu) << 4) | ((revision & 0xFFu) << 8) | (Tag << 16);
}

// `hw_vertex`: the program runs as the hardware vertex stage (RuntimeInfo::stage ==
// Shader::Stage::Vertex, also when it is the logical tessellation-evaluation stage) or as the
// geometry stage (ShaderBinaryVersionOf);
// `fragment` + `vr_cut`: a fragment program and its FragmentRuntimeInfo::vr_cut.
constexpr Scope ScopeOf(bool hw_vertex, bool fragment, std::uint32_t vr_cut) {
    if (hw_vertex) {
        return Scope::VertexEpilogue;
    }
    if (fragment) {
        return vr_cut != 0 ? Scope::FragmentCut : Scope::Fragment;
    }
    return Scope::Common;
}

constexpr Word ScopedRevision(Scope scope) {
    switch (scope) {
    case Scope::VertexEpilogue:
        return VrVertexRevision;
    case Scope::FragmentCut:
        return VrCutRevision;
    case Scope::Fragment:
        return VrFragmentRevision;
    case Scope::Common:
    default:
        return 0u;
    }
}

constexpr Word PipelineKeyVersion(Naming naming) {
    return Encode(UpstreamKeyVersion, 0u,
                  naming == Naming::Ids ? VrKeyLayoutRevision : VrKeyLayoutRevisionIndexed);
}

constexpr Word ShaderMetaVersion(Naming naming) {
    return Encode(UpstreamShaderVersion, 0u,
                  naming == Naming::Ids ? VrMetaLayoutRevision : VrMetaLayoutRevisionIndexed);
}

constexpr Naming OtherNaming(Naming naming) {
    return naming == Naming::Ids ? Naming::Indexed : Naming::Ids;
}

constexpr Word ShaderBinaryVersion(Scope scope) {
    return Encode(UpstreamShaderVersion, ScopedRevision(scope), VrRevision);
}

// A loaded *.spv that cannot be a SPIR-V module (empty, truncated, another file's bytes) is never
// handed to the driver: its pipeline counts as stale and is compiled again.
constexpr std::uint32_t SpirvMagic = 0x07230203u;
constexpr bool LooksLikeSpirv(std::span<const std::uint32_t> words) {
    // Header: magic, version, generator, id bound (> 0), schema (0); then at least one instruction.
    return words.size() > 5 && words[0] == SpirvMagic && words[3] != 0 && words[4] == 0;
}

// Why a cached pipeline was not preloaded (the warm-up's summary line counts each).
enum class Reject : Word {
    None,
    KeyVersion,   // the key's word is another build's
    StageMissing, // a stage's meta or binary is not in the cache
    StageStale,   // a stage's meta or binary word is another build's (or a scoped bump)
    Corrupt,      // truncated / unreadable bytes, or a binary that is not SPIR-V
    Conflict,     // a permutation cached at two indices or under two ids, or two permutations
                  // at one index or under one id (entries of two cache generations)
    Duplicate,    // the same key twice
    OtherMode,    // the other mode's (flat vs native stereo): kept for it, never used in this one
    MotionIdle,   // an object-motion variant, the session started without an upscaler: kept
    OtherNaming,  // the other permutation naming's key (FAST_CPU_PERM_IDS): kept, never read here
    Count,
};

constexpr const char* RejectName(Reject r) {
    switch (r) {
    case Reject::None:
        return "loaded";
    case Reject::KeyVersion:
        return "another build's key";
    case Reject::StageMissing:
        return "a stage missing";
    case Reject::StageStale:
        return "a stage stale";
    case Reject::Corrupt:
        return "corrupt";
    case Reject::Conflict:
        return "permutation conflict";
    case Reject::Duplicate:
        return "duplicate";
    case Reject::OtherMode:
        return "the other mode's";
    case Reject::MotionIdle:
        return "object motion, no upscaler";
    case Reject::OtherNaming:
        return "the other permutation naming's";
    default:
        return "?";
    }
}

// A rejected key file is deleted at the warm-up (it can never load; its pipeline writes a fresh one
// when it compiles again), so the next session neither reads it nor reports it again. Not in the
// archived (zip) mode, which cannot delete single entries. The other mode's keys load in its
// sessions, the object-motion variants' in the sessions started with an upscaler, the other
// permutation naming's in its sessions.
constexpr bool PruneRejected(Reject r, bool archived) {
    return !archived && r != Reject::None && r != Reject::OtherMode && r != Reject::MotionIdle &&
           r != Reject::OtherNaming;
}

// A key the warm-up keeps on disk names permutations that stay: the loaded ones, the other mode's
// and the object-motion variants' (their stages are loaded too). The other naming's keys stay
// too, but are never read (their stage hashes are not ids of this naming).
constexpr bool KeptKey(Reject r) {
    return r == Reject::None || r == Reject::OtherMode || r == Reject::MotionIdle;
}

// ---- permutation ids (a pipeline key's stage hash, its meta's and its binary's name)

using Id = std::uint64_t;

// 0 is a pipeline key's "no program at this stage": never an id.
constexpr Id NonZeroId(Id hash) {
    return hash != 0 ? hash : Id{1};
}

// The next candidate when another permutation of the same program already has `id` (a 64-bit
// collision; GetProgram): deterministic, never 0. Such a permutation is never written to disk (its
// name would not be its content's), so no later session can mistake it for another.
constexpr Id NextId(Id id) {
    Id z = id + 0x9E3779B97F4A7C15ull; // splitmix64
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return NonZeroId(z ^ (z >> 31));
}

// The id a meta's or binary's file name holds: the name is "0x" and 16 hex digits (the id as
// "{:#018x}" writes it); anything else (an earlier binary's "<program>_<index>", another file) has
// none.
constexpr bool ParseId(std::string_view name, Id& id) {
    if (name.size() != 18 || name[0] != '0' || (name[1] != 'x' && name[1] != 'X')) {
        return false;
    }
    Id value = 0;
    for (std::size_t i = 2; i < name.size(); ++i) {
        const char c = name[i];
        Id digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<Id>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<Id>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<Id>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    if (value == 0) {
        return false;
    }
    id = value;
    return true;
}

// The warm-up's collection: a meta or binary (`name`) is deleted unless it is named by the id of a
// permutation the session holds or a kept key names (`named`). The keys are pruned first: a meta
// only a pruned key named is not loaded, so it goes too, while a loaded one stays for this session
// (a pipeline compiled again from it writes a key that names it). Not in the archived (zip) mode,
// which cannot delete single entries.
template <typename IsNamed>
constexpr bool CollectUnnamed(std::string_view name, bool archived, IsNamed&& named) {
    if (archived) {
        return false;
    }
    Id id = 0;
    return !ParseId(name, id) || !named(id);
}

} // namespace Vulkan::PipelineCachePolicy
