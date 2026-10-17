// SPDX-License-Identifier: GPL-2.0-or-later

// The command processor's per-draw shortcuts (video_core/renderer_vulkan/vk_perf_memo.h): the
// switches and their epoch, the spec projection of V# / T# / S# / FMASK sharps (what
// Shader::StageSpecialization reads, without addresses and sizes: equal projections must mean an
// equal specialization, a different spec-relevant field must always change the projection), the
// stage projection's grouping, the byte-keyed memo (render-target descriptions), and the bind
// memo's key and table (a stage's image bindings: byte-equal keys only, both epochs current, a
// slot replaced by a new key, an entry being filled never found), and the copy memo's table and
// protocol (the copy shader HLE's buffers: found only with every epoch read before the full pass
// that made the entry; a model of the write tracking shows every hit is a batch whose full pass
// would have nothing to do, also with writes racing the pass).

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_perf_memo.h"

namespace {

using namespace Vulkan::PerfMemo;

AmdGpu::Buffer MakeBuffer() {
    AmdGpu::Buffer b{};
    b.base_address = 0x12345600;
    b.stride = 16;
    b.num_records = 100;
    b.dst_sel_x = 4;
    b.dst_sel_y = 5;
    b.dst_sel_z = 6;
    b.dst_sel_w = 7;
    b.num_format = 7;
    b.data_format = 14;
    return b;
}

AmdGpu::Image MakeImage() {
    AmdGpu::Image t{};
    t.base_address = 0x4567;
    t.data_format = 10;
    t.num_format = 0;
    t.width = 1023;
    t.height = 511;
    t.pitch = 1023;
    t.dst_sel_x = 4;
    t.dst_sel_y = 5;
    t.dst_sel_z = 6;
    t.dst_sel_w = 7;
    t.base_level = 0;
    t.last_level = 9;
    t.tiling_index = 13;
    t.type = 9; // Color2D
    return t;
}

template <typename T, typename F>
Projection Project(const T& sharp, F project) {
    Projection p;
    p.Clear();
    project(sharp, p);
    return p;
}

Projection PB(const AmdGpu::Buffer& b) {
    return Project(b, [](const auto& s, Projection& p) { ProjectBuffer(s, p); });
}
Projection PI(const AmdGpu::Image& t) {
    return Project(t, [](const auto& s, Projection& p) { ProjectImage(s, p); });
}
Projection PF(const AmdGpu::Image& t) {
    return Project(t, [](const auto& s, Projection& p) { ProjectFmask(s, p); });
}
Projection PS(const AmdGpu::Sampler& s) {
    return Project(s, [](const auto& x, Projection& p) { ProjectSampler(x, p); });
}

TEST(PerfMemoProjection, BufferIgnoresAddressAndSize) {
    const auto a = MakeBuffer();
    auto b = a;
    b.base_address = 0xABCDE000;
    b.num_records = 7;
    EXPECT_TRUE(PB(a) == PB(b)); // another buffer of the same layout: the same specialization
}

TEST(PerfMemoProjection, BufferKeepsEveryFieldTheSpecReads) {
    const auto a = MakeBuffer();
    const auto base = PB(a);
    std::vector<AmdGpu::Buffer> changed;
    auto c = a;
    c.num_records = 0; // bound / unbound
    changed.push_back(c);
    c = a, c.stride = 32, changed.push_back(c);
    c = a, c.swizzle_enable = 1, changed.push_back(c);
    c = a, c.cache_swizzle = 1, changed.push_back(c);
    c = a, c.dst_sel_x = 0, changed.push_back(c);
    c = a, c.dst_sel_y = 0, changed.push_back(c);
    c = a, c.dst_sel_z = 0, changed.push_back(c);
    c = a, c.dst_sel_w = 1, changed.push_back(c);
    c = a, c.num_format = 4, changed.push_back(c);
    c = a, c.data_format = 4, changed.push_back(c);
    c = a, c.element_size = 2, changed.push_back(c);
    c = a, c.index_stride = 3, changed.push_back(c);
    c = a, c.add_tid_enable = 1, changed.push_back(c);
    c = a, c.type = 1, changed.push_back(c); // not a V# (GetSharp's Null)
    for (size_t i = 0; i < changed.size(); ++i) {
        EXPECT_FALSE(PB(changed[i]) == base) << "field change " << i;
    }
}

TEST(PerfMemoProjection, ImageIgnoresAddressSizePitchAndTiling) {
    const auto a = MakeImage();
    auto b = a;
    b.base_address = 0x9999;
    b.width = 255;
    b.height = 127;
    b.pitch = 255;
    b.tiling_index = 8;
    b.depth = 3;
    b.base_array = 1;
    b.last_array = 2;
    b.min_lod = 5;
    b.perf_modulation = 2;
    EXPECT_TRUE(PI(a) == PI(b));
}

TEST(PerfMemoProjection, ImageKeepsEveryFieldTheSpecReads) {
    const auto a = MakeImage();
    const auto base = PI(a);
    std::vector<AmdGpu::Image> changed;
    auto c = a;
    c.base_address = 0; // unbound
    changed.push_back(c);
    c = a, c.type = 11, changed.push_back(c); // cube
    c = a, c.type = 13, changed.push_back(c); // 2D array
    c = a, c.data_format = 4, changed.push_back(c);
    c = a, c.num_format = 9, changed.push_back(c); // sRGB
    c = a, c.num_format = 5, changed.push_back(c); // integer
    c = a, c.dst_sel_x = 1, changed.push_back(c);
    c = a, c.dst_sel_y = 1, changed.push_back(c);
    c = a, c.dst_sel_z = 1, changed.push_back(c);
    c = a, c.dst_sel_w = 1, changed.push_back(c);
    c = a, c.base_level = 1, changed.push_back(c); // mip-array bindings
    c = a, c.last_level = 3, changed.push_back(c);
    for (size_t i = 0; i < changed.size(); ++i) {
        EXPECT_FALSE(PI(changed[i]) == base) << "field change " << i;
    }
}

TEST(PerfMemoProjection, FmaskKeepsSizeAndBinding) {
    const auto a = MakeImage();
    auto b = a;
    b.base_address = 0x7777;
    EXPECT_TRUE(PF(a) == PF(b));
    b = a, b.width = 100;
    EXPECT_FALSE(PF(a) == PF(b));
    b = a, b.height = 100;
    EXPECT_FALSE(PF(a) == PF(b));
    b = a, b.base_address = 0;
    EXPECT_FALSE(PF(a) == PF(b));
}

TEST(PerfMemoProjection, SamplerKeepsOnlyWhatTheSpecReads) {
    AmdGpu::Sampler a{};
    a.raw0 = 0x0000'0000'0000'0012ull; // some clamp modes
    a.raw1 = 0x0000'0000'0010'0000ull;
    auto b = a;
    b.lod_bias.Assign(100); // filters and lod are not part of the shader
    b.max_aniso.Assign(AmdGpu::AnisoRatio::Sixteen);
    EXPECT_TRUE(PS(a) == PS(b));
    b = a;
    b.force_unnormalized.Assign(1);
    EXPECT_FALSE(PS(a) == PS(b));
    b = a;
    b.force_degamma.Assign(1);
    EXPECT_FALSE(PS(a) == PS(b));
    AmdGpu::Sampler zero{};
    zero.raw0 = 0;
    zero.raw1 = 0;
    EXPECT_FALSE(PS(a) == PS(zero)); // unset
}

TEST(PerfMemoProjection, OverflowIsNeverEqualOrStored) {
    Projection a;
    a.Clear();
    for (u32 i = 0; i < Projection::Capacity + 1; ++i) {
        a.Push(i);
    }
    EXPECT_FALSE(a.Usable());
    EXPECT_FALSE(a == a);
    std::vector<u32> stored(Projection::Capacity);
    EXPECT_FALSE(a.Equals(stored));
}

TEST(PerfMemoProjection, StoredCopyCompares) {
    Projection a;
    a.Clear();
    a.Push(1);
    a.Push(2);
    std::vector<u32> stored;
    a.StoreTo(stored);
    EXPECT_TRUE(a.Equals(stored));
    stored[1] = 3;
    EXPECT_FALSE(a.Equals(stored));
    stored = {1, 2, 0};
    EXPECT_FALSE(a.Equals(stored)); // different length
    Projection empty;
    empty.Clear();
    std::vector<u32> none;
    EXPECT_TRUE(empty.Equals(none));
}

// A stand-in for Shader::Info and its resource lists: GetSharp(info) as the real descriptors.
struct FakeInfo;
struct FakeBufferDesc {
    AmdGpu::Buffer sharp;
    AmdGpu::Buffer GetSharp(const FakeInfo&) const {
        return sharp;
    }
};
struct FakeImageDesc {
    AmdGpu::Image sharp;
    AmdGpu::Image GetSharp(const FakeInfo&) const {
        return sharp;
    }
};
struct FakeSamplerDesc {
    AmdGpu::Sampler sharp;
    AmdGpu::Sampler GetSharp(const FakeInfo&) const {
        return sharp;
    }
};
struct FakeInfo {
    std::vector<FakeBufferDesc> buffers;
    std::vector<FakeImageDesc> images;
    std::vector<FakeImageDesc> fmasks;
    std::vector<FakeSamplerDesc> samplers;
};

TEST(PerfMemoProjection, StageGroupsStayApart) {
    // One buffer + one image against the same words moved between the groups: never equal.
    FakeInfo x;
    x.buffers.push_back({MakeBuffer()});
    x.images.push_back({MakeImage()});
    Projection px, py;
    ProjectStage(x, static_cast<const std::vector<FakeBufferDesc>*>(nullptr), px);
    FakeInfo y = x;
    y.images.clear();
    ProjectStage(y, static_cast<const std::vector<FakeBufferDesc>*>(nullptr), py);
    EXPECT_FALSE(px == py);
    // The attributes are a group of their own: the same V# as an attribute or as a buffer differs.
    FakeInfo z;
    z.images = x.images;
    Projection pz;
    ProjectStage(z, &x.buffers, pz);
    EXPECT_FALSE(px == pz);
    // Equal inputs, equal projection; a buffer at another address, still equal.
    Projection px2;
    FakeInfo x2 = x;
    x2.buffers[0].sharp.base_address = 0x1000;
    ProjectStage(x2, static_cast<const std::vector<FakeBufferDesc>*>(nullptr), px2);
    EXPECT_TRUE(px == px2);
    x2.images[0].sharp.data_format = 1;
    ProjectStage(x2, static_cast<const std::vector<FakeBufferDesc>*>(nullptr), px2);
    EXPECT_FALSE(px == px2);
}

TEST(PerfMemoSwitches, ToggleBumpsTheEpoch) {
    SwitchState s;
    const u32 e0 = Epoch();
    EXPECT_FALSE(s.Apply(s.Get())); // nothing changed
    EXPECT_EQ(Epoch(), e0);
    Switches next = s.Get();
    next.spec_memo = !next.spec_memo;
    EXPECT_TRUE(s.Apply(next));
    EXPECT_NE(Epoch(), e0);
    const u32 e1 = Epoch();
    next.rt_memo = !next.rt_memo;
    EXPECT_TRUE(s.Apply(next));
    EXPECT_NE(Epoch(), e1);
}

TEST(PerfMemoSwitches, EpochIsNeverZero) {
    g_epoch.store(0xFFFFFFFFu);
    BumpEpoch();
    EXPECT_NE(Epoch(), 0u);
    g_epoch.store(1);
}

TEST(PerfMemoBytes, StoreFindAndInvalidate) {
    struct Regs {
        u32 a, b, c;
    };
    BytesMemo<sizeof(Regs) + sizeof(u32), int> memo;
    KeyBytes<sizeof(Regs) + sizeof(u32)> k;
    k.Add(Regs{1, 2, 3}).Add(u32{4});
    ASSERT_TRUE(k.Complete());
    EXPECT_EQ(memo.Find(k, 7), nullptr); // empty
    memo.Store(k, 42, 7);
    ASSERT_NE(memo.Find(k, 7), nullptr);
    EXPECT_EQ(*memo.Find(k, 7), 42);
    EXPECT_EQ(memo.Find(k, 8), nullptr); // another epoch: a toggle or a cache change
    KeyBytes<sizeof(Regs) + sizeof(u32)> k2;
    k2.Add(Regs{1, 2, 3}).Add(u32{5}); // one register differs
    EXPECT_EQ(memo.Find(k2, 7), nullptr);
    KeyBytes<sizeof(Regs) + sizeof(u32)> shortk;
    shortk.Add(Regs{1, 2, 3}); // a key of the wrong size is never found nor stored
    EXPECT_FALSE(shortk.Complete());
    EXPECT_EQ(memo.Find(shortk, 7), nullptr);
    memo.Store(shortk, 9, 7);
    EXPECT_EQ(*memo.Find(k, 7), 42);
    BytesMemo<4, int> zero_epoch;
    KeyBytes<4> kz;
    kz.Add(u32{0});
    EXPECT_EQ(zero_epoch.Find(kz, 0), nullptr); // epoch 0 = empty, never a hit
}

// ---------------------------------------------------------------------------------------------
// perf_bind_memo

BindKey KeyOf(const std::vector<std::pair<AmdGpu::Image, u64>>& images) {
    BindKey k;
    k.Clear();
    for (const auto& [t, attrs] : images) {
        k.Add(t, attrs);
    }
    return k;
}

TEST(PerfMemoBind, AttrsAreDistinct) {
    const u64 none = BindAttrs(false, false, false, 0, 0);
    EXPECT_NE(BindAttrs(true, false, false, 0, 0), none);
    EXPECT_NE(BindAttrs(false, true, false, 0, 0), none);
    EXPECT_NE(BindAttrs(false, false, true, 0, 0), none);
    EXPECT_NE(BindAttrs(false, false, false, 1, 0), none);
    EXPECT_NE(BindAttrs(false, false, false, 2, 0), BindAttrs(false, false, false, 1, 0));
    EXPECT_NE(BindAttrs(false, false, false, 0, 3), none);
    EXPECT_NE(BindAttrs(false, false, false, 0, 3), BindAttrs(false, false, false, 0, 4));
    // Each attribute in its own bits: no two single changes give the same word.
    EXPECT_NE(BindAttrs(true, false, false, 0, 0), BindAttrs(false, true, false, 0, 0));
    EXPECT_NE(BindAttrs(false, false, false, 1, 0), BindAttrs(true, false, false, 0, 0));
    EXPECT_NE(BindAttrs(false, false, false, 0, 1), BindAttrs(true, false, false, 0, 0));
}

TEST(PerfMemoBind, KeyEqualOnlyForTheSameBytes) {
    const AmdGpu::Image a = MakeImage();
    AmdGpu::Image b = MakeImage();
    b.base_address = 0x9999; // another texture of the same layout: another key
    const u64 at = BindAttrs(false, false, false, 0, 0);
    const BindKey k1 = KeyOf({{a, at}, {b, at}});
    const BindKey k2 = KeyOf({{a, at}, {b, at}});
    EXPECT_TRUE(k1 == k2);
    EXPECT_EQ(k1.Hash(), k2.Hash());
    EXPECT_FALSE(k1 == KeyOf({{b, at}, {a, at}})); // the order is the binding order
    EXPECT_FALSE(k1 == KeyOf({{a, at}}));           // fewer images
    EXPECT_FALSE(k1 == KeyOf({{a, at}, {b, BindAttrs(false, true, false, 0, 0)}})); // array
    EXPECT_FALSE(k1 == KeyOf({{a, at}, {b, BindAttrs(true, false, false, 0, 0)}})); // depth
    EXPECT_FALSE(k1 == KeyOf({{a, at}, {b, BindAttrs(false, false, false, 1, 2)}})); // mips
    // Any field of the T# (address, size, format, tiling, levels, swizzle) changes the key.
    const auto changed = [&](auto&& change) {
        AmdGpu::Image c = b;
        change(c);
        return !(k1 == KeyOf({{a, at}, {c, at}}));
    };
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.width = 511; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.height = 255; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.pitch = 2047; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.data_format = 11; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.num_format = 1; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.tiling_index = 14; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.base_level = 1; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.last_level = 3; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.dst_sel_x = 0; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.type = 10; }));
    EXPECT_TRUE(changed([](AmdGpu::Image& c) { c.base_address = 0x999A; }));
}

