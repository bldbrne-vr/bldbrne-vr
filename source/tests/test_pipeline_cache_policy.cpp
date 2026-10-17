// SPDX-License-Identifier: GPL-2.0-or-later

// The on-disk pipeline cache (video_core/pipeline_cache_policy.h, common/serdes.h): the version
// words (asymmetric projection changes metadata and vertex binaries, the pushed VR values and
// then the pushed motion buffers every binary and the metadata, the permutation ids the keys and
// the metadata), the scoped revisions (a vertex-epilogue, a fragment or a vr_cut bump leaves the
// other programs cached; no two revision triples share a word), the SPIR-V sanity check, the
// prune rule, the permutation ids' names (FAST_CPU_PERM_IDS; the indexed naming keeps the words
// before them) and the warm-up's collection of the files no permutation is named by, and the
// reader that never crashes on a truncated or corrupted file.

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "common/serdes.h"
#include "video_core/pipeline_cache_policy.h"

namespace {

using namespace Vulkan::PipelineCachePolicy;

// The words of the cache the pre-asymmetric VrRevision 3 builds wrote: 3 | 0x5652 << 16 |
// 3 << 8 for metas and binaries, 4 | ... for pipeline keys.
constexpr Word LegacyShaderWord = 3u | (0x5652u << 16) | (3u << 8);
constexpr Word LegacyKeyWord = 4u | (0x5652u << 16) | (3u << 8);

TEST(PipelineCachePolicy, NativeDrawFlagsInvalidateMetadataAndShaderABI) {
    // Changing any of these makes every user's cache stale once (one slow first session): only
    // with a reason, and then this test changes with it.
    EXPECT_EQ(PipelineKeyVersion(Naming::Ids), Encode(UpstreamKeyVersion, 0, 4));
    EXPECT_NE(PipelineKeyVersion(Naming::Ids), LegacyKeyWord);
    EXPECT_EQ(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, 13));
    EXPECT_EQ(ShaderBinaryVersion(Scope::Common), Encode(UpstreamShaderVersion, 0, 8));
    EXPECT_EQ(ShaderBinaryVersion(Scope::VertexEpilogue), Encode(UpstreamShaderVersion, 5, 8));
    EXPECT_EQ(ShaderBinaryVersion(Scope::FragmentCut), Encode(UpstreamShaderVersion, 1, 8));
    EXPECT_EQ(ShaderBinaryVersion(Scope::Fragment), Encode(UpstreamShaderVersion, 1, 8));
}

TEST(PipelineCachePolicy, PushedVrValuesRetireTheBakedPermutations) {
    // Shader::VrPushData: every native graphics stage declares the longer push block and
    // RuntimeInfo gained native_asymmetric, so no meta or binary of the builds that baked the
    // projection, the light tiles and the cut radii (meta 10, revision 6) is used again; the
    // pipeline keys' layout did not change.
    for (const Scope scope :
         {Scope::Common, Scope::VertexEpilogue, Scope::FragmentCut, Scope::Fragment}) {
        EXPECT_NE(ShaderBinaryVersion(scope),
                  Encode(UpstreamShaderVersion, ScopedRevision(scope), 6u));
    }
    EXPECT_NE(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, 10u));
    // (The keys' word stayed revision 3 until the permutation ids: PermutationIdsRenameEverything.)
    // The pushed cut's permutations (vr_cut 1, only the flag) keep the cut scope.
    EXPECT_EQ(ScopeOf(false, true, 1u), Scope::FragmentCut);
}

TEST(PipelineCachePolicy, PushedMotionBuffersRetireTheEarlierPushBlock) {
    // The push block gained the motion buffers' addresses (every native graphics stage declares
    // them) and the metas may now hold a motion permutation (vr_motion's flag alone), so no binary
    // or meta of the builds before (revision 7, meta 11) is used again; the keys' layout is the
    // same (a motion variant's key is an ordinary key, its colour slot 7 included).
    for (const Scope scope :
         {Scope::Common, Scope::VertexEpilogue, Scope::FragmentCut, Scope::Fragment}) {
        EXPECT_NE(ShaderBinaryVersion(scope),
                  Encode(UpstreamShaderVersion, ScopedRevision(scope), 7u));
    }
    EXPECT_NE(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, 11u));
}

