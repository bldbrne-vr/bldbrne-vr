// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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
// The word: upstream's own number in bits 0..3, the fork's SCOPED revision in bits 4..19, the
// fork's revision in bits 20..27 and the tag 0xA above. Each field only ever grows and every number
// is checked to fit its field (a compile error, never a wrap), so two different (upstream, scoped,
// revision) triples never give the same word. (Until 2026-11-16 the scoped field had 4 bits and the
// tag 'VR' (0x5652) bits 16..31: fragment revision 162 wrote 146's word, and a cache 16 bumps old
// could be read as current (a code review). Those words' top four bits are 5, never 0xA: every
// entry they wrote is stale once, its pipeline compiled again on first use.)
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
//  - the fragment programs change: VrFragmentRevision - the other stages stay cached. The
//    body-cutaway permutations (FragmentRuntimeInfo::vr_cut != 0) are fragment programs too: their
//    word carries VrFragmentRevision with VrCutRevision, so they follow every fragment change (until
//    2026-11-16 a change to every fragment permutation had to bump VrCutRevision by hand, and 160's
//    and 162's did not: a code review);
//  - only the body-cutaway permutations change (the VR-owned variants of the hunter's own fragment
//    programs): VrCutRevision - the game's own fragment programs stay cached;
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

constexpr Word Tag = 0xAu; // (the words before 2026-11-16: 0x5652 in bits 16..31)
constexpr Word UpstreamBits = 4u, ScopedBits = 16u, RevisionBits = 8u;
constexpr bool Fits(Word value, Word bits) {
    return value < (Word{1} << bits);
}
constexpr Word UpstreamShaderVersion = 3u; // upstream's ShaderBinary / ShaderMeta number
constexpr Word UpstreamKeyVersion = 4u;    // upstream's PipelineKey number