TEST(PerfMemoBind, KeyOverflowIsNeverUsable) {
    std::vector<std::pair<AmdGpu::Image, u64>> many;
    for (u32 i = 0; i < BindKey::MaxImages + 1; ++i) {
        AmdGpu::Image t = MakeImage();
        t.base_address = 0x1000 + i;
        many.emplace_back(t, 0);
    }
    const BindKey k = KeyOf(many);
    EXPECT_FALSE(k.Usable());
    EXPECT_FALSE(k == k); // an overflowed key equals nothing, not even itself
    many.pop_back();
    const BindKey full = KeyOf(many);
    EXPECT_TRUE(full.Usable());
    EXPECT_TRUE(full == KeyOf(many));
}

struct FakeBinding {
    u32 image{};
};

TEST(PerfMemoBind, TableFindsOnlyCurrentEpochsAndEqualKeys) {
    BindTable<FakeBinding, 4> table;
    const u64 at = BindAttrs(false, false, false, 0, 0);
    const BindKey k = KeyOf({{MakeImage(), at}});
    const u64 h = k.Hash();
    EXPECT_EQ(table.Find(k, h, 5, 7), nullptr); // empty
    auto& e = table.Prepare(k, h);
    e.bindings[0].image = 42;
    e.num_bindings = 1;
    e.array_sizes[0] = 1;
    EXPECT_EQ(table.Find(k, h, 5, 7), nullptr); // being filled: not found before Commit
    BindTable<FakeBinding, 4>::Commit(e, 5, 7);
    const auto* f = table.Find(k, h, 5, 7);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->num_bindings, 1u);
    EXPECT_EQ(f->bindings[0].image, 42u);
    EXPECT_EQ(table.Find(k, h, 6, 7), nullptr); // an image registered / unregistered since
    EXPECT_EQ(table.Find(k, h, 5, 8), nullptr); // a switch toggled, a program cache change
    AmdGpu::Image other = MakeImage();
    other.base_address = 0x8888;
    const BindKey k2 = KeyOf({{other, at}});
    EXPECT_EQ(table.Find(k2, k2.Hash(), 5, 7), nullptr); // another key
    EXPECT_EQ(table.Find(k2, h, 5, 7), nullptr);         // another key with a forged hash
}

