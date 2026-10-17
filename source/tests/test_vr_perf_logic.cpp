// SPDX-License-Identifier: GPL-2.0-or-later

// The GPU-pass optimizations' pure logic (renderer_vulkan/vr_perf_logic.h): when the image ->
// buffer resync may be skipped (perf_resync_memo), which transitions count as writes, the depth
// target's view and layout (perf_depth_one_layout), the live switch and the shadow check's
// comparison.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_perf_logic.h"

namespace {

namespace PL = VideoCore::PerfLogic;

PL::ResyncKey Key() {
    return PL::ResyncKey{.image_uid = 7,
                         .vk_image = 0x1000,
                         .buffer_serial = 3,
                         .vk_buffer = 0x2000,
                         .buf_offset = 256,
                         .copy_size = 19668480,
                         .num_copies = 1,
                         .tiled = 1};
}

PL::ResyncSeqs Seqs() {
    return PL::ResyncSeqs{.image = 10, .buffer = 20, .global = 5};
}

TEST(VrPerfResyncMemo, NoMemoNeverSkips) {
    const PL::ResyncMemo memo{};
    EXPECT_FALSE(PL::CanSkip(memo, Key(), Seqs()));
    EXPECT_FALSE(PL::CanSkip(memo, PL::ResyncKey{}, PL::ResyncSeqs{}));
}

TEST(VrPerfResyncMemo, SameTargetNothingWrittenSkips) {
    PL::ResyncMemo memo{};
    PL::Store(memo, Key(), Seqs(), false);
    EXPECT_TRUE(PL::CanSkip(memo, Key(), Seqs()));
    // Again and again while nothing moves.
    EXPECT_TRUE(PL::CanSkip(memo, Key(), Seqs()));
}

TEST(VrPerfResyncMemo, AnyWriteCounterInvalidates) {
    PL::ResyncMemo memo{};
    PL::Store(memo, Key(), Seqs(), false);
    auto s = Seqs();
    s.image += 1; // the image transitioned for a write (render target, storage, copy, clear)
    EXPECT_FALSE(PL::CanSkip(memo, Key(), s));
    s = Seqs();
    s.buffer += 1; // an upload, a shader / copy / fill write, another resync into the buffer
    EXPECT_FALSE(PL::CanSkip(memo, Key(), s));
    s = Seqs();
    s.global += 1; // a toggle or a raw-memory (DMA) shader
    EXPECT_FALSE(PL::CanSkip(memo, Key(), s));
}

TEST(VrPerfResyncMemo, AnyKeyFieldInvalidates) {
    PL::ResyncMemo memo{};
    PL::Store(memo, Key(), Seqs(), false);
    const std::array<void (*)(PL::ResyncKey&), 8> changes = {
        [](PL::ResyncKey& k) { k.image_uid += 1; },     // another image at the address
        [](PL::ResyncKey& k) { k.vk_image += 8; },      // a new backing (MSAA switch)
        [](PL::ResyncKey& k) { k.buffer_serial += 1; }, // the buffer recreated (join, GC)
        [](PL::ResyncKey& k) { k.vk_buffer += 8; },     // another VkBuffer
        [](PL::ResyncKey& k) { k.buf_offset += 4; },    // other bytes of the buffer
        [](PL::ResyncKey& k) { k.copy_size -= 4; },     // fewer mips fit
        [](PL::ResyncKey& k) { k.num_copies += 1; },    // more mips
        [](PL::ResyncKey& k) { k.tiled = 0; },          // the linear path
    };
    for (const auto change : changes) {
        auto k = Key();
        change(k);
        EXPECT_FALSE(PL::CanSkip(memo, k, Seqs()));
    }
    EXPECT_TRUE(PL::CanSkip(memo, Key(), Seqs()));
}

TEST(VrPerfResyncMemo, WriteBoundInThisDrawKeepsNoMemo) {
    // The draw's shader writes the image or the buffer after its bindings, i.e. after this
    // resync on the GPU, although their counters moved before it: no memo.
    PL::ResyncMemo memo{};
    PL::Store(memo, Key(), Seqs(), true);
    EXPECT_FALSE(PL::CanSkip(memo, Key(), Seqs()));
    // The next full resync (a later draw) stores one again.
    PL::Store(memo, Key(), Seqs(), false);
    EXPECT_TRUE(PL::CanSkip(memo, Key(), Seqs()));
}

TEST(VrPerfResyncMemo, StoreReplacesTheTarget) {
    PL::ResyncMemo memo{};
    PL::Store(memo, Key(), Seqs(), false);
    auto other = Key();
    other.buf_offset = 4096;
    PL::Store(memo, other, Seqs(), false);
    EXPECT_FALSE(PL::CanSkip(memo, Key(), Seqs())); // one memo per image: the last target
    EXPECT_TRUE(PL::CanSkip(memo, other, Seqs()));
}

TEST(VrPerfAccess, ReadsAreNotWrites) {
    // SHADER_READ, SHADER_SAMPLED_READ, TRANSFER_READ, COLOR/DEPTH_ATTACHMENT_READ, MEMORY_READ.
    for (const uint64_t a : {0x20ull, 0x100000000ull, 0x800ull, 0x80ull, 0x200ull, 0x8000ull,
                             0x20ull | 0x800ull, 0x0ull}) {
        EXPECT_FALSE(PL::MayWrite(a, false)) << a;
    }
}

TEST(VrPerfAccess, WritesAndUndefinedAreWrites) {
    // SHADER_WRITE, COLOR_ATTACHMENT_WRITE, DEPTH_STENCIL_ATTACHMENT_WRITE, TRANSFER_WRITE,
    // HOST_WRITE, MEMORY_WRITE, SHADER_STORAGE_WRITE; a read with a write; an unknown bit.
    for (const uint64_t a : {0x40ull, 0x100ull, 0x400ull, 0x1000ull, 0x4000ull, 0x10000ull,
                             0x400000000ull, 0x20ull | 0x40ull, 0x80ull | 0x100ull,
                             0x200ull | 0x400ull, 1ull << 50}) {
        EXPECT_TRUE(PL::MayWrite(a, false)) << a;
    }
    // Out of UNDEFINED the contents are gone: a write even for a read.
    EXPECT_TRUE(PL::MayWrite(0x20ull, true));
    EXPECT_TRUE(PL::MayWrite(0x0ull, true));
}

// Today's rule (before perf_depth_one_layout), restated from Rasterizer::BeginRendering at
// 7fbc8e22: the write view's layout when the draw writes depth, else read-only (with stencil
// attachment writes when the stencil test is on).
PL::DepthLayout Today(bool writes_depth, bool has_stencil, bool stencil_enable) {
    const bool stencil_write = has_stencil && stencil_enable && !writes_depth;
    return writes_depth ? (has_stencil ? PL::DepthLayout::DepthStencilAttachment
                                       : PL::DepthLayout::DepthAttachment)
           : stencil_write ? PL::DepthLayout::DepthReadOnlyStencilAttachment
           : has_stencil   ? PL::DepthLayout::DepthStencilReadOnly
                           : PL::DepthLayout::DepthReadOnly;
}

TEST(VrPerfDepth, SwitchOffIsTodayForEveryCase) {
    for (int m = 0; m < 32; ++m) {
        const bool writes = m & 1, sampled = m & 2, stencil = m & 4, test = m & 8, now = m & 16;
        const bool keep = PL::KeepDepthWritable(writes, false, sampled, now);
        EXPECT_FALSE(keep) << m;
        EXPECT_EQ(PL::DepthAttachmentLayout(writes || keep, stencil, test),
                  Today(writes, stencil, test))
            << m;
    }
}

TEST(VrPerfDepth, AWriteOffDrawInTheWritableLayoutStaysThere) {
    for (int m = 0; m < 4; ++m) {
        const bool stencil = m & 1, test = m & 2;
        const bool keep = PL::KeepDepthWritable(false, true, false, true);
        EXPECT_TRUE(keep);
        EXPECT_FALSE(PL::KeepDepthWritable(true, true, false, true)); // already the write view
        // The same layout as the depth-writing draw before it: no transition, no pass break.
        EXPECT_EQ(PL::DepthAttachmentLayout(keep, stencil, test),
                  PL::DepthAttachmentLayout(true, stencil, test))
            << m;
    }
}

TEST(VrPerfDepth, SampledOrReadOnlyDepthKeepsTodaysChoice) {
    for (int m = 0; m < 16; ++m) {
        const bool writes = m & 1, stencil = m & 2, test = m & 4, now = m & 8;
        // Sampled: today's layout whatever the depth is in now.
        EXPECT_FALSE(PL::KeepDepthWritable(writes, true, true, now)) << m;
        // Not in the writable layout now: today's layout.
        EXPECT_FALSE(PL::KeepDepthWritable(writes, true, false, false)) << m;
        (void)stencil;
        (void)test;
    }
}

// Every sequence of draws into one depth target (each: writes depth or not, samples it or not;
// with and without stencil, stencil test on or off) up to length 8: with the switch on, the number
// of view/layout changes between consecutive draws (each one ends the render pass, a layout change
// also records a barrier) is never more than today's, and the draws that write depth or sample it
// get exactly today's view and layout.
TEST(VrPerfDepth, NeverMoreViewOrLayoutChangesThanToday) {
    struct Att {
        bool write_view;
        PL::DepthLayout layout;
        bool operator==(const Att&) const = default;
    };
    int fewer = 0;
    for (int cfg = 0; cfg < 4; ++cfg) {
        const bool stencil = cfg & 1, test = cfg & 2;
        for (int len = 1; len <= 8; ++len) {
            for (int seq = 0; seq < (1 << (2 * len)); ++seq) {
                // Both start from the writable layout (a cleared depth) and from read-only.
                for (int start_writable = 0; start_writable < 2; ++start_writable) {
                    // The depth as the pass before left it: written (the write view, writable)
                    // or read-only; the first draw's change from it counts too.
                    int changes_today = 0, changes_on = 0;
                    const Att start{start_writable != 0,
                                    PL::DepthAttachmentLayout(start_writable != 0, stencil, test)};
                    Att prev_today = start, prev_on = start;
                    const bool have_prev = true;
                    PL::DepthLayout layout_on = start.layout;
                    for (int i = 0; i < len; ++i) {
                        const bool writes = (seq >> (2 * i)) & 1;
                        const bool sampled = (seq >> (2 * i + 1)) & 1;
                        const Att today{writes, Today(writes, stencil, test)};
                        const bool now = layout_on == PL::DepthAttachmentLayout(true, stencil, test);
                        const bool keep = PL::KeepDepthWritable(writes, true, sampled, now);
                        const Att on{writes || keep,
                                     PL::DepthAttachmentLayout(writes || keep, stencil, test)};
                        if (writes || sampled) {
                            EXPECT_TRUE(on == today);
                        }
                        if (have_prev) {
                            changes_today += !(today == prev_today);
                            changes_on += !(on == prev_on);
                        }
                        prev_today = today;
                        prev_on = on;
                        layout_on = on.layout;
                    }
                    EXPECT_LE(changes_on, changes_today) << cfg << " " << len << " " << seq;
                    fewer += changes_on < changes_today;
                }
            }
        }
    }
    EXPECT_GT(fewer, 0);
}

TEST(VrPerfSwitch, FirstValueIsNoChangeThenTogglesAre) {
    PL::Switch s{};
    EXPECT_FALSE(s.Update(true)); // the first frame: no toggle
    EXPECT_FALSE(s.Update(true));
    EXPECT_TRUE(s.Update(false));
    EXPECT_FALSE(s.Update(false));
    EXPECT_TRUE(s.Update(true));
    EXPECT_TRUE(s.value);
}

TEST(VrPerfCompare, EqualAndDifferent) {
    const std::vector<uint8_t> a = {1, 2, 3, 4, 5};
    auto b = a;
    auto c = PL::CompareBytes(a, b);
    EXPECT_EQ(c.bytes, 0u);
    b[3] = 9;
    b[4] = 9;
    c = PL::CompareBytes(a, b);
    EXPECT_EQ(c.bytes, 2u);
    EXPECT_EQ(c.first, 3u);
    const std::vector<uint8_t> shorter = {1, 2, 3};
    c = PL::CompareBytes(a, shorter);
    EXPECT_EQ(c.bytes, 2u);
    EXPECT_EQ(c.first, 3u);
}

} // namespace
