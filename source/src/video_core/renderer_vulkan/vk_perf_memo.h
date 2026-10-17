// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Per-draw CPU shortcuts of the command processor (Bloodborne VR, 2026-10-09), the pure parts:
//  - the switches (vr.ini perf_spec_memo / perf_rt_memo / perf_bind_memo / perf_copy_memo,
//    VR_TUNE_FILE perf.spec_memo / perf.rt_memo / perf.bind_memo / perf.copy_memo) with an
//    epoch that every toggle bumps, so a memo filled before a toggle is never used after it;
//  - the "spec projection": the fields of the bound sharps (V#, T#, S#, FMASK) that
//    Shader::StageSpecialization reads, without their addresses and sizes. Two draws whose
//    projections, runtime infos and binding starts are equal build equal specializations (the
//    same program permutation), so the pipeline cache can skip building and comparing one
//    (vk_pipeline_cache.cpp GetProgram);
//  - a byte-compared memo of a pure function (the render targets' image descriptions,
//    vk_rasterizer.cpp PrepareRenderState);
//  - the bind memo (perf_bind_memo, vr.ini / VR_TUNE_FILE perf.bind_memo): the key of one shader
//    stage's image bindings (every T# as GetSharp returns it, with the descriptor attributes
//    BindTextures reads) and a direct-mapped table of the bindings it resolved to, valid only for
//    the texture cache's registry epoch and the memo epoch it was made in
//    (vk_rasterizer.cpp BindTextures);
//  - the copy memo (perf_copy_memo, vr.ini / VR_TUNE_FILE perf.copy_memo): the buffers and offsets
//    the copy shader's HLE obtained for one batch's source and destination ranges, valid only
//    while the buffer cache's epoch and both ranges' write-tracking epochs are the ones read
//    before the full pass that made it (vk_rasterizer.cpp ObtainCopyBuffers).
// Header-only and free of emulator state: tests/test_perf_memo.cpp.

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

#include "common/types.h"

namespace Vulkan::PerfMemo {

// ---------------------------------------------------------------------------------------------
// Switches

// The defaults are the vr.ini defaults (VR::Settings): on since 2026-10-09, the user asked for
// them on; copy_memo off (it never hit in the measured scenes). PerfRefresh applies the settings
// within 250 ms of the start.
struct Switches {
    bool spec_memo{true}; // perf_spec_memo: the program permutation memo (GetProgram)
    bool rt_memo{true};   // perf_rt_memo: the render targets' description memo (PrepareRenderState)
    bool bind_memo{true}; // perf_bind_memo: a stage's image bindings by their T#s (BindTextures)
    bool copy_memo{false}; // perf_copy_memo: the copy shader HLE's buffers (ObtainCopyBuffers)

    bool operator==(const Switches&) const = default;
};

/// Global epoch of every memo here: a memo entry is valid only while its stored epoch equals the
/// current one. Bumped by a switch toggle and by any change of the program cache's permutations.
/// Never 0 (0 marks an empty entry).
inline std::atomic<u32> g_epoch{1};

inline u32 Epoch() noexcept {
    return g_epoch.load(std::memory_order_relaxed);
}

inline void BumpEpoch() noexcept {
    u32 e = g_epoch.load(std::memory_order_relaxed) + 1;
    if (e == 0) {
        e = 1;
    }
    g_epoch.store(e, std::memory_order_relaxed);
}

/// The switches in force. Apply() replaces them; any difference bumps the epoch (returns true).
class SwitchState {
public:
    Switches Get() const noexcept {
        const auto bits = current.load(std::memory_order_relaxed);
        return {bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8)};
    }
    bool Apply(const Switches& next) noexcept {
        const u32 bits = u32(next.spec_memo) | (u32(next.rt_memo) << 1) |
                         (u32(next.bind_memo) << 2) | (u32(next.copy_memo) << 3);
        if (current.exchange(bits, std::memory_order_relaxed) == bits)
            return false;
        BumpEpoch();
        return true;
    }

private:
    std::atomic<u32> current{7};
};

inline SwitchState g_switches{};