TEST(PerfMemoBind, NewKeyInTheSlotReplacesTheOld) {
    BindTable<FakeBinding, 1> table; // 2 slots: collisions are certain
    const u64 at = BindAttrs(false, false, false, 0, 0);
    std::vector<BindKey> keys;
    for (u32 i = 0; i < 8; ++i) {
        AmdGpu::Image t = MakeImage();
        t.base_address = 0x2000 + i * 0x100;
        keys.push_back(KeyOf({{t, at}}));
    }
    for (u32 i = 0; i < keys.size(); ++i) {
        auto& e = table.Prepare(keys[i], keys[i].Hash());
        e.bindings[0].image = i;
        e.num_bindings = 1;
        BindTable<FakeBinding, 1>::Commit(e, 1, 1);
        // The key just stored is found with its own result; every hit is the right key's.
        const auto* f = table.Find(keys[i], keys[i].Hash(), 1, 1);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->bindings[0].image, i);
        for (u32 j = 0; j < i; ++j) {
            const auto* g = table.Find(keys[j], keys[j].Hash(), 1, 1);
            if (g != nullptr) {
                EXPECT_EQ(g->bindings[0].image, j);
            }
        }
    }
}

TEST(PerfMemoBind, SwitchToggleBumpsTheEpoch) {
    SwitchState s;
    Switches next = s.Get();
    const u32 e0 = Epoch();
    next.bind_memo = !next.bind_memo;
    EXPECT_TRUE(s.Apply(next));
    EXPECT_NE(Epoch(), e0);
    EXPECT_FALSE(s.Apply(next));
}

