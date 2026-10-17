// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_GPU_READ_BARRIERS' logic (src/video_core/buffer_cache/read_barrier_logic.h): the old
// Buffer::GetBarrier step, the read step that replaces it, and FAST_GPU_READ_BARRIERS_VERIFY's
// comparison, on fixed and random request sequences.

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include "video_core/buffer_cache/read_barrier_logic.h"

namespace {
using namespace VideoCore::ReadBarrierLogic;

// VkPipelineStageFlagBits2 values the requests use.
constexpr uint64_t DrawIndirect = 0x2;
constexpr uint64_t Transfer = 0x1000;
constexpr uint64_t IndexInput = 0x1000000000ull;
constexpr uint64_t VertexAttributeInput = 0x2000000000ull;

// The requests Buffer::GetBarrier gets (BindBuffers, BindVertexBuffers, BindIndexBuffer, the
// indirect draws, TextureCache::RefreshImage, JoinOverlap).
constexpr Request ShaderReadReq{ShaderRead, AllCommands, true, false};
constexpr Request ShaderWriteReq{ShaderWrite, AllCommands, true, false};
constexpr Request VertexReq{VertexAttributeRead, VertexAttributeInput, true, false};
constexpr Request IndexReq{IndexRead, IndexInput, true, false};
constexpr Request IndirectReq{IndirectCommandRead, DrawIndirect, true, false};
constexpr Request TransferReadReq{TransferRead, Transfer, true, false};
constexpr Request TransferWriteReq{TransferWrite, Transfer, true, false};
constexpr Request JoinPostReq{MemoryRead | MemoryWrite, AllCommands, true, false};

// A new Buffer's state (buffer.h).
constexpr State Initial{MemoryRead | MemoryWrite | TransferRead | TransferWrite, AllCommands};

Request Excluded(Request r) {
    r.excluded = true;
    return r;
}

struct Pair {
    State now{Initial};
    State old{Initial};
    int now_barriers{};
    int old_barriers{};

    // One request through both steps; the comparison FAST_GPU_READ_BARRIERS_VERIFY makes.
    Kind Step(const Request& r, Mismatch* mismatch = nullptr) {
        Masks m{};
        const Kind kind = ReadStep(now, r, m);
        const Masks o = OldStep(old, r.access, r.stage);
        now_barriers += m.valid ? 1 : 0;
        old_barriers += o.valid ? 1 : 0;
        const Mismatch result = Check(m, now, o, old);
        if (mismatch) {
            *mismatch = result;
        }
        EXPECT_EQ(result, Mismatch::None);
        return kind;
    }
    // Bit 44's SettleShaderRead: both states.
    void Settle() {
        now = old = {ShaderRead, AllCommands};
    }
};

TEST(ReadBarriers, OldStepIsGetBarrier) {
    State s{Initial};
    Masks m = OldStep(s, ShaderRead, AllCommands);
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.src_stage, AllCommands);
    EXPECT_EQ(m.src_access, Initial.access);
    EXPECT_EQ(m.dst_stage, AllCommands);
    EXPECT_EQ(m.dst_access, ShaderRead);
    EXPECT_EQ(s, (State{ShaderRead, AllCommands}));
    // The same request again: no barrier, the state stays.
    m = OldStep(s, ShaderRead, AllCommands);
    EXPECT_FALSE(m.valid);
    // Another read kind: a read -> read barrier.
    m = OldStep(s, VertexAttributeRead, VertexAttributeInput);
    ASSERT_TRUE(m.valid);
    EXPECT_TRUE(ReadAfterRead(m));
}

TEST(ReadBarriers, OneBarrierForEveryReadKindAfterAWrite) {
    Pair p;
    p.Step(ShaderWriteReq);
    EXPECT_EQ(p.Step(VertexReq), Kind::Broadened);
    EXPECT_EQ(p.Step(ShaderReadReq), Kind::Elided);
    EXPECT_EQ(p.Step(IndexReq), Kind::Elided);
    EXPECT_EQ(p.Step(IndirectReq), Kind::Elided);
    EXPECT_EQ(p.Step(TransferReadReq), Kind::Elided);
    EXPECT_EQ(p.Step(VertexReq), Kind::Elided);
    // The write and the first read each got one; the old step one per read-kind change.
    EXPECT_EQ(p.now_barriers, 2);
    EXPECT_EQ(p.old_barriers, 7);
}