/// VR_PERF_CHECK=1 (tests only): every memo hit also runs the original path next to it, compares
/// the two results and uses the original's (counted, the first mismatches logged in detail).
/// Read once at start-up (a plain load per draw, no function-local static's guard).
inline const bool g_check_mode = [] {
    const char* v = std::getenv("VR_PERF_CHECK");
    return v != nullptr && v[0] == '1';
}();
inline bool CheckMode() noexcept {
    return g_check_mode;
}

// ---------------------------------------------------------------------------------------------
// Spec projection

/// The projected words of one stage's bound resources. Fixed capacity (no allocation per draw);
/// a stage with more words than fit is never memoized (overflow).
struct Projection {
    static constexpr u32 Capacity = 384;
    std::array<u32, Capacity> words;
    u32 count{0};
    bool overflow{false};

    void Clear() noexcept {
        count = 0;
        overflow = false;
    }
    void Push(u32 w) noexcept {
        if (count < Capacity) {
            words[count++] = w;
        } else {
            overflow = true;
        }
    }
    bool Usable() const noexcept {
        return !overflow;
    }
    bool operator==(const Projection& o) const noexcept {
        return !overflow && !o.overflow && count == o.count &&
               std::memcmp(words.data(), o.words.data(), count * sizeof(u32)) == 0;
    }
    /// Equal to a stored copy (StoreTo).
    template <typename Vec>
    bool Equals(const Vec& stored) const noexcept {
        return !overflow && stored.size() == count &&
               (count == 0 || std::memcmp(words.data(), stored.data(), count * sizeof(u32)) == 0);
    }
    template <typename Vec>
    void StoreTo(Vec& stored) const {
        stored.assign(words.begin(), words.begin() + count);
    }
};

/// V# (AmdGpu::Buffer): StageSpecialization reads whether it is bound (num_records != 0), its
/// stride, swizzle_enable, data/number format, destination swizzle, number conversion (from the
/// formats), index stride and element size. Kept as well: cache_swizzle, add_tid_enable and the
/// type bits (a superset is safe: it can only cost a memo hit, never give a wrong one).
template <typename Buffer, typename Output>
inline void ProjectBuffer(const Buffer& b, Output& out) noexcept {
    out.Push(u32(b.stride) | (u32(b.cache_swizzle) << 14) | (u32(b.swizzle_enable) << 15) |
             (u32(b.num_records != 0) << 16));
    out.Push(u32(b.dst_sel_x) | (u32(b.dst_sel_y) << 3) | (u32(b.dst_sel_z) << 6) |
             (u32(b.dst_sel_w) << 9) | (u32(b.num_format) << 12) | (u32(b.data_format) << 15) |
             (u32(b.element_size) << 19) | (u32(b.index_stride) << 21) |
             (u32(b.add_tid_enable) << 23) | (u32(b.type) << 24));
}

/// T# (AmdGpu::Image, after ImageResource::GetSharp's null/depth fallbacks): StageSpecialization
/// reads whether it is bound (base_address != 0), its type (view type, cube), data/number format
/// (integer, sRGB, number conversion, swizzle remap), destination swizzle, and base/last level
/// (ImageResource::NumBindings). Not its address, size, pitch or tiling.
template <typename Image, typename Output>
inline void ProjectImage(const Image& t, Output& out) noexcept {
    out.Push(u32(t.base_address != 0) | (u32(t.data_format) << 1) | (u32(t.num_format) << 7) |
             (u32(t.type) << 11));
    out.Push(u32(t.dst_sel_x) | (u32(t.dst_sel_y) << 3) | (u32(t.dst_sel_z) << 6) |
             (u32(t.dst_sel_w) << 9) | (u32(t.base_level) << 12) | (u32(t.last_level) << 16));
}

/// FMASK T#: whether it is bound, its width and height.
template <typename Image, typename Output>
inline void ProjectFmask(const Image& t, Output& out) noexcept {
    out.Push(u32(t.base_address != 0) | (u32(t.width) << 1) | (u32(t.height) << 15));
}

/// S# (AmdGpu::Sampler): whether it is set at all, force_unnormalized and force_degamma.
template <typename Sampler, typename Output>
inline void ProjectSampler(const Sampler& s, Output& out) noexcept {
    out.Push(u32(s.raw0 != 0 || s.raw1 != 0) | (u32((s.raw0 >> 15) & 1) << 1) |
             (u32((s.raw0 >> 20) & 1) << 2));
}