// ---------------------------------------------------------------------------------------------
// perf_copy_memo

CopyEntry MakeCopyEntry(const CopyKey& key, u64 cache, u64 src_track, u64 dst_track, u32 memo) {
    return CopyEntry{.key = key,
                     .cache_epoch = cache,
                     .src_track = src_track,
                     .dst_track = dst_track,
                     .memo_epoch = memo,
                     .src_buffer = 3,
                     .dst_buffer = 4,
                     .src_offset = 0x100,
                     .dst_offset = 0x2000};
}

TEST(PerfMemoCopy, KeyEqualOnlyForTheSameRanges) {
    const CopyKey k{.src_addr = 0x1000, .dst_addr = 0x9000, .src_size = 0x400, .dst_size = 0x800};
    CopyKey same = k;
    EXPECT_TRUE(k == same);
    EXPECT_EQ(k.Hash(), same.Hash());
    for (int field = 0; field < 4; ++field) {
        CopyKey c = k;
        switch (field) {
        case 0: c.src_addr += 0x10; break;
        case 1: c.dst_addr += 0x10; break;
        case 2: c.src_size += 1; break;
        case 3: c.dst_size += 1; break;
        }
        EXPECT_FALSE(k == c) << field;
    }
    // Source and destination swapped: another batch.
    const CopyKey swapped{.src_addr = 0x9000, .dst_addr = 0x1000, .src_size = 0x800,
                          .dst_size = 0x400};
    EXPECT_FALSE(k == swapped);
    EXPECT_NE(k.Hash(), swapped.Hash());
}