TEST(PipelineCachePolicy, PermutationIdsRenameEverything) {
    // A permutation is named by its id (the hash of its specialization), no longer by its index
    // in the session's list: the keys' stage hashes and the metas' names mean something else, so
    // every key and meta of revision 3 / 12 is stale (their layouts are the same)...
    EXPECT_NE(PipelineKeyVersion(Naming::Ids), Encode(UpstreamKeyVersion, 0, 3u));
    EXPECT_NE(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, 12u));
    EXPECT_EQ(PipelineKeyVersion(Naming::Ids), Encode(UpstreamKeyVersion, 0, VrKeyLayoutRevision));
    EXPECT_EQ(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, VrMetaLayoutRevision));
    // ...while the SPIR-V is the same: the binaries' words did not move (an earlier binary is
    // never found under its new name anyway, and the warm-up deletes it).
    EXPECT_EQ(ShaderBinaryVersion(Scope::Common), Encode(UpstreamShaderVersion, 0, 8u));
    EXPECT_EQ(ShaderBinaryVersion(Scope::VertexEpilogue), Encode(UpstreamShaderVersion, 5u, 8u));
    EXPECT_EQ(ShaderBinaryVersion(Scope::Fragment), Encode(UpstreamShaderVersion, 1u, 8u));
    EXPECT_EQ(ShaderBinaryVersion(Scope::FragmentCut), Encode(UpstreamShaderVersion, 1u, 8u));
}

TEST(PipelineCachePolicy, TheIndexedNamingKeepsTheWordsBeforeTheIds) {
    // FAST_CPU_PERM_IDS off (the default): the keys and metas are named by HashCombine(program,
    // index) as before, so their words are the ones before the ids (key 3, meta 12) and the
    // caches written before them are read unchanged...
    EXPECT_EQ(PipelineKeyVersion(Naming::Indexed), Encode(UpstreamKeyVersion, 0, 3u));
    EXPECT_EQ(ShaderMetaVersion(Naming::Indexed), Encode(UpstreamShaderVersion, 0, 12u));
    EXPECT_EQ(PipelineKeyVersion(Naming::Indexed),
              Encode(UpstreamKeyVersion, 0, VrKeyLayoutRevisionIndexed));
    EXPECT_EQ(ShaderMetaVersion(Naming::Indexed),
              Encode(UpstreamShaderVersion, 0, VrMetaLayoutRevisionIndexed));
    // ...while the two namings never read each other's keys or metas...
    EXPECT_NE(PipelineKeyVersion(Naming::Indexed), PipelineKeyVersion(Naming::Ids));
    EXPECT_NE(ShaderMetaVersion(Naming::Indexed), ShaderMetaVersion(Naming::Ids));
    EXPECT_EQ(OtherNaming(Naming::Ids), Naming::Indexed);
    EXPECT_EQ(OtherNaming(Naming::Indexed), Naming::Ids);
    // ...and the indexed keys' word is the one every build wrote before the ids (the keys'
    // layout never changed until then: their stages' meta and binary words decide), while no
    // meta of the pre-asymmetric builds is read.
    EXPECT_EQ(PipelineKeyVersion(Naming::Indexed), LegacyKeyWord);
    EXPECT_NE(ShaderMetaVersion(Naming::Indexed), LegacyShaderWord);
}