/// Every resource of one stage, in StageSpecialization's order: the fetch shader's vertex
/// attributes (`attribs`, may be null), buffers, images, FMASKs, samplers. A marker word between
/// the groups keeps two different splits of the same words apart.
template <typename Info, typename Attribs, typename Output>
inline void ProjectStageInto(const Info& info, const Attribs* attribs, Output& out) noexcept {
    out.Clear();
    if (attribs != nullptr) {
        for (const auto& a : *attribs) {
            ProjectBuffer(a.GetSharp(info), out);
        }
    }
    out.Push(0xA5A50001u);
    for (const auto& d : info.buffers) {
        ProjectBuffer(d.GetSharp(info), out);
    }
    out.Push(0xA5A50002u);
    for (const auto& d : info.images) {
        ProjectImage(d.GetSharp(info), out);
    }
    out.Push(0xA5A50003u);
    for (const auto& d : info.fmasks) {
        ProjectFmask(d.GetSharp(info), out);
    }
    out.Push(0xA5A50004u);
    for (const auto& d : info.samplers) {
        ProjectSampler(d.GetSharp(info), out);
    }
}

template <typename Info, typename Attribs>
inline void ProjectStage(const Info& info, const Attribs* attribs, Projection& out) noexcept {
    ProjectStageInto(info, attribs, out);
}

// Counts depend only on immutable resource lists, never on mutable descriptor values.
// Oversized lists retain the checked implementation and its exact overflow behavior.
template <typename Info, typename Attribs>
inline void ProjectStagePreflight(const Info& info, const Attribs* attribs, Projection& out) noexcept {
    const size_t a = attribs ? attribs->size() : 0;
    if (a > Projection::Capacity || info.buffers.size() > Projection::Capacity ||
        info.images.size() > Projection::Capacity || info.fmasks.size() > Projection::Capacity ||
        info.samplers.size() > Projection::Capacity ||
        4 + 2 * (a + info.buffers.size() + info.images.size()) +
            info.fmasks.size() + info.samplers.size() > Projection::Capacity) {
        ProjectStage(info, attribs, out);
        return;
    }
    struct Writer {
        Projection& projection;
        u32* cursor;
        void Clear() noexcept { projection.Clear(); cursor = projection.words.data(); }
        void Push(u32 value) noexcept { *cursor++ = value; }
    } writer{out, out.words.data()};
    ProjectStageInto(info, attribs, writer);
    out.count = static_cast<u32>(writer.cursor - out.words.data());
}

// ---------------------------------------------------------------------------------------------
// Byte-compared memo of a pure function: N key bytes -> Value.

/// A key built from the raw bytes of its parts (register blocks: no padding of their own).
template <size_t N>
struct KeyBytes {
    std::array<unsigned char, N> bytes{};
    size_t used{0};

    template <typename T>
    KeyBytes& Add(const T& part) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (used + sizeof(T) <= N) {
            std::memcpy(bytes.data() + used, &part, sizeof(T));
        }
        used += sizeof(T);
        return *this;
    }
    bool Complete() const noexcept {
        return used == N;
    }
};

template <size_t N, typename Value>
struct BytesMemo {
    std::array<unsigned char, N> key{};
    Value value{};
    u32 epoch{0};

    /// The stored value when `k` equals the stored key and the epoch is current; else null.
    const Value* Find(const KeyBytes<N>& k, u32 now) const noexcept {
        if (epoch == 0 || epoch != now || !k.Complete() ||
            std::memcmp(key.data(), k.bytes.data(), N) != 0) {
            return nullptr;
        }
        return &value;
    }
    void Store(const KeyBytes<N>& k, const Value& v, u32 now) {
        if (!k.Complete()) {
            return;
        }
        key = k.bytes;
        value = v;
        epoch = now;
    }
};

// ---------------------------------------------------------------------------------------------
// perf_bind_memo: one shader stage's image bindings (Rasterizer::BindTextures)

/// What BindTextures reads of an image descriptor besides its T#: depth, array, written (a
/// storage image; such a stage is never memoized), the mip fallback mode and the constant mip.
inline u64 BindAttrs(bool is_depth, bool is_array, bool is_written, u32 mip_fallback_mode,
                     u32 constant_mip_index) noexcept {
    return u64(is_depth) | (u64(is_array) << 1) | (u64(is_written) << 2) |
           (u64(mip_fallback_mode & 0xFFu) << 8) | (u64(constant_mip_index) << 32);
}