TEST(PerfMemoCopy, TableFindsOnlyWithEveryEpochCurrent) {
    CopyTable<4> table;
    const CopyKey k{.src_addr = 0x1000, .dst_addr = 0x9000, .src_size = 0x400, .dst_size = 0x800};
    EXPECT_EQ(table.Find(k, 1, 10, 20, 7), nullptr); // empty
    EXPECT_TRUE(table.Store(MakeCopyEntry(k, 1, 10, 20, 7)));
    const CopyEntry* e = table.Find(k, 1, 10, 20, 7);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->src_buffer, 3u);
    EXPECT_EQ(e->dst_buffer, 4u);
    EXPECT_EQ(e->src_offset, 0x100u);
    EXPECT_EQ(e->dst_offset, 0x2000u);
    EXPECT_EQ(table.Find(k, 2, 10, 20, 7), nullptr); // a buffer created / removed, a range dropped
    EXPECT_EQ(table.Find(k, 1, 11, 20, 7), nullptr); // a source page changed state
    EXPECT_EQ(table.Find(k, 1, 10, 21, 7), nullptr); // a destination page changed state
    EXPECT_EQ(table.Find(k, 1, 10, 20, 8), nullptr); // a switch toggled
    EXPECT_EQ(table.Find(k, 1, 0, 20, 7), nullptr);  // the source is not tracked now
    EXPECT_EQ(table.Find(k, 1, 10, 0, 7), nullptr);  // the destination is not tracked now
    CopyKey other = k;
    other.dst_size = 0x804;
    EXPECT_EQ(table.Find(other, 1, 10, 20, 7), nullptr);
}

