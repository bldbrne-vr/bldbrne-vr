// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_FUSED_PAIR's pass (scratchpad/dual-eye-perf/fused_pair.h, NativeStereo::PairCarried):
// random draws (stages of storage buffers, image arrays and samplers; cache, stream, constant
// ring and unregistered buffers, written ones with and without a right twin, camera pairs and
// their memo repeats, stereo and plain views, now and then a hint that does not match) are paired
// from what the binding carried and by a model of NativeStereo::PairDescriptors, which looks
// every descriptor up in the registry. Both must give the same writes, the same [left, right]
// infos, the same right buffers created and stream ranges mirrored in the same order.

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "scratchpad/dual-eye-perf/fused_pair.h"

namespace {

namespace Fused = NativeStereo::Fused;
using Handle = std::uint64_t;

enum class Type { Sampler, SampledImage, StorageImage, StorageBuffer, UniformTexel };

struct BufferInfo {
    Handle buffer{};
    std::uint64_t offset{}, range{};
    bool operator==(const BufferInfo&) const = default;
};
struct ImageInfo {
    Handle sampler{}, imageView{};
    std::uint32_t imageLayout{};
    bool operator==(const ImageInfo&) const = default;
};
struct Write {
    Type descriptorType{};
    std::uint32_t dstBinding{}, descriptorCount{};
    const BufferInfo* pBufferInfo{};
    const ImageInfo* pImageInfo{};
};
struct Entry {
    Handle right{};
    bool stream{};
};
struct BufferHint {
    Handle left{};
    Entry* entry{};
    BufferInfo camera{};
};
struct ViewHint {
    Handle left{}, right{};
};
struct CameraPair {
    BufferInfo left, right;
};

// The registry: buffers (a right twin made on demand) and stereo views (their right).
struct Registry {
    std::map<Handle, Entry> buffers;
    std::map<Handle, Handle> views;
};
constexpr Handle RightName(Handle left) {
    return 100000 + left;
}

// What the pass records, in order: a right buffer created ('c') or a stream range mirrored ('m').
struct Event {
    char what{};
    Handle buffer{};
    std::uint64_t offset{}, range{};
    bool operator==(const Event&) const = default;
};

Fused::Kind KindOf(const Write& w) {
    switch (w.descriptorType) {
    case Type::StorageBuffer:
        return Fused::Kind::Buffer;
    case Type::SampledImage:
    case Type::StorageImage:
        return Fused::Kind::Image;
    case Type::Sampler:
        return Fused::Kind::Sampler;
    default:
        return Fused::Kind::Other;
    }
}

struct Paired {
    std::vector<Write> writes;
    std::vector<BufferInfo> buffers;
    std::vector<ImageInfo> images;
    std::vector<Event> events;
    bool unregistered_written{};
};

// PairDescriptors from the registry alone (no carried handles), as native_stereo.cpp has it.
Paired Model(std::span<const Write> original, Registry& registry,
             std::span<const CameraPair> cameras, std::span<const std::uint8_t> writable,
             bool fold) {
    Paired out;
    std::size_t n = 0;
    for (const auto& w : original) {
        n += w.descriptorCount * 2;
    }
    out.buffers.reserve(n);
    out.images.reserve(n);
    for (auto w : original) {
        if (w.descriptorType == Type::StorageBuffer) {
            const bool written = w.dstBinding < writable.size() && writable[w.dstBinding];
            const auto first = out.buffers.size();
            for (std::uint32_t i = 0; i < w.descriptorCount; ++i) {
                auto left = w.pBufferInfo[i], right = left;
                if (left.buffer) { // RightBufferLocked
                    const auto it = registry.buffers.find(left.buffer);
                    if (it == registry.buffers.end()) {
                        out.unregistered_written |= written;
                    } else {
                        if (!it->second.right && written) {
                            it->second.right = RightName(left.buffer);
                            out.events.push_back({'c', left.buffer, 0, 0});
                        }
                        if (it->second.right) {
                            right.buffer = it->second.right;
                        }
                    }
                }
                if (right.buffer != left.buffer && !written &&
                    registry.buffers.at(left.buffer).stream) {
                    out.events.push_back({'m', left.buffer, left.offset, left.range});
                }
                for (const auto& c : cameras) {
                    if (left == c.left) {
                        right = c.right;
                        break;
                    }
                }
                out.buffers.push_back(left);
                out.buffers.push_back(right);
            }
            w.descriptorCount *= 2;
            w.pBufferInfo = out.buffers.data() + first;
        } else if (w.descriptorType == Type::SampledImage ||
                   w.descriptorType == Type::StorageImage) {
            const auto first = out.images.size();
            for (std::uint32_t i = 0; i < w.descriptorCount; ++i) {
                auto left = w.pImageInfo[i], right = left;
                const auto it = registry.views.find(left.imageView);
                right.imageView = it == registry.views.end() ? left.imageView : it->second;
                out.images.push_back(left);
                out.images.push_back(right);
            }
            w.descriptorCount *= 2;
            w.pImageInfo = out.images.data() + first;
        } else if (fold && w.descriptorType == Type::Sampler) {
            const auto first = out.images.size();
            out.images.insert(out.images.end(), w.pImageInfo, w.pImageInfo + w.descriptorCount);
            w.pImageInfo = out.images.data() + first;
        }
        out.writes.push_back(w);
    }
    return out;
}

// NativeStereo's CarriedOps on the mock registry.
struct MockOps {
    Registry& registry;
    std::span<const CameraPair> cameras;
    std::vector<Event>& events;
    int recorded = 0, lookups = 0, overflows = 0;
    bool unregistered_written = false;