/// The key of a stage's image bindings: every image descriptor in order, its T# (the 32 bytes
/// ImageResource::GetSharp returns, after its null / depth fallbacks: the T# the bindings are
/// built from) and its attributes. Everything BindTextures' image part reads from the stage.
/// Fixed capacity (no allocation per draw); a stage with more images is never memoized.
struct BindKey {
    static constexpr u32 MaxImages = 16;
    static constexpr u32 WordsPerImage = 5;
    std::array<u64, MaxImages * WordsPerImage> words;
    u32 count{0};
    bool overflow{false};

    void Clear() noexcept {
        count = 0;
        overflow = false;
    }
    template <typename Image>
    void Add(const Image& tsharp, u64 attrs) noexcept {
        static_assert(sizeof(Image) == 4 * sizeof(u64) && std::is_trivially_copyable_v<Image>);
        if (count >= MaxImages) {
            overflow = true;
            return;
        }
        u64* w = words.data() + count * WordsPerImage;
        std::memcpy(w, &tsharp, sizeof(Image));
        w[4] = attrs;
        ++count;
    }
    bool Usable() const noexcept {
        return !overflow;
    }
    u64 Hash() const noexcept {
        u64 h = 0x9E3779B97F4A7C15ull ^ count;
        for (u32 i = 0; i < count * WordsPerImage; ++i) {
            h = (h ^ words[i]) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 31;
        }
        return h;
    }
    bool operator==(const BindKey& o) const noexcept {
        return !overflow && !o.overflow && count == o.count &&
               std::memcmp(words.data(), o.words.data(), count * WordsPerImage * sizeof(u64)) == 0;
    }
};

/// A remembered result: the stage's bindings (one per descriptor array element: an image with
/// the DynamicIndex mip fallback takes one per level) and each descriptor's array size. Valid
/// only in the texture cache's registry epoch and the memo epoch it was made in.
template <typename Binding>
struct BindEntry {
    static constexpr u32 MaxBindings = 24;
    BindKey key;
    u64 hash{0};
    u64 registry_epoch{0};
    u32 memo_epoch{0}; // 0 = empty or being filled
    u32 num_bindings{0};
    std::array<Binding, MaxBindings> bindings{};
    std::array<u8, BindKey::MaxImages> array_sizes{};
};

/// The bind memo's table: direct-mapped by the key's hash, 2^Bits entries on the heap. An entry
/// is found only with its key byte-equal and both epochs current; a new key in its slot replaces
/// it (Prepare empties the slot, Commit makes it valid once filled).
template <typename Binding, u32 Bits>
class BindTable {
public:
    using Entry = BindEntry<Binding>;
    static constexpr u32 Size = 1u << Bits;

    // Allocate once; scratch experiment trades more resident memory for fewer collisions.
    explicit BindTable(bool larger = false, bool extra_large = false)
        : slot_mask{Size * (extra_large ? 16 : larger ? 4 : 1) - 1},
          entries{std::make_unique<Entry[]>(slot_mask + 1)} {}

    const Entry* Find(const BindKey& key, u64 hash, u64 registry_epoch,
                      u32 memo_epoch) const noexcept {
        const Entry& e = entries[Slot(hash)];
        if (e.memo_epoch == 0 || e.memo_epoch != memo_epoch || e.registry_epoch != registry_epoch ||
            e.hash != hash || !(e.key == key)) {
            return nullptr;
        }
        return &e;
    }
    Entry& Prepare(const BindKey& key, u64 hash) noexcept {
        Entry& e = entries[Slot(hash)];
        e.memo_epoch = 0;
        e.key.count = key.count;
        e.key.overflow = key.overflow;
        std::memcpy(e.key.words.data(), key.words.data(),
                    key.count * BindKey::WordsPerImage * sizeof(u64));
        e.hash = hash;
        e.num_bindings = 0;
        return e;
    }
    static void Commit(Entry& e, u64 registry_epoch, u32 memo_epoch) noexcept {
        e.registry_epoch = registry_epoch;
        e.memo_epoch = memo_epoch;
    }

private:
    u32 Slot(u64 hash) const noexcept {
        return u32((hash ^ (hash >> 29)) & slot_mask);
    }
    const u32 slot_mask;
    std::unique_ptr<Entry[]> entries;
};