TEST(PerfMemoCopy, UntrackedOrEmptyEpochIsNeverStored) {
    CopyTable<4> table;
    const CopyKey k{.src_addr = 0x1000, .dst_addr = 0x9000, .src_size = 0x400, .dst_size = 0x800};
    EXPECT_FALSE(table.Store(MakeCopyEntry(k, 1, 0, 20, 7)));
    EXPECT_FALSE(table.Store(MakeCopyEntry(k, 1, 10, 0, 7)));
    EXPECT_FALSE(table.Store(MakeCopyEntry(k, 1, 10, 20, 0)));
    // A zero lookup epoch never finds a stored zero (nothing was stored).
    EXPECT_EQ(table.Find(k, 1, 0, 20, 7), nullptr);
    EXPECT_EQ(table.Find(k, 1, 10, 20, 0), nullptr);
}

TEST(PerfMemoCopy, NewKeyInTheSlotReplacesTheOld) {
    CopyTable<1> table; // 2 slots: collisions are certain
    std::vector<CopyKey> keys;
    for (u32 i = 0; i < 8; ++i) {
        keys.push_back(CopyKey{.src_addr = 0x10000u + i * 0x1000u,
                               .dst_addr = 0x90000u + i * 0x1000u,
                               .src_size = 0x100,
                               .dst_size = 0x100});
    }
    for (u32 i = 0; i < keys.size(); ++i) {
        CopyEntry e = MakeCopyEntry(keys[i], 1, 1, 1, 1);
        e.src_offset = i;
        ASSERT_TRUE(table.Store(e));
        const CopyEntry* f = table.Find(keys[i], 1, 1, 1, 1);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->src_offset, i);
        for (u32 j = 0; j < i; ++j) {
            const CopyEntry* g = table.Find(keys[j], 1, 1, 1, 1);
            if (g != nullptr) {
                EXPECT_EQ(g->src_offset, j); // every hit is the right key's
            }
        }
    }
}