TEST(PipelineCachePolicy, WrittenColourTargetsRetireTheFragmentMetas) {
    // A fragment permutation keeps only the colour targets its program writes, in the form the
    // recompiler reads them (FragmentRuntimeInfo::KeepWrittenColorBuffers). The metas before it
    // hold every bound target as its registers say, so a selection would never match them again:
    // the game's fragment programs (until then in the Common scope, scoped 0) and the cut
    // permutations (cut 0) of this revision are stale...
    const Word old_fragment = Encode(UpstreamShaderVersion, 0, VrRevision);
    EXPECT_NE(ShaderBinaryVersion(ScopeOf(false, true, 0)), old_fragment);
    EXPECT_NE(ShaderBinaryVersion(ScopeOf(false, true, 1u)), old_fragment);
    // ...while every other program stays cached: the compute, hull, export and local binaries and
    // the vertex and geometry ones (the vertex epilogue's scope) keep their words, and the meta
    // and key layouts did not change.
    EXPECT_EQ(ShaderBinaryVersion(ScopeOf(false, false, 0)), old_fragment);
    EXPECT_EQ(ShaderBinaryVersion(ScopeOf(true, false, 0)),
              Encode(UpstreamShaderVersion, VrVertexRevision, VrRevision));
    EXPECT_EQ(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, VrMetaLayoutRevision));
    EXPECT_EQ(PipelineKeyVersion(Naming::Ids), Encode(UpstreamKeyVersion, 0, VrKeyLayoutRevision));
}

TEST(PipelineCachePolicy, ScopeOfAProgram) {
    // The hardware vertex stage (ApplyStereoShift in its epilogue), also with a cut value around.
    EXPECT_EQ(ScopeOf(true, false, 0), Scope::VertexEpilogue);
    EXPECT_EQ(ScopeOf(true, false, 1234), Scope::VertexEpilogue);
    // The game's own fragment programs vs the VR-owned body-cutaway permutations.
    EXPECT_EQ(ScopeOf(false, true, 0), Scope::Fragment);
    EXPECT_EQ(ScopeOf(false, true, 220u | (30u << 16)), Scope::FragmentCut);
    EXPECT_EQ(ScopeOf(false, true, 0x80000000u), Scope::FragmentCut);
    // Compute / hull / export / local: a vr_cut value is never theirs.
    EXPECT_EQ(ScopeOf(false, false, 0), Scope::Common);
    EXPECT_EQ(ScopeOf(false, false, 77), Scope::Common);
}

TEST(PipelineCachePolicy, AScopedBumpOnlyChangesItsScope) {
    // Geometry-stage sprites facing the eye change only the vertex epilogue's revision.
    const Word vs_before = Encode(UpstreamShaderVersion, 4, VrRevision);
    const Word vs_after = Encode(UpstreamShaderVersion, 5, VrRevision);
    EXPECT_NE(vs_before, vs_after);
    EXPECT_EQ(ShaderBinaryVersion(Scope::VertexEpilogue), vs_after);
    EXPECT_EQ(ShaderBinaryVersion(Scope::Common), Encode(UpstreamShaderVersion, 0, VrRevision));
    EXPECT_EQ(ShaderBinaryVersion(Scope::FragmentCut),
              Encode(UpstreamShaderVersion, VrCutRevision, VrRevision));
    EXPECT_EQ(ShaderBinaryVersion(Scope::Fragment),
              Encode(UpstreamShaderVersion, VrFragmentRevision, VrRevision));
    // The meta and key words never follow a SPIR-V bump (their layout did not change).
    EXPECT_EQ(ShaderMetaVersion(Naming::Ids), Encode(UpstreamShaderVersion, 0, VrMetaLayoutRevision));
    EXPECT_EQ(PipelineKeyVersion(Naming::Ids), Encode(UpstreamKeyVersion, 0, VrKeyLayoutRevision));
}