// ---------------------------------------------------------------------------------------------
// perf_copy_memo: the copy shader HLE's batches (Rasterizer::ObtainCopyBuffers)

/// One batch of the copy shader's HLE: the source and destination ranges it obtains buffers for.
struct CopyKey {
    u64 src_addr{};
    u64 dst_addr{};
    u32 src_size{};
    u32 dst_size{};

    bool operator==(const CopyKey&) const = default;
    u64 Hash() const noexcept {
        u64 h = 0x9E3779B97F4A7C15ull;
        for (const u64 v : {src_addr, dst_addr, (u64(src_size) << 32) | dst_size}) {
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 31;
        }
        return h;
    }
};

/// A remembered batch: the buffers (slot indices) and offsets ObtainBuffer gave for its ranges,
/// with what they are valid for: the buffer cache's epoch (no buffer registered or unregistered,
/// no GPU-modified range dropped since) and both ranges' write-tracking epochs, all read BEFORE
/// the full pass that made the entry, and the memo epoch. Any page state change during or after
/// that pass (its own uploads and first GPU marks included) makes the entry stale, so an entry is
/// used only once a full pass found both ranges already in sync and nothing changed since: then
/// ObtainBuffer would find the same buffers and have nothing to upload, mark or add.
struct CopyEntry {
    CopyKey key{};
    u64 cache_epoch{};
    u64 src_track{}; // 0 = not tracked: never stored
    u64 dst_track{};
    u32 memo_epoch{}; // 0 = empty
    u32 src_buffer{};
    u32 dst_buffer{};
    u32 src_offset{};
    u32 dst_offset{};
};

/// The copy memo's table: direct-mapped by the key's hash, 2^Bits entries on the heap; a new key
/// in its slot replaces the old one.
template <u32 Bits>
class CopyTable {
public:
    static constexpr u32 Size = 1u << Bits;

    CopyTable() : entries{std::make_unique<std::array<CopyEntry, Size>>()} {}

    const CopyEntry* Find(const CopyKey& key, u64 cache_epoch, u64 src_track, u64 dst_track,
                          u32 memo_epoch) const noexcept {
        const CopyEntry& e = (*entries)[Slot(key.Hash())];
        if (e.memo_epoch == 0 || e.memo_epoch != memo_epoch || src_track == 0 || dst_track == 0 ||
            e.cache_epoch != cache_epoch || e.src_track != src_track ||
            e.dst_track != dst_track || !(e.key == key)) {
            return nullptr;
        }
        return &e;
    }
    /// Stores the entry (a full pass's result with the epochs read before it); an entry with an
    /// untracked range or the empty memo epoch is not stored.
    bool Store(const CopyEntry& e) noexcept {
        if (e.memo_epoch == 0 || e.src_track == 0 || e.dst_track == 0) {
            return false;
        }
        (*entries)[Slot(e.key.Hash())] = e;
        return true;
    }

private:
    static u32 Slot(u64 hash) noexcept {
        return u32((hash ^ (hash >> 29)) & (Size - 1));
    }
    std::unique_ptr<std::array<CopyEntry, Size>> entries;
};

// ---------------------------------------------------------------------------------------------
// Counters (the CP thread only): hits, misses, shadow checks and their mismatches.

struct Counters {
    u64 hits{0};
    u64 misses{0};
    u64 checked{0};
    u64 mismatches{0};
    u64 total_mismatches{0}; // since the start (the first ones are logged in detail)

    /// One mismatch more; true for the first 20 since the start (log those in detail).
    bool NoteMismatch() noexcept {
        ++mismatches;
        return ++total_mismatches <= 20;
    }
    /// A new statistics window (keeps total_mismatches).
    void Reset() noexcept {
        hits = misses = checked = mismatches = 0;
    }
    double HitRate() const noexcept {
        const u64 n = hits + misses;
        return n ? double(hits) / double(n) : 0.0;
    }
};

} // namespace Vulkan::PerfMemo