// A model of the buffer cache's write tracking for one batch: the source range's pages may be
// CPU-modified (a full pass uploads and clears them), the destination's pages are marked
// GPU-modified by the pass (and dropped by a download); every state change bumps that range's
// epoch, as RegionManager does; a buffer created or removed bumps the cache epoch. The memo
// protocol of Rasterizer::ObtainCopyBuffers: epochs read before; a hit skips the pass; a miss runs
// it and stores the entry with the epochs read before it. Every hit must be a batch whose full
// pass would have had nothing to do.
struct TrackModel {
    u64 src_epoch{1};
    u64 dst_epoch{1};
    u64 cache_epoch{1};
    bool src_cpu_dirty{true};
    bool dst_cpu_dirty{false};
    bool dst_gpu_marked{false};
    u32 buffer{1};

    void CpuWriteSrc() {
        if (!src_cpu_dirty) {
            src_cpu_dirty = true;
            ++src_epoch; // the write fault marks the page (only the first write faults)
        }
    }
    void CpuWriteDst() {
        if (!dst_cpu_dirty) {
            dst_cpu_dirty = true;
            ++dst_epoch;
        }
    }
    void Download() {
        if (dst_gpu_marked) {
            dst_gpu_marked = false;
            ++dst_epoch;
        }
        ++cache_epoch; // the GPU-modified range set dropped the range
    }
    void Rebuffer() {
        ++buffer;
        ++cache_epoch;
    }
    bool PassIsIdle() const {
        return !src_cpu_dirty && !dst_cpu_dirty && dst_gpu_marked;
    }
    // The full pass; a write racing it may land after its source sync (on_race).
    template <typename Race>
    void FullPass(Race&& on_race) {
        if (src_cpu_dirty) {
            src_cpu_dirty = false;
            ++src_epoch;
        }
        on_race();
        if (dst_cpu_dirty) {
            dst_cpu_dirty = false;
            ++dst_epoch;
        }
        if (!dst_gpu_marked) {
            dst_gpu_marked = true;
            ++dst_epoch;
        }
    }
};