constexpr Word VrKeyLayoutRevision = 10u;  // the stage hashes are permutation ids
constexpr Word VrMetaLayoutRevision = 23u; // a permutation's own draw-time VR fields too (vr_bump_image: flat parallax) (22: its own resources; 21: vr_bump_image)
// FAST_CPU_PERM_IDS off (the default): the indexed naming's words, the caches' before the ids.
constexpr Word VrKeyLayoutRevisionIndexed = 9u;
constexpr Word VrMetaLayoutRevisionIndexed = 22u; // a permutation's own draw-time VR fields too (21: its own resources; 20: vr_bump_image)
constexpr Word VrRevision = 9u;           // NonUniform on final per-eye image/atomic operands
constexpr Word VrVertexRevision = 8u;     // object motion stores its position with one plain store
constexpr Word VrFragmentRevision = 167u;  // the AO pass's lamp mask writer keyed by its tier (RuntimeInfo::VrAoLampLow / High; the lamp shadows switch live); 166: lamp shadow presets: Low sharp edges to 2 m, gone by 3, fewer march steps and no torch boost; Medium three lamps sharp to 4 m, gone by 5; High sharp to 5 m, gone by 6 (light_shadow.h EdgeTierInM, EdgeTierOutM, LowSteps); 165: Medium lamp shadows' sharp edges reach 4.5 m, was 4 (light_shadow.h EdgeTierOutM: the lamp programs hold the reach); 164: the lamp shadows' mask on by default (Low, its march noise 2) and High's hero lamps marching in the lamp programs (light_shadow.h High), which declare his body maps and the grid at both; 163: the fixture clearance 46 cm at a reach of 5 m, was 45 (light_shadow.h ClearFireM); 162: the bounce's 2x2 parity from the unscaled FragCoord; fragment shaders that can discard keep exact math; 161: the bounce's 2x2 split whenever it is on, each pixel's own normal, soft neighbour weights (bounce_light.h QuadGather); 160: the game's fragment math may fuse multiply-add where no depth is written (emit_spirv_floating_point.cpp); 159: a spawned grey or blue light's shadows in full from 0.5 (the cane men's lanterns); 158: the lamp glow's normal and inverse only where a light reaches, the scene copy's setup inside its branch, the block blend's ramp (157) (the lamp read blends between the mask's blocks (their size in its last word, the facing 6 bits a component) (156) (the fog grid branch joined with the flame vote within 0.18 m (155; 154 was the flame vote alone) (the fog grid holds the moonlit air too: no cells' march, no tent in the scene copy, with VR_FOG_GRID=1 only (153; without the switch every program is still byte for byte 151's) (the air pass's programs keyed for the fog grid, VR_FOG_GRID=1 (152; without the switch every program is byte for byte 151's) (the moon glow a bit more than a quarter outside the woods (151; 150 was a test) (the glow's knee, the moon glow's far keep 0.75 (149) (the lamps' plane test by the angle (148) (the mist brought to each pixel's distance, the haze in the apply (147) (the haze's own brightness, the ambient level (146) (the distance haze (145) (the moon glow LanternEngine's way: far surfaces keep 0.8, no dimming (144) (the mist high haze, coarse blocks take the mean jitter (143) (the glow's loop ends after the lights with a glow, votes in blocks (142) (thick mist, small lights' glow, the moon's rays behind VR_AIR_RAYS (141) (the air pass in the reflections pass (its half-size host), the mist, the moon aim (140) (the air pass: moon beams in half-size cells, the halos and their source votes in the scene copy, the colour probe, lit smoke by core (139) (foveated march steps (138) (near pixels march more steps; the GPU step dial (137) (raised bricks on the map's G-buffer programs only (136) (raised bricks: no parallax where a branch some pixels skip holds its reads (135) (close lamps (the held torch) march more steps (134) (the grid's block stamps off by default (133) (the grid's block stamps (VR_MARCH_OPT 128) (132) (the depth pyramid in the mask march (131) (the mask pass's exact speed-ups (VR_MARCH_OPT) (130) (the world-tied noise off again (129) (the mask march with the world-tied noise by default (128) (tests: the mask march smart steps (VR_SCENE_MARCH_SMART), world-tied noise (VR_SCENE_MARCH_NOISE) (127) (the mask word: half-float distance + octahedral facing, the lamp read weighs facing (126) (the mask march at half the steps, the grid cell halfway back read only behind a solid one (125) (the old march in the mask (MarchSteps shared), the grid fallback careful (own surface skipped, edge blend, two cells for a full hit) (124) (the screen splats back, each cube shadow checked against the view (a ghost in empty air left out) (123) (trace-first: the smart trace (growing steps, a crossing refined, no half hits), world-tied offsets, the outer view traced, no screen splats (122) (each slot's trace reach (the dial), the mask read's nearest-depth fallback, the grid splat counts this frame's stamps, no strongest-lamp tracking beside the full trace, tests VR_SCENE_EARLY (121) (120: the trace lift taken back (119: lifted; 118:  // the trace's blockers 30 cm thick (118) (117:  // every slot lamp traced by default, fresh ones too (117) (116:  // the mask read depth-aware (116) (115:  // the trace a function: per lamp (tests) or the top one; its footprint outside branches (115) (114:  // the mask with our AO off (114) (113:  // contact shadows on (113) (112:  // contact shadows hold still (112) (111:  // contact shadows at full rate, within 12 m (111) (110:  // contact shadows behind VR_SCENE_CONTACT (110) (109:  // uncapped slope bias, edge-on fade only at the end (109) (108:  // 16 cubes of 192, the mask bilinear (107:  // the AO pass foveated again, the shadow work over coarse blocks (106:  // edge-on light and the rim fade the mask's shadows (105:  // seamless cubes (105) (104:  // the cubes' normal offset (no acne) (103:  // splats that hold still: footprints, the grid's in-view test by depth, previous places (102:  // the shadow pass at full rate, grid out of view only, a smooth lookup (101:  // the mask's size written every frame (100:  // tests: the mask shown, the slope's share (99:  // enemies never in the grid; valid branches (98:  // loops' conditions before their merge (valid SPIR-V) (97:  // the GPU's slot match where the CPU saw no list (96:  // slope bias for the screen's entries too (95:  // enemies and NPCs out of view as capsules (94: held lights in the slots, lamp programs bind less (93: the lamp's slot from the CPU, a byte; 92: the shadow mask, world-axis cubes two deep (91: 24 map slots read in loops; 90: 16 slots; 89: maps only; 88: the grid into the scene maps, live keys (87: the grid into the scene maps (86: his shadow from up to three lamps; 85: one loop over a lamp shader's lights (84: scene shadow maps (VR_SCENE_MAPS) (83: merge: the lamp work (82: grouping off by default (81: the game's shadow lookups lit without an Identity (80: small lights within 20 cm grouped for shadows (79: blockers 40 cm thick (78: the game's shadowed lamps: ours for each, its maps lit; more steps at medium distance (77: the grid looked up only within its reach, the march ending beyond it (76: single lights cast nothing (light_shadow.h NoCastLights); tests behind VR_LIGHT_PROBE (75: tests; 74: bounce light: a sample at most luminance 2 (73: camera lights by place and reach; 72: matched in either view; 71: the camera fill light removable; 70: lights kept on the camera cast none; 69: live grid and thickness keys; 68: the blocker grid; 67: fixture clearance 12 to 72 cm, the air back to the old one; 66: both eyes' pictures; 65: fixture clearance 6 to 32 cm; 64: small lights all steps; 63: no flicker, full reach, more steps (62: a strength, a held torch casts, edge fade)) and the moon (81 on the other branch: the moon 1.8x the real one (was 3.6x; 77..80 left to the graphics folder's unpushed work) (76: single lights cast nothing (light_shadow.h NoCastLights); tests behind VR_LIGHT_PROBE (75: tests; 74: bounce light: a sample at most luminance 2 (73: camera lights by place and reach; 72: matched in either view; 71: the camera fill light removable; 70: lights kept on the camera cast none; 69: live grid and thickness keys; 68: the blocker grid; 67: fixture clearance 12 to 72 cm, the air back to the old one; 66: both eyes' pictures; 65: fixture clearance 6 to 32 cm; 64: small lights all steps; 63: no flicker, full reach, more steps (62: a strength, a held torch casts, edge fade)))))))))))))))))
constexpr Word VrCutRevision = 3u;        // Monocular variant: the free eye keeps the pushed body cut

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
    return (upstream & 0xFu) | ((scoped & 0xFFFFu) << 4) | ((revision & 0xFFu) << 20) | (Tag << 28);
}