TEST(ReadBarriers, BroadenedBarrierPublishesEverything) {
    State s{ShaderWrite, AllCommands};
    Masks m{};
    ASSERT_EQ(ReadStep(s, IndexReq, m), Kind::Broadened);
    ASSERT_TRUE(m.valid);
    EXPECT_EQ(m.src_stage, AllCommands);
    EXPECT_EQ(m.src_access, ShaderWrite | MemoryWrite);
    EXPECT_EQ(m.dst_stage, AllCommands);
    EXPECT_EQ(m.dst_access, AnyRead);
    EXPECT_TRUE(Elidable(s));
    // It covers the barrier the old step records for the same request.
    State old{ShaderWrite, AllCommands};
    EXPECT_TRUE(Covers(m, OldStep(old, IndexRead, IndexInput)));
    // From a narrow read state (src VERTEX_ATTRIBUTE_INPUT before): every earlier command now.
    State narrow{VertexAttributeRead, VertexAttributeInput};
    ASSERT_EQ(ReadStep(narrow, ShaderReadReq, m), Kind::Broadened);
    EXPECT_EQ(m.src_stage, AllCommands);
    EXPECT_EQ(m.src_access, VertexAttributeRead | MemoryWrite);
}

TEST(ReadBarriers, WriteAfterElidedReadsWaitsForAllOfThem) {
    Pair p;
    p.Step(ShaderWriteReq);
    p.Step(VertexReq);
    p.Step(IndexReq);
    Masks m{};
    State s = p.now;
    ASSERT_EQ(ReadStep(s, ShaderWriteReq, m), Kind::Old);
    ASSERT_TRUE(m.valid);
    // WAR: from every stage, after the elided vertex and index reads.
    EXPECT_EQ(m.src_stage, AllCommands);
    EXPECT_EQ(m.src_access, AnyRead);
    EXPECT_EQ(m.dst_access, ShaderWrite);
    EXPECT_EQ(s, (State{ShaderWrite, AllCommands}));
}

TEST(ReadBarriers, ExcludedReadsTakeTheOldStep) {
    // A buffer holding image data, a copy's source, or a read asked for by the draw that also
    // writes it.
    Pair p;
    p.Step(ShaderWriteReq);
    EXPECT_EQ(p.Step(Excluded(VertexReq)), Kind::Old);
    EXPECT_EQ(p.now, p.old);
    EXPECT_EQ(p.Step(Excluded(ShaderReadReq)), Kind::Old);
    EXPECT_EQ(p.now_barriers, p.old_barriers);
    // The next draw: broadened, its source every stage and every write (the draw's write landed
    // after the barriers of its own bindings).
    Masks m{};
    State s = p.now;
    ASSERT_EQ(ReadStep(s, IndexReq, m), Kind::Broadened);
    EXPECT_EQ(m.src_stage, AllCommands);
    EXPECT_NE(m.src_access & MemoryWrite, 0u);
    // A partial range is never broadened either.
    Request partial = IndexReq;
    partial.whole = false;
    State t{ShaderWrite, AllCommands};
    EXPECT_EQ(ReadStep(t, partial, m), Kind::Old);
    EXPECT_EQ(t, (State{IndexRead, IndexInput}));
}