TEST(PipelineCachePolicy, NoTwoRevisionTriplesShareAWord) {
    // An additive scheme would let (revision 4, scoped 0) read a binary written by (revision 3,
    // scoped 1): stale SPIR-V used. The fields are separate bits.
    EXPECT_NE(Encode(3, 0, 4), Encode(3, 1, 3));
    EXPECT_NE(Encode(4, 0, 3), Encode(3, 1, 3));
    std::set<Word> seen;
    for (Word up = 0; up < 16; ++up) {
        for (Word sc = 0; sc < 16; ++sc) {
            for (Word rev = 0; rev < 256; ++rev) {
                const Word w = Encode(up, sc, rev);
                EXPECT_EQ(w >> 16, Tag);
                seen.insert(w);
            }
        }
    }
    EXPECT_EQ(seen.size(), 16u * 16u * 256u);
}

std::vector<std::uint32_t> Module(std::size_t words) {
    std::vector<std::uint32_t> v(words, 0u);
    if (words >= 5) {
        v[0] = SpirvMagic;
        v[1] = 0x00010600u; // 1.6
        v[2] = 0;
        v[3] = 42; // id bound
        v[4] = 0;  // schema
    }
    return v;
}

TEST(PipelineCachePolicy, OnlySpirvReachesTheDriver) {
    EXPECT_TRUE(LooksLikeSpirv(Module(64)));
    EXPECT_FALSE(LooksLikeSpirv(Module(0)));   // an empty file
    EXPECT_FALSE(LooksLikeSpirv(Module(5)));   // a header and nothing else (truncated)
    auto bad = Module(64);
    bad[0] = 0x03022307u; // the wrong endianness / another file's bytes
    EXPECT_FALSE(LooksLikeSpirv(bad));
    bad = Module(64);
    bad[3] = 0; // no ids
    EXPECT_FALSE(LooksLikeSpirv(bad));
    bad = Module(64);
    bad[4] = 1; // the reserved schema word
    EXPECT_FALSE(LooksLikeSpirv(bad));
}

TEST(PipelineCachePolicy, RejectedKeysArePrunedExceptInTheArchive) {
    EXPECT_FALSE(PruneRejected(Reject::None, false));
    EXPECT_FALSE(PruneRejected(Reject::None, true));
    // The other mode's keys stay for the sessions of that mode, the object-motion variants' for
    // the sessions started with an upscaler, the other permutation naming's for its sessions.
    EXPECT_FALSE(PruneRejected(Reject::OtherMode, false));
    EXPECT_FALSE(PruneRejected(Reject::MotionIdle, false));
    EXPECT_FALSE(PruneRejected(Reject::OtherNaming, false));
    for (Word r = 1; r < static_cast<Word>(Reject::Count); ++r) {
        const Reject why = static_cast<Reject>(r);
        EXPECT_EQ(PruneRejected(why, false), why != Reject::OtherMode &&
                                                 why != Reject::MotionIdle &&
                                                 why != Reject::OtherNaming)
            << RejectName(why);
        EXPECT_FALSE(PruneRejected(static_cast<Reject>(r), true));
        EXPECT_NE(std::string{RejectName(static_cast<Reject>(r))}, std::string{"?"});
    }
}

TEST(PipelineCachePolicy, KeptKeysNameTheirPermutations) {
    // The loaded keys, the other mode's and the object-motion variants' stay on disk (and so do
    // the files they name); every other rejected key is pruned (outside the archive), but the
    // other naming's, which stays unread: its stage hashes are no ids of this naming.
    EXPECT_TRUE(KeptKey(Reject::None));
    EXPECT_TRUE(KeptKey(Reject::OtherMode));
    EXPECT_TRUE(KeptKey(Reject::MotionIdle));
    EXPECT_FALSE(KeptKey(Reject::OtherNaming));
    for (Word r = 1; r < static_cast<Word>(Reject::Count); ++r) {
        const Reject why = static_cast<Reject>(r);
        EXPECT_EQ(KeptKey(why), !PruneRejected(why, false) && why != Reject::OtherNaming)
            << RejectName(why);
    }
}

// ---- permutation ids

// The name the cache writes for an id (fmt "{:#018x}").
std::string Name(Id id) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(id));
    return text;
}