    static Fused::Kind KindOf(const Write& w) {
        return ::KindOf(w);
    }
    static Handle RightOf(Entry& entry) {
        return entry.right;
    }
    static bool Stream(const Entry& entry) {
        return entry.stream;
    }
    Handle Create(Handle left, Entry& entry) {
        ++recorded;
        entry.right = RightName(left);
        events.push_back({'c', left, 0, 0});
        return entry.right;
    }
    Handle Lookup(Handle left, bool written, bool& stream) {
        ++lookups;
        const auto it = registry.buffers.find(left);
        if (it == registry.buffers.end()) {
            unregistered_written |= written;
            return left;
        }
        stream = it->second.stream;
        if (written && !it->second.right) {
            return Create(left, it->second);
        }
        return it->second.right ? it->second.right : left;
    }
    void Mirror(const BufferInfo& left, Handle) {
        ++recorded;
        events.push_back({'m', left.buffer, left.offset, left.range});
    }
    Handle LookupView(Handle left) {
        ++lookups;
        const auto it = registry.views.find(left);
        return it == registry.views.end() ? left : it->second;
    }
    void ScanCamera(const BufferInfo& left, BufferInfo& right) {
        ++lookups;
        for (const auto& c : cameras) {
            if (left == c.left) {
                right = c.right;
                break;
            }
        }
    }
    bool Overflow() {
        ++overflows;
        return false;
    }
};

// One draw as the rasterizer binds it: infos, the hints parallel to them, the writes pointing in.
struct Draw {
    Registry registry;
    std::vector<BufferInfo> buffer_infos;
    std::vector<BufferHint> buffer_hints;
    std::vector<ImageInfo> image_infos;
    std::vector<ViewHint> view_hints;
    std::vector<Write> writes;
    std::vector<std::uint8_t> writable;
    std::vector<CameraPair> cameras;
    int corrupted = 0, cameras_bound = 0, written = 0;
};

constexpr Handle StreamBuffer = 50, RingBuffer = 60;

Draw MakeDraw(std::mt19937& rng) {
    std::uniform_int_distribution<int> pct(0, 99);
    const auto roll = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    Draw d;
    // Room for every info: pointers into the arrays must stay put.
    d.buffer_infos.reserve(64);
    d.image_infos.reserve(128);
    for (Handle h = 1; h <= 12; ++h) {
        d.registry.buffers[h] = {pct(rng) < 50 ? RightName(h) : 0, false};
    }
    d.registry.buffers[StreamBuffer] = {pct(rng) < 20 ? RightName(StreamBuffer) : 0, true};
    d.registry.buffers[RingBuffer] = {0, true};
    for (Handle v = 200; v < 216; v += 2) {
        d.registry.views[v] = v + 500; // stereo views; odd ones are plain
    }
    std::uint64_t stream_at = 0, ring_at = 0;
    std::uint32_t unified = 0;
    const int stages = 1 + roll(3);
    for (int stage = 0; stage < stages; ++stage) {
        const int buffers = roll(7);
        for (int b = 0; b < buffers; ++b) {
            BufferInfo left{};
            BufferInfo camera{};
            bool written = false;
            const int kind = pct(rng);
            if (kind < 10) {
                left = {0, 0, ~0ull}; // a null descriptor
            } else if (kind < 40) {
                left = {Handle(1 + roll(12)), std::uint64_t(roll(16)) * 256,
                        std::uint64_t(16 + roll(8) * 16)};
                written = pct(rng) < 30;
            } else if (kind < 48) {
                left = {Handle(70 + roll(4)), 0, 64}; // unregistered, read only
            } else if (kind < 66) {
                left = {StreamBuffer, stream_at += 1024, 128};
                written = pct(rng) < 5; // the LDS buffer: makes the stream buffer's right
            } else if (kind < 76) {
                left = {RingBuffer, ring_at += 1024, 208};
            } else {
                // A camera (lighting, screen point...) pair: its own allocations, or the memo's
                // repeat of an earlier one (registered again with the same halves).
                if (!d.cameras.empty() && pct(rng) < 25) {
                    const CameraPair again = d.cameras[roll(int(d.cameras.size()))];
                    d.cameras.push_back(again);
                } else {
                    const Handle buffer = pct(rng) < 50 ? StreamBuffer : RingBuffer;
                    auto& at = buffer == StreamBuffer ? stream_at : ring_at;
                    at += 2048;
                    d.cameras.push_back({{buffer, at, 864}, {buffer, at + 1024, 864}});
                }
                left = d.cameras.back().left;
                camera = d.cameras.back().right;
                ++d.cameras_bound;
            }
            d.written += written;
            if (d.writable.size() <= unified) {
                d.writable.resize(unified + 1);
            }
            d.writable[unified] = written;
            d.buffer_infos.push_back(left);
            const auto it = d.registry.buffers.find(left.buffer);
            BufferHint hint{left.buffer, it == d.registry.buffers.end() ? nullptr : &it->second,
                            camera};
            if (left.buffer && pct(rng) < 5) {
                ++d.corrupted;
                if (pct(rng) < 50) {
                    hint.left = 9999; // no hint for this handle: the registry and the scan
                    hint.camera = {};
                } else {
                    hint.entry = nullptr; // no entry: the registry
                }
            }
            d.buffer_hints.push_back(hint);
            d.writes.push_back({Type::StorageBuffer, unified++, 1, &d.buffer_infos.back(), nullptr});
        }
        const int images = roll(5);
        for (int i = 0; i < images; ++i) {
            const std::uint32_t array = 1 + roll(3);
            const auto first = d.image_infos.size();
            for (std::uint32_t e = 0; e < array; ++e) {
                const int kind = pct(rng);
                const Handle view = kind < 10 ? 0 : Handle(200 + roll(16));
                d.image_infos.push_back({0, view, std::uint32_t(roll(3))});
                const auto it = d.registry.views.find(view);
                ViewHint hint{view, view == 0 ? 0 : it == d.registry.views.end() ? view : it->second};
                if (view && pct(rng) < 5) {
                    ++d.corrupted;
                    hint.left = 1; // another view's hint
                }
                d.view_hints.resize(d.image_infos.size() - 1); // samplers leave empty hints
                d.view_hints.push_back(hint);
            }
            d.writes.push_back({pct(rng) < 20 ? Type::StorageImage : Type::SampledImage, unified,
                                array, nullptr, &d.image_infos[first]});
            unified += array;
        }
        const int samplers = roll(3);
        for (int s = 0; s < samplers; ++s) {
            d.image_infos.push_back({Handle(300 + roll(8)), 0, 0});
            d.writes.push_back({Type::Sampler, unified++, 1, nullptr, &d.image_infos.back()});
        }
        if (pct(rng) < 5) {
            d.writes.push_back({Type::UniformTexel, unified++, 1, nullptr, nullptr});
        }
    }
    if (!d.view_hints.empty() && pct(rng) < 5) {
        d.view_hints.pop_back(); // the last hint missing: the registry
        ++d.corrupted;
    }
    d.writable.resize(unified + 1);
    return d;
}

struct OpsCount {
    int recorded = 0, lookups = 0, overflows = 0;
};

// The carried pass on its own copy of the draw's registry (the model takes another).
Paired Carried(const Draw& d, Registry& registry, bool fold, OpsCount& count) {
    Paired out;
    out.writes.assign(d.writes.begin(), d.writes.end());
    out.buffers.resize(2 * d.buffer_infos.size());
    out.images.resize(2 * d.image_infos.size());
    // The hints name the copy's entries (the same keys).
    std::vector<BufferHint> hints = d.buffer_hints;
    for (auto& h : hints) {
        for (const auto& [key, entry] : d.registry.buffers) {
            if (h.entry == &entry) {
                h.entry = &registry.buffers.at(key);
                break;
            }
        }
    }
    MockOps ops{registry, d.cameras, out.events};
    const Fused::Bound<BufferInfo, ImageInfo, BufferHint, ViewHint> bound{
        d.buffer_infos.data(), d.buffer_infos.size(), hints,
        d.image_infos.data(), d.image_infos.size(), d.view_hints};
    std::size_t buffers_used = 0, images_used = 0;
    const bool paired =
        Fused::Pair(std::span<Write>{out.writes}, bound, std::span<const std::uint8_t>{d.writable},
                    std::span<BufferInfo>{out.buffers}, std::span<ImageInfo>{out.images},
                    buffers_used, images_used, fold, ops);
    if (!paired) {
        out.writes.clear();
    }
    out.buffers.resize(buffers_used);
    out.images.resize(images_used);
    out.unregistered_written = ops.unregistered_written;
    count = {ops.recorded, ops.lookups, ops.overflows};
    return out;
}

// The first difference between the model's and the carried pass's results ("" when none).
std::string Difference(const Paired& model, const Paired& carried, bool fold) {
    if (model.writes.size() != carried.writes.size()) {
        return "write count";
    }
    for (std::size_t k = 0; k < model.writes.size(); ++k) {
        const auto& a = model.writes[k];
        const auto& b = carried.writes[k];
        const std::string at = " at write " + std::to_string(k);
        if (a.descriptorType != b.descriptorType || a.dstBinding != b.dstBinding ||
            a.descriptorCount != b.descriptorCount) {
            return "write" + at;
        }
        const auto kind = KindOf(a);
        for (std::uint32_t i = 0; i < a.descriptorCount; ++i) {
            if (kind == Fused::Kind::Buffer && !(a.pBufferInfo[i] == b.pBufferInfo[i])) {
                return "buffer info " + std::to_string(i) + at;
            }
            if ((kind == Fused::Kind::Image || (fold && kind == Fused::Kind::Sampler)) &&
                !(a.pImageInfo[i] == b.pImageInfo[i])) {
                return "image info " + std::to_string(i) + at;
            }
        }
        if ((kind == Fused::Kind::Other || (!fold && kind == Fused::Kind::Sampler)) &&
            (a.pImageInfo != b.pImageInfo || a.pBufferInfo != b.pBufferInfo)) {
            return "untouched write" + at;
        }
    }
    if (model.events != carried.events) {
        return "recorded creations and mirrors";
    }
    if (model.unregistered_written != carried.unregistered_written) {
        return "unregistered written buffer";
    }
    return "";
}

TEST(FusedPair, IndexIn) {
    const std::uint64_t array[4]{};
    EXPECT_EQ(Fused::IndexIn(array + 2, array, 4), std::size_t{2});
    EXPECT_EQ(Fused::IndexIn(array + 4, array, 4), std::size_t{4}); // one past: elsewhere
    EXPECT_EQ(Fused::IndexIn(array - 1, array, 4), std::size_t{4});
    const auto* bytes = reinterpret_cast<const unsigned char*>(array) + 3;
    EXPECT_EQ(Fused::IndexIn(reinterpret_cast<const std::uint64_t*>(bytes), array, 4),
              std::size_t{4}); // inside, not on an element
}

TEST(FusedPair, MatchesTheRegistryPass) {
    int draws = 0, corrupted = 0, cameras = 0, written = 0, events = 0, lookups = 0, wrong = 0;
    for (std::uint32_t seed = 1; seed <= 20000; ++seed) {
        std::mt19937 rng(seed);
        const Draw d = MakeDraw(rng);
        const bool fold = seed % 3 != 0;
        Registry model_registry = d.registry;
        Registry carried_registry = d.registry;
        const Paired model = Model(d.writes, model_registry, d.cameras, d.writable, fold);
        OpsCount ops;
        const Paired carried = Carried(d, carried_registry, fold, ops);
        const std::string diff = Difference(model, carried, fold);
        if (!diff.empty() && ++wrong <= 5) {
            EXPECT_EQ(diff + " (seed " + std::to_string(seed) + ")", std::string{});
        }
        EXPECT_EQ(ops.overflows, 0);
        bool same_rights = true;
        for (const auto& [key, entry] : model_registry.buffers) {
            same_rights = same_rights && carried_registry.buffers.at(key).right == entry.right;
        }
        EXPECT_TRUE(same_rights);
        ++draws;
        corrupted += d.corrupted;
        cameras += d.cameras_bound;
        written += d.written;
        events += int(model.events.size());
        lookups += ops.lookups;
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(draws, 20000);
    // The streams covered what they should: misses, cameras, creations and mirrors.
    EXPECT_GT(corrupted, 1000);
    EXPECT_GT(cameras, 10000);
    EXPECT_GT(written, 5000);
    EXPECT_GT(events, 5000);
    EXPECT_GT(lookups, 10000);
}

TEST(FusedPair, CarriedHintsTakeNoLookups) {
    // Every descriptor carried its hint (no unregistered buffer, no corruption): the registry is
    // asked nothing, whatever it holds.
    for (std::uint32_t seed = 1; seed <= 2000; ++seed) {
        std::mt19937 rng(seed);
        Draw d = MakeDraw(rng);
        bool clean = d.corrupted == 0;
        for (const auto& h : d.buffer_hints) {
            clean = clean && (h.left == 0 || h.entry);
        }
        if (!clean) {
            continue;
        }
        Registry registry = d.registry;
        OpsCount ops;
        (void)Carried(d, registry, true, ops);
        EXPECT_EQ(ops.lookups, 0);
    }
}

TEST(FusedPair, OverflowRecordsNothing) {
    // A write pointing outside the binding's arrays with more descriptors than they hold.
    Draw d;
    d.registry.buffers[1] = {0, false};
    d.buffer_infos.push_back({1, 0, 64});
    d.buffer_hints.push_back({1, &d.registry.buffers[1], {}});
    const BufferInfo outside[3]{{1, 0, 64}, {1, 64, 64}, {1, 128, 64}};
    d.writes.push_back({Type::StorageBuffer, 0, 3, outside, nullptr});
    d.writable = {1};
    Registry registry = d.registry;
    OpsCount ops;
    const Paired carried = Carried(d, registry, true, ops);
    EXPECT_EQ(ops.overflows, 1);
    EXPECT_EQ(ops.recorded, 0);
    EXPECT_TRUE(carried.writes.empty());
    EXPECT_EQ(registry.buffers.at(1).right, Handle{0});
}

TEST(FusedPair, CameraNeedsUniqueLefts) {
    // The carried camera is the descriptor's own pair; the registry pass takes the first pair
    // whose left half equals the descriptor. They agree because every pair's left is an
    // allocation of its own (or the memo's repeat of the same pair). A plain descriptor equal to
    // a pair's left half would be the one case apart: shown here, never made by the binding.
    Draw d;
    d.registry.buffers[StreamBuffer] = {0, true};
    d.cameras.push_back({{StreamBuffer, 0, 864}, {StreamBuffer, 1024, 864}});
    d.buffer_infos.push_back({StreamBuffer, 0, 864}); // plain, not the camera's binding
    d.buffer_hints.push_back({StreamBuffer, &d.registry.buffers[StreamBuffer], {}});
    d.writes.push_back({Type::StorageBuffer, 0, 1, &d.buffer_infos.back(), nullptr});
    d.writable = {0};
    Registry model_registry = d.registry, carried_registry = d.registry;
    const Paired model = Model(d.writes, model_registry, d.cameras, d.writable, true);
    OpsCount ops;
    const Paired carried = Carried(d, carried_registry, true, ops);
    EXPECT_EQ(model.buffers.at(1).offset, std::uint64_t{1024});
    EXPECT_EQ(carried.buffers.at(1).offset, std::uint64_t{0});
}

} // namespace