TEST(PerfMemoCopy, ProtocolHitsOnlyWhenThePassWouldBeIdle) {
    std::mt19937 rng{1234};
    TrackModel m;
    CopyTable<4> table;
    const CopyKey k{.src_addr = 0x1000, .dst_addr = 0x9000, .src_size = 0x400, .dst_size = 0x800};
    u32 hits = 0;
    u32 misses = 0;
    u32 memo_epoch = 1;
    for (int step = 0; step < 20000; ++step) {
        // Events between two uses of the batch.
        const u32 r = rng() % 100;
        if (r < 30) {
            m.CpuWriteSrc();
        } else if (r < 33) {
            m.CpuWriteDst();
        } else if (r < 35) {
            m.Download();
        } else if (r < 36) {
            m.Rebuffer();
        } else if (r < 37) {
            ++memo_epoch; // a switch toggled
        }
        const u64 cache = m.cache_epoch;
        const u64 src = m.src_epoch;
        const u64 dst = m.dst_epoch;
        const CopyEntry* e = table.Find(k, cache, src, dst, memo_epoch);
        if (e != nullptr) {
            ++hits;
            ASSERT_TRUE(m.PassIsIdle()) << "step " << step;
            ASSERT_EQ(e->src_buffer, m.buffer) << "step " << step;
            continue;
        }
        ++misses;
        const bool race = rng() % 10 == 0;
        m.FullPass([&] {
            if (race) {
                m.CpuWriteSrc(); // lands after the source sync: the pass did not upload it
            }
        });
        CopyEntry stored = MakeCopyEntry(k, cache, src, dst, memo_epoch);
        stored.src_buffer = m.buffer;
        table.Store(stored);
    }
    EXPECT_GT(hits, 1000u);
    EXPECT_GT(misses, 1000u);
}

TEST(PerfMemoCopy, EntryFromAPassThatDidWorkIsNotUsedUntilAnIdlePass) {
    TrackModel m;
    CopyTable<4> table;
    const CopyKey k{.src_addr = 0x1000, .dst_addr = 0x9000, .src_size = 0x400, .dst_size = 0x800};
    // Pass 1 uploads and marks: its entry (epochs before) is stale at once.
    u64 src = m.src_epoch, dst = m.dst_epoch;
    m.FullPass([] {});
    table.Store(MakeCopyEntry(k, m.cache_epoch, src, dst, 1));
    EXPECT_EQ(table.Find(k, m.cache_epoch, m.src_epoch, m.dst_epoch, 1), nullptr);
    // Pass 2 finds everything in sync: its entry is used from the next batch on.
    src = m.src_epoch;
    dst = m.dst_epoch;
    m.FullPass([] {});
    EXPECT_EQ(src, m.src_epoch);
    EXPECT_EQ(dst, m.dst_epoch);
    table.Store(MakeCopyEntry(k, m.cache_epoch, src, dst, 1));
    EXPECT_NE(table.Find(k, m.cache_epoch, m.src_epoch, m.dst_epoch, 1), nullptr);
    // A CPU write to the source: stale again.
    m.CpuWriteSrc();
    EXPECT_EQ(table.Find(k, m.cache_epoch, m.src_epoch, m.dst_epoch, 1), nullptr);
}

TEST(PerfMemoCopy, SwitchToggleBumpsTheEpoch) {
    SwitchState s;
    Switches next = s.Get();
    const u32 e0 = Epoch();
    next.copy_memo = !next.copy_memo;
    EXPECT_TRUE(s.Apply(next));
    EXPECT_NE(Epoch(), e0);
    EXPECT_FALSE(s.Apply(next));
}

TEST(PerfMemoCounters, FirstTwentyMismatchesAreDetailed) {
    Counters c;
    int detailed = 0;
    for (int i = 0; i < 30; ++i) {
        detailed += c.NoteMismatch() ? 1 : 0;
    }
    EXPECT_EQ(detailed, 20);
    EXPECT_EQ(c.mismatches, 30u);
    c.Reset();
    EXPECT_EQ(c.mismatches, 0u);
    EXPECT_EQ(c.total_mismatches, 30u);
    EXPECT_FALSE(c.NoteMismatch());
    c.hits = 3;
    c.misses = 1;
    EXPECT_NEAR(c.HitRate(), 0.75, 1e-12);
}

} // namespace