TEST(PermutationIds, NeverZero) {
    // 0 is a pipeline key's "no program at this stage".
    EXPECT_EQ(NonZeroId(0), 1u);
    EXPECT_EQ(NonZeroId(1), 1u);
    EXPECT_EQ(NonZeroId(0xFEDCBA9876543210ull), 0xFEDCBA9876543210ull);
    // The collision fallback: deterministic, never 0, never the id it replaces, and a chain of
    // them does not cycle quickly.
    std::set<Id> seen;
    Id id = 0x1234;
    for (int i = 0; i < 1000; ++i) {
        const Id next = NextId(id);
        EXPECT_NE(next, 0u);
        EXPECT_NE(next, id);
        EXPECT_EQ(NextId(id), next);
        seen.insert(next);
        id = next;
    }
    EXPECT_EQ(seen.size(), 1000u);
}

TEST(PermutationIds, NamesRoundTrip) {
    for (const Id id : {Id{1}, Id{0x0184f2888f6684c2ull}, Id{0xFFFFFFFFFFFFFFFFull},
                        Id{0x8000000000000000ull}, Id{0x00000000000000abull}}) {
        Id parsed = 0;
        ASSERT_TRUE(ParseId(Name(id), parsed)) << Name(id);
        EXPECT_EQ(parsed, id);
    }
    Id parsed = 0;
    EXPECT_TRUE(ParseId("0X00000000DEADBEEF", parsed)); // any case
    EXPECT_EQ(parsed, 0xDEADBEEFull);
}

TEST(PermutationIds, OtherNamesAreNoIds) {
    Id parsed = 0x77;
    // The earlier binaries' "<program>_<index>" and the pipeline keys' "g_<hash>" / "c_<hash>".
    EXPECT_FALSE(ParseId("0x0184f2888f6684c2_0", parsed));
    EXPECT_FALSE(ParseId("0x0184f2888f6684c2_12", parsed));
    EXPECT_FALSE(ParseId("g_0x0184f2888f6684c2", parsed));
    // Too short / long, no prefix, not hex, the reserved 0.
    EXPECT_FALSE(ParseId("0x184f2888f6684c2", parsed));
    EXPECT_FALSE(ParseId("0x00184f2888f6684c2", parsed));
    EXPECT_FALSE(ParseId("000184f2888f6684c2", parsed));
    EXPECT_FALSE(ParseId("0x0184f2888f6684cg", parsed));
    EXPECT_FALSE(ParseId("0x0000000000000000", parsed));
    EXPECT_FALSE(ParseId("", parsed));
    EXPECT_FALSE(ParseId("profile", parsed));
    EXPECT_EQ(parsed, 0x77u); // untouched
}

TEST(PermutationIds, TheWarmUpCollectsWhatNoPermutationIsNamedBy) {
    const std::unordered_set<Id> live{0x0184f2888f6684c2ull, 0xe881fd8fe40cb35dull};
    const auto named = [&](Id id) { return live.contains(id); };
    // A permutation the session holds or a kept key names: its meta and binary stay.
    EXPECT_FALSE(CollectUnnamed(Name(0x0184f2888f6684c2ull), false, named));
    EXPECT_FALSE(CollectUnnamed(Name(0xe881fd8fe40cb35dull), false, named));
    // Any other id (stale, only a pruned key's, never named), an earlier binary's name, junk.
    EXPECT_TRUE(CollectUnnamed(Name(0x1111111111111111ull), false, named));
    EXPECT_TRUE(CollectUnnamed("0x0184f2888f6684c2_0", false, named));
    EXPECT_TRUE(CollectUnnamed("0x0184f2888f6684c2_3", false, named));
    EXPECT_TRUE(CollectUnnamed("something", false, named));
    // The archive cannot delete single entries.
    EXPECT_FALSE(CollectUnnamed(Name(0x1111111111111111ull), true, named));
    EXPECT_FALSE(CollectUnnamed("0x0184f2888f6684c2_0", true, named));
}