TEST(ReadBarriers, WritesAndHostReadsAreUnchanged) {
    for (const Request& r : {ShaderWriteReq, TransferWriteReq, JoinPostReq,
                             Request{HostRead, 0x4000, true, false}}) {
        State a{VertexAttributeRead, VertexAttributeInput};
        State b{a};
        Masks m{};
        EXPECT_EQ(ReadStep(a, r, m), Kind::Old);
        const Masks o = OldStep(b, r.access, r.stage);
        EXPECT_EQ(a, b);
        EXPECT_EQ(m.valid, o.valid);
        EXPECT_EQ(m.src_access, o.src_access);
        EXPECT_EQ(m.dst_access, o.dst_access);
        EXPECT_EQ(m.src_stage, o.src_stage);
        EXPECT_EQ(m.dst_stage, o.dst_stage);
    }
}

TEST(ReadBarriers, CheckFindsWhatItShould) {
    const Masks write_to_read{true, AllCommands, ShaderWrite, VertexAttributeInput,
                              VertexAttributeRead};
    const Masks read_to_read{true, AllCommands, ShaderRead, VertexAttributeInput,
                             VertexAttributeRead};
    const State any{AnyRead, AllCommands};
    const State vertex{VertexAttributeRead, VertexAttributeInput};
    // No barrier where the old step publishes a write.
    EXPECT_EQ(Check({}, any, write_to_read, vertex), Mismatch::ElidedWrite);
    // No barrier where the old one only orders a read after a read: what the switch saves.
    EXPECT_EQ(Check({}, any, read_to_read, vertex), Mismatch::None);
    // A barrier that does not make the write visible.
    const Masks weak{true, AllCommands, ShaderRead, AllCommands, AnyRead};
    EXPECT_EQ(Check(weak, any, write_to_read, vertex), Mismatch::Weaker);
    // A state that is neither the old one nor (AnyRead, ALL_COMMANDS).
    EXPECT_EQ(Check({}, State{IndexRead, IndexInput}, {}, vertex), Mismatch::Diverged);
    // Merge covers both.
    const Masks merged = Merge(weak, write_to_read);
    EXPECT_TRUE(Covers(merged, weak));
    EXPECT_TRUE(Covers(merged, write_to_read));
    EXPECT_TRUE(Covers(Merge({}, read_to_read), read_to_read));
    // MEMORY_READ / MEMORY_WRITE stand for every read / write.
    EXPECT_TRUE(AccessCovers(MemoryRead, AnyRead | HostRead));
    EXPECT_TRUE(AccessCovers(MemoryWrite, ShaderWrite | TransferWrite));
    EXPECT_FALSE(AccessCovers(MemoryRead, ShaderWrite));
    EXPECT_FALSE(AccessCovers(ShaderRead, IndexRead));
}

// Random requests (a fixed xorshift seed): the comparison never fires, and without exclusions
// the read step never records more barriers than the old one.
TEST(ReadBarriers, RandomSequences) {
    constexpr std::array<Request, 8> requests{ShaderReadReq,  ShaderWriteReq,  VertexReq,
                                              IndexReq,       IndirectReq,     TransferReadReq,
                                              TransferWriteReq, JoinPostReq};
    uint64_t x = 0x9E3779B97F4A7C15ull;
    const auto next = [&] {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    for (const bool exclusions : {false, true}) {
        for (int run = 0; run < 200; ++run) {
            Pair p;
            int elided = 0;
            for (int i = 0; i < 400; ++i) {
                const uint64_t pick = next();
                if (pick % 23 == 0) {
                    if (!Elidable(p.now)) {
                        p.Settle(); // bit 44 binds only when the state is not settled
                    }
                    continue;
                }
                Request r = requests[pick % requests.size()];
                // GetBarrierReads excludes a copy's source (TRANSFER) always, image data and a
                // same-draw write now and then.
                if (exclusions && (r.stage == Transfer || (pick >> 32) % 5 == 0)) {
                    r.excluded = true;
                }
                Mismatch mismatch{};
                elided += p.Step(r, &mismatch) == Kind::Elided ? 1 : 0;
                ASSERT_EQ(mismatch, Mismatch::None) << "run " << run << " step " << i;
            }
            if (!exclusions) {
                EXPECT_LE(p.now_barriers, p.old_barriers);
            }
            EXPECT_GT(elided, 0);
        }
    }
}

} // namespace