// The body-cutaway permutations' scoped revision: the fragment programs' (theirs too) in its low 12
// bits, the cut's own above (never 0: never the game's own fragment programs' word).
constexpr Word CutScopedRevision() {
    return (VrCutRevision << 12) | VrFragmentRevision;
}
static_assert(Fits(UpstreamShaderVersion, UpstreamBits) && Fits(UpstreamKeyVersion, UpstreamBits));
static_assert(Fits(VrKeyLayoutRevision, RevisionBits) && Fits(VrMetaLayoutRevision, RevisionBits) &&
              Fits(VrKeyLayoutRevisionIndexed, RevisionBits) &&
              Fits(VrMetaLayoutRevisionIndexed, RevisionBits) && Fits(VrRevision, RevisionBits),
              "a revision past its field: widen the word (and retire the old one), never wrap");
static_assert(Fits(VrVertexRevision, ScopedBits) && Fits(VrFragmentRevision, 12u) &&
                  VrCutRevision > 0 && Fits(VrCutRevision, ScopedBits - 12u),
              "a scoped revision past its field: widen the word (and retire the old one), never wrap");

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
        return CutScopedRevision();
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

// The low tag bit separates adaptive lamp binaries from regular ones across scoped revisions.
enum class LampBinary : Word {
    None = 0,
    Adaptive = 5,     // edge pass serial in slot dword 2
    MaskConsumer = 7, // compute-edge lamp consumers skip blending unassigned slots
    FiveLampPass = 8, // same shortcut on High, which consumes the fifth packed verdict
};
constexpr Word ShaderBinaryVersion(Scope scope, LampBinary lamp = LampBinary::None,
                                   bool fog_composite = false) {
    static_assert((Tag & 1u) == 0);
    // The surface composite reads the sign of the shared light's core as its shadow mode: its own
    // bit of the revision, above every lamp kind, so every kind with or without it has a word of
    // its own (2026-11-24: added to the kind, MaskConsumer + the composite came out as
    // FiveLampPass's word. The loader checks each meta's word against that meta's own spec, so
    // no binary was ever taken for another; a word shared by two kinds was only ambiguous).
    constexpr Word FogCompositeBit = Word{1} << (RevisionBits - 1);
    static_assert(VrRevision + static_cast<Word>(LampBinary::FiveLampPass) < FogCompositeBit,
                  "the lamp kinds' revisions must stay below the fog composite's bit");
    const Word revision =
        (VrRevision + static_cast<Word>(lamp)) | (fog_composite ? FogCompositeBit : 0u);
    return Encode(UpstreamShaderVersion, ScopedRevision(scope), revision) |
           (lamp != LampBinary::None ? Word{1} << 28 : 0u);
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
    Misaligned,   // a stage's bindings start where the stages before it do not end (BindingsLineUp):
                  // its descriptors would be read as another stage's; never built
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
    case Reject::Misaligned:
        return "stages' bindings not lined up";
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