// ---- the reader (common/serdes.h): a truncated or corrupted file fails, it never crashes

std::vector<std::uint8_t> Sample() {
    Serialization::Archive ar;
    Serialization::Writer w{ar};
    w.Write(std::uint32_t{0x56520303u});
    w.Write(std::uint64_t{0x1122334455667788ull});
    w.Write(std::string{"0101"});
    w.Write(std::vector<std::uint32_t>{7u, 8u, 9u});
    return ar.TakeOff();
}

TEST(PipelineCacheReader, RoundTrip) {
    Serialization::Archive ar{Sample()};
    Serialization::Reader r{ar};
    std::uint32_t a{};
    std::uint64_t b{};
    std::string c{};
    std::vector<std::uint32_t> d{};
    r.Read(a);
    r.Read(b);
    r.Read(c);
    r.Read(d);
    EXPECT_FALSE(ar.Failed());
    EXPECT_EQ(a, 0x56520303u);
    EXPECT_EQ(b, 0x1122334455667788ull);
    EXPECT_EQ(c, "0101");
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[2], 9u);
    EXPECT_TRUE(ar.IsEoS());
}

TEST(PipelineCacheReader, EveryTruncationFailsCleanly) {
    const auto full = Sample();
    for (std::size_t n = 0; n < full.size(); ++n) {
        std::vector<std::uint8_t> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
        Serialization::Archive ar{std::move(cut)};
        Serialization::Reader r{ar};
        std::uint32_t a{};
        std::uint64_t b{};
        std::string c{};
        std::vector<std::uint32_t> d{};
        r.Read(a);
        r.Read(b);
        r.Read(c);
        r.Read(d);
        EXPECT_TRUE(ar.Failed()) << "cut at " << n;
        EXPECT_LE(d.size(), 3u);
        EXPECT_LE(c.size(), 4u);
    }
}

TEST(PipelineCacheReader, AFailedReadZeroesAndStops) {
    Serialization::Archive ar{std::vector<std::uint8_t>{1, 2}};
    Serialization::Reader r{ar};
    std::uint32_t v = 0xFFFFFFFFu;
    r.Read(v);
    EXPECT_TRUE(ar.Failed());
    EXPECT_EQ(v, 0u);
    EXPECT_EQ(ar.Remaining(), 0u);
    // Nothing more is read after a failure.
    std::uint8_t x = 5;
    r.Read(x);
    EXPECT_EQ(x, 0u);
}

TEST(PipelineCacheReader, CorruptedCountsDoNotAllocate) {
    // A container / string whose length word is garbage: no huge allocation, no long loop.
    Serialization::Archive src;
    Serialization::Writer w{src};
    w.Write(std::size_t{0xFFFFFFFFFFFFull});
    w.Write(std::uint32_t{1});
    const auto bytes = src.TakeOff();
    {
        Serialization::Archive ar{std::vector<std::uint8_t>(bytes)};
        Serialization::Reader r{ar};
        std::vector<std::uint32_t> v;
        r.Read(v);
        EXPECT_TRUE(ar.Failed());
        EXPECT_TRUE(v.empty());
    }
    {
        Serialization::Archive ar{std::vector<std::uint8_t>(bytes)};
        Serialization::Reader r{ar};
        std::string s;
        r.Read(s);
        EXPECT_TRUE(ar.Failed());
        EXPECT_TRUE(s.empty());
    }
}

TEST(PipelineCacheReader, AdvancePastTheEndFails) {
    Serialization::Archive ar{std::vector<std::uint8_t>(8, 0)};
    ar.Advance(4);
    EXPECT_FALSE(ar.Failed());
    EXPECT_EQ(ar.Remaining(), 4u);
    ar.Advance(5);
    EXPECT_TRUE(ar.Failed());
    EXPECT_EQ(ar.Remaining(), 0u);
}

} // namespace
