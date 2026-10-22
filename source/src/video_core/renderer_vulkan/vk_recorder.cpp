#include "video_core/flip_order_trace.h"
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#if !defined(__clang__) && !defined(__GNUC__)
#include <xmmintrin.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_push_template.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_set_write_codec.h"

namespace Vulkan {
// FAST bit 35: redundant binding commands are not encoded, and Allocate requests the ring lines
// ahead of the write position (see RecorderBindState in vk_recorder.h).
static constexpr auto RecorderBindFilter = static_cast<FastRender::Bit>(35);
#include "video_core/renderer_vulkan/vk_recorder_packets.inc"
namespace {
constexpr u32 Aligned(size_t n) {
    return static_cast<u32>((n + 7) & ~size_t{7});
}
struct Header {
    RecorderOpcode opcode;
    u32 size;
};
static_assert(sizeof(Header) == 8);
template <class T>
void Store(std::byte*& dst, const vk::ArrayProxy<const T>& values) {
    static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
    if (values.size())
        std::memcpy(dst, values.data(), values.size() * sizeof(T));
    dst += Aligned(values.size() * sizeof(T));
}
template <class T>
vk::ArrayProxy<const T> Load(const std::byte*& src, u32 count) {
    const auto* result = reinterpret_cast<const T*>(src);
    src += Aligned(count * sizeof(T));
    return {count, result};
}
template <class T>
bool HasNext(const vk::ArrayProxy<const T>& values) {
    return std::any_of(values.begin(), values.end(), [](const T& v) { return v.pNext != nullptr; });
}
struct PushPacket {
    vk::PipelineLayout layout;
    vk::ShaderStageFlags stages;
    u32 offset, size;
};
struct DescriptorPacket {
    vk::PipelineLayout layout;
    vk::PipelineBindPoint bind_point;
    u32 set, count;
    u32 bulk{}; // FAST bit 28 layout (1), TemplatePush or SetWritePacket; former padding
};
static_assert(sizeof(DescriptorPacket) == 24);
// FAST_CPU_PUSH_TEMPLATE (pushDescriptorSetTemplate): a DescriptorPacket whose `bulk` is this,
// `count` the payload bytes, then the update template and the payload (bit 28's buffer infos,
// then its image infos), replayed by one vkCmdPushDescriptorSetWithTemplateKHR. Its layout field
// is where the per-write packets keep theirs (FindBound reads it).
constexpr u32 TemplatePush = 2;
constexpr u32 TemplateAt = Aligned(sizeof(DescriptorPacket));
constexpr u32 TemplatePayloadAt = TemplateAt + Aligned(sizeof(vk::DescriptorUpdateTemplate));
// DescriptorPacket::bulk of a FAST_CPU_DRIVER_CALLS set write (CommandRecorder::ReplaySetWrite):
// a SetWriteHead follows the packet, then the writes' SetWriteCodec encoding. Its layout is the
// pipeline layout the set is bound with (FindBound reads it as for any descriptor packet).
constexpr u32 SetWritePacket = 3;
struct SetWriteHead {
    DescriptorHeap* heap;
    vk::DescriptorSetLayout set_layout;
    u64 hash; // FAST_CPU_DRIVER_CALLS_VERIFY: SetWriteCodec::Hash of the caller's writes
};
static_assert(sizeof(SetWriteHead) == 24);
struct Barrier2Packet {
    vk::DependencyFlags flags;
    u32 memory, buffers, images;
};
struct BarrierPacket {
    vk::PipelineStageFlags src, dst;
    vk::DependencyFlags flags;
    u32 memory, buffers, images;
};
struct RenderingPacket {
    vk::RenderingInfo info;
    u32 depth, stencil;
};
struct ControlPacket {
    u32 kind, bytes;
    u32 records; // FAST_CPU_PUSH_SPLIT_VERIFY: its replay may record (QueueControl)
};
struct LabelPacket {
    std::array<float, 4> color;
};
u32 DescriptorBytes(vk::DescriptorType type, u32 count) {
    switch (type) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return count * sizeof(vk::DescriptorBufferInfo);
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment:
        return count * sizeof(vk::DescriptorImageInfo);
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return count * sizeof(vk::BufferView);
    default:
        return 0;
    }
}
const void* DescriptorData(const vk::WriteDescriptorSet& w) {
    switch (w.descriptorType) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return w.pBufferInfo;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        return w.pTexelBufferView;
    default:
        return w.pImageInfo;
    }
}
void SetDescriptorData(vk::WriteDescriptorSet& w, const std::byte* data) {
    switch (w.descriptorType) {
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        w.pBufferInfo = reinterpret_cast<const vk::DescriptorBufferInfo*>(data);
        break;
    case vk::DescriptorType::eUniformTexelBuffer:
    case vk::DescriptorType::eStorageTexelBuffer:
        w.pTexelBufferView = reinterpret_cast<const vk::BufferView*>(data);
        break;
    default:
        w.pImageInfo = reinterpret_cast<const vk::DescriptorImageInfo*>(data);
        break;
    }
}
// FAST bit 28 (bulk push descriptors). A bulk packet is the writes followed by a copy of the
// caller's buffer-info array and image-info array (bbport vk_pipeline_common.cpp:68-85). The
// write's info pointer holds a payload byte offset; other info pointers are cleared exactly as
// in the per-write layout. bbport rebases at record time; ring packets stay position-independent.
enum class BulkInfo { Other, Buffer, Image };
BulkInfo BulkInfoOf(vk::DescriptorType type) {
    switch (type) { // the classes of DescriptorBytes; texel views are not bulk-encoded
    case vk::DescriptorType::eUniformBuffer:
    case vk::DescriptorType::eStorageBuffer:
    case vk::DescriptorType::eUniformBufferDynamic:
    case vk::DescriptorType::eStorageBufferDynamic:
        return BulkInfo::Buffer;
    case vk::DescriptorType::eSampler:
    case vk::DescriptorType::eCombinedImageSampler:
    case vk::DescriptorType::eSampledImage:
    case vk::DescriptorType::eStorageImage:
    case vk::DescriptorType::eInputAttachment:
        return BulkInfo::Image;
    default:
        return BulkInfo::Other;
    }
}
constexpr size_t NotWithin = ~size_t{0};
// Byte offset of info[0, count) inside `array`; NotWithin for null, empty or outside ranges.
template <class T>
size_t OffsetWithin(const T* info, u32 count, std::span<const T> array) {
    const size_t at =
        reinterpret_cast<std::uintptr_t>(info) - reinterpret_cast<std::uintptr_t>(array.data());
    const size_t bytes = size_t{count} * sizeof(T);
    return count && bytes <= array.size_bytes() && at <= array.size_bytes() - bytes ? at
                                                                                     : NotWithin;
}
void RebaseBulk(vk::WriteDescriptorSet& w, const std::byte* data) {
    if (w.pBufferInfo)
        w.pBufferInfo = reinterpret_cast<const vk::DescriptorBufferInfo*>(
            data + reinterpret_cast<std::uintptr_t>(w.pBufferInfo));
    else
        w.pImageInfo = reinterpret_cast<const vk::DescriptorImageInfo*>(
            data + reinterpret_cast<std::uintptr_t>(w.pImageInfo));
}
// FAST_PUSH_BULK_VERIFY: decodes a bulk packet as replay does and compares every write with what
// the per-write encoding replays. Returns the first differing write, or the count when equal.
u32 VerifyBulk(const std::byte* data, u32 bytes,
               const vk::ArrayProxy<const vk::WriteDescriptorSet>& writes) {
    const auto& p = *reinterpret_cast<const DescriptorPacket*>(data);
    const auto* encoded =
        reinterpret_cast<const vk::WriteDescriptorSet*>(data + Aligned(sizeof(p)));
    const auto* infos = data + Aligned(sizeof(p)) + Aligned(p.count * sizeof(*encoded));
    if (!p.bulk || p.count != writes.size())
        return 0;
    for (u32 i = 0; i < p.count; ++i) {
        auto decoded = encoded[i];
        RebaseBulk(decoded, data);
        const auto* info = static_cast<const std::byte*>(DescriptorData(decoded));
        auto expected = writes.data()[i];
        expected.pNext = nullptr;
        expected.pBufferInfo = nullptr;
        expected.pImageInfo = nullptr;
        expected.pTexelBufferView = nullptr;
        SetDescriptorData(expected, info);
        const u32 n = DescriptorBytes(expected.descriptorType, expected.descriptorCount);
        if (!n || decoded != expected || info < infos || info + n > data + bytes ||
            std::memcmp(info, DescriptorData(writes.data()[i]), n) != 0)
            return i;
    }
    return p.count;
}

// The switches below are read once at start-up into namespace-scope constants: a plain load per
// use, where function-local statics paid a guard (a thread-local read) on every call.
// FAST_SKIP_EMPTY_FINISH_VERIFY=N (vk_scheduler.cpp, FAST bit 56): only then does the replay count
// its packets per command buffer; without it the replay loop is the one before bit 56.
const bool g_replay_census = [] {
    const char* value = std::getenv("FAST_SKIP_EMPTY_FINISH_VERIFY");
    return value != nullptr && std::strtoull(value, nullptr, 0) != 0;
}();
bool ReplayCensusWanted() {
    return g_replay_census;
}
// FAST_CPU_DRIVER_CALLS_VERIFY=1 (the switch itself: Pipeline::BindResources): every set-write
// packet carries the hash of the caller's writes, and its replay compares the decoded writes'
// hash with it before writing them (a mismatch is logged: the first 16, then every 1024th). Every
// heap use through WriteDescriptorSet or the replay also checks that no other thread is inside
// the heap at the same time.
const bool g_driver_calls_verify = [] {
    const char* value = std::getenv("FAST_CPU_DRIVER_CALLS_VERIFY");
    return value && value[0] == '1';
}();
std::atomic<u32> g_heap_users{}; // FAST_CPU_DRIVER_CALLS_VERIFY: threads inside a heap write
// DescriptorHeap::Write, with FAST_CPU_DRIVER_CALLS_VERIFY's check that the heap is this
// thread's alone meanwhile.
vk::DescriptorSet HeapWrite(DescriptorHeap& heap, vk::DescriptorSetLayout set_layout,
                            std::span<vk::WriteDescriptorSet> writes) {
    if (!g_driver_calls_verify) {
        return heap.Write(set_layout, writes);
    }
    if (const u32 users = g_heap_users.fetch_add(1, std::memory_order_acq_rel); users != 0) {
        LOG_ERROR(Render_Vulkan,
                  "FAST_CPU_DRIVER_CALLS verify: {} other thread(s) inside the descriptor heap",
                  users);
    }
    const vk::DescriptorSet desc_set = heap.Write(set_layout, writes);
    g_heap_users.fetch_sub(1, std::memory_order_acq_rel);
    return desc_set;
}

// FAST bit 35 (RecorderBindFilter).
constexpr std::array<const char*, static_cast<size_t>(RecorderBindKind::Count)> kBindKindNames{
    "pipeline", "vertex_input", "vertex", "index", "push"};
// FAST_BIND_FILTER_VERIFY=N: every Nth skip is checked against the encoded ring (FindBound).
const u64 g_bind_verify_every = [] {
    const char* value = std::getenv("FAST_BIND_FILTER_VERIFY");
    return value ? std::strtoull(value, nullptr, 0) : 0ULL;
}();
u64 BindVerifyEvery() {
    return g_bind_verify_every;
}
// FAST_BIND_FILTER_PREFETCH=0 keeps bit 35's filter without the ring write prefetch.
const bool g_bind_prefetch = [] {
    const char* value = std::getenv("FAST_BIND_FILTER_PREFETCH");
    return !value || value[0] != '0';
}();
bool BindPrefetch() {
    return g_bind_prefetch;
}
// bbport RecordChunk::PrefetchAhead (vk_scheduler.h:661-671): the replay thread read these ring
// lines last; requesting them a few lines ahead overlaps the ownership transfers instead of
// stalling on each first store (bbport parallel_gpu.md:294, kept with the FindView memo).
inline void PrefetchWrite(const std::byte* address) {
#if defined(__clang__) || defined(__GNUC__)
    __builtin_prefetch(address, 1, 3);
#else
    _mm_prefetch(reinterpret_cast<const char*>(address), _MM_HINT_T0);
#endif
}
// FAST_CPU_PUSH_SPLIT (on unless '0', latched per command buffer in Configure): PushSplit leaves
// out what the command buffer holds.
const bool g_push_split = [] {
    const char* value = std::getenv("FAST_CPU_PUSH_SPLIT");
    return !value || value[0] != '0';
}();
bool PushSplitOn() {
    return g_push_split;
}
// FAST_CPU_PUSH_SPLIT_VERIFY=1: PushSplit keeps the held block as it would, but pushes the whole
// block every time, and checks each leave-out it would have made against the encoded ring
// (VerifyPushSplit).
const bool g_push_split_verify = [] {
    const char* value = std::getenv("FAST_CPU_PUSH_SPLIT_VERIFY");
    return value && value[0] == '1';
}();
bool PushSplitVerify() {
    return g_push_split_verify;
}
// FAST_CPU_DRAIN_SPIN (on unless '0'; read once at start-up): Drain spins for the replay (4096
// pauses, about 70 us on a 5800X3D) before it sleeps, as the stage pipe's waits do. A flush took
// 72 us on average (2026-10-14 profile), and a thread woken after sleeping that long resumes
// about 5 us later than one spinning (7 against 2 us measured), on stage B's critical path.
// A decimal number above 1 sets the pauses instead (e.g. 1024: about the wake-up gain, so long
// flushes burn less of B). The spin is busy time on the waiting thread, counted apart
// (FastRender::NoteRecorderSpin: flush_spin_us/f, stageB_drain_spin_us/f): stageB_busy and
// stageB_us/draw rise with it while fps may improve, so judge the switch by fps and frame time.
// Windows builds only (elsewhere the waits keep sleeping at once).
[[maybe_unused]] const u32 g_drain_spin_pauses = [] {
    const char* value = std::getenv("FAST_CPU_DRAIN_SPIN");
    if (!value)
        return 4096u;
    if (value[0] == '0')
        return 0u;
    const unsigned long pauses = std::strtoul(value, nullptr, 10);
    return pauses > 1 ? static_cast<u32>(std::min<unsigned long>(pauses, 1ul << 20)) : 4096u;
}();
// FAST_RECORDER_PUBLISH_KB (read once at start-up; 1..4096, default 16): Commit publishes the
// batch to the replay thread once this many KiB are encoded since the last publish. Smaller
// batches let the replay follow closer behind (shorter drains) at the cost of more wakes of a
// sleeping replay thread; larger ones the reverse. Publication only: the replay order is the same.
const u64 g_publish_bytes = [] {
    const char* value = std::getenv("FAST_RECORDER_PUBLISH_KB");
    const long kib = value ? std::strtol(value, nullptr, 10) : 16l;
    return u64(std::clamp(kib > 0 ? kib : 16l, 1l, 4096l)) * 1024;
}();
// The call's array equals the first `count` tracked values (Vulkan-Hpp member comparison).
template <class T>
bool SameArray(const vk::ArrayProxy<const T>& values, const T* tracked, u32 count) {
    return values.size() == count && std::equal(values.begin(), values.end(), tracked);
}
// The tracked binding a packet of this opcode replaces; Count for every other packet.
RecorderBindKind BindKindOf(RecorderOpcode opcode) {
    switch (opcode) {
    case RecorderOpcode::bindPipeline:
        return RecorderBindKind::Pipeline;
    case RecorderOpcode::setVertexInputEXT:
        return RecorderBindKind::VertexInput;
    case RecorderOpcode::bindVertexBuffers:
    case RecorderOpcode::bindVertexBuffers2:
        return RecorderBindKind::VertexBuffers;
    case RecorderOpcode::bindIndexBuffer:
        return RecorderBindKind::IndexBuffer;
    case RecorderOpcode::PushConstants:
        return RecorderBindKind::Push;
    default:
        return RecorderBindKind::Count;
    }
}
} // namespace

CommandRecorder::CommandRecorder(vk::CommandBuffer& command, bool allowed)
    : command{command}, allowed{allowed} {
    Configure();
}
CommandRecorder::~CommandRecorder() {
    Stop();
}
void CommandRecorder::Configure() {
    const bool next = allowed && FastRender::On(FastRender::RecorderInline);
    // A or B may be the producer, never both: DrawPipe's completion acquire
    // transfers scheduler ownership before A touches it. Replay remains owned
    // exclusively by this recorder's consumer across producer handoffs.
    const bool next_threaded = next && FastRender::On(FastRender::RecorderThread);
    if (next != enabled || next_threaded != threaded) {
        FlipOrderTrace::Event("mode_begin", reinterpret_cast<uintptr_t>(this), next, next_threaded,
                              (u64(enabled) << 1) | u64(threaded));
        Drain("mode_change");
        if (next && !ring)
            ring = std::make_unique<std::byte[]>(Capacity);
        enabled = next;
        threaded = next_threaded;
        if (threaded && !worker.joinable())
            worker = std::jthread([this] { Run(); });
        FlipOrderTrace::Event("mode_end", reinterpret_cast<uintptr_t>(this), enabled, threaded);
    }
    // A new producer command buffer starts here. In R2 its physical allocation may
    // still be queued; the proxy never reads that handle before a raw drain.
    raw_borrowed = false;
    // FAST bit 35 is latched for the whole command buffer, which starts with nothing bound: a
    // toggle never meets state tracked while the filter was off.
    bind_filter = enabled && FastRender::On(RecorderBindFilter);
    prefetch_ahead = bind_filter && BindPrefetch();
    binds.Reset();
    // FAST_CPU_PUSH_SPLIT, latched like bit 35: a new command buffer holds no push constants.
    push_split = enabled && PushSplitOn();
    push_split_verify = push_split && PushSplitVerify();
    ForgetHeldPush();
    verify_held.Forget();
    segment_start = write_position;
}
void CommandRecorder::Stop() {
    Drain("shutdown");
    if (!worker.joinable())
        return;
    stopping.store(true, std::memory_order_release);
    wake_sequence.fetch_add(1, std::memory_order_release);
    wake_sequence.notify_one();
    worker.join();
}
void CommandRecorder::QueueControl(u32 kind, const void* payload, u32 bytes, bool publish,
                                   bool records) {
    if (records)
        ForgetHeldPush(); // FAST_CPU_PUSH_SPLIT: its replay may record into the command buffer
    auto* data = Allocate(RecorderOpcode::Control, Aligned(sizeof(ControlPacket)) + bytes);
    const ControlPacket packet{kind, bytes, records};
    std::memcpy(data, &packet, sizeof(packet));
    if (bytes)
        std::memcpy(data + Aligned(sizeof(packet)), payload, bytes);
    Commit();
    ++control_packets; // FAST bit 56: not a Vulkan command
    if (publish)
        Publish(); // submissions and CPU waits publish the partial batch
}
void CommandRecorder::PublishStats() {
    if (!encoded_packets)
        return;
    FastRender::NoteRecorderEncoded(encoded_packets, encoded_bytes, encode_ns, encode_samples);
    encoded_packets = encoded_bytes = encode_ns = encode_samples = 0;
}
void CommandRecorder::Publish() {
    PublishStats();
    if (!threaded || write_position == last_published)
        return;
    last_published = write_position;
    published.store(write_position, std::memory_order_release);
    wake_sequence.fetch_add(1, std::memory_order_release);
    wake_sequence.notify_one();
}
void CommandRecorder::Run() {
    Common::SetCurrentThreadName("fast:Recorder");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    FastRender::g_on_recorder = true;
    FastRender::RegisterThread(FastRender::Role::Recorder);
    const auto spin = std::chrono::microseconds([] {
        const char* text = std::getenv("FAST_RECORDER_SPIN_US");
        return text ? std::clamp(std::atoi(text), 0, 2000) : 10;
    }());
    while (!stopping.load(std::memory_order_acquire)) {
        const u64 end = published.load(std::memory_order_acquire);
        if (completed.load(std::memory_order_acquire) < end) {
            ReplayUntil(end);
            continue;
        }
        const auto until = std::chrono::steady_clock::now() + spin;
        while (std::chrono::steady_clock::now() < until &&
               published.load(std::memory_order_acquire) <=
                   completed.load(std::memory_order_acquire) &&
               !stopping.load(std::memory_order_relaxed)) {
#ifdef _MSC_VER
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
        const u32 wake = wake_sequence.load(std::memory_order_acquire);
        if (stopping.load(std::memory_order_acquire))
            break;
        if (published.load(std::memory_order_acquire) > completed.load(std::memory_order_acquire))
            continue;
        wake_sequence.wait(wake, std::memory_order_acquire);
    }
}
vk::CommandBuffer RecorderProxy::Raw(std::string_view reason) const {
    return recorder.Raw(reason, site);
}
vk::CommandBuffer CommandRecorder::Raw(std::string_view reason, std::source_location site) {
    ++raw_borrows; // FAST bit 56: the caller may record directly, unseen by the packet counts
    Drain(reason, site);
    raw_borrowed = true;
    ResetBinds();     // FAST bit 35: raw commands may bind anything
    ForgetHeldPush(); // FAST_CPU_PUSH_SPLIT: or push anything
    return command;
}
std::byte* CommandRecorder::Allocate(RecorderOpcode opcode, u32 bytes) {
    ASSERT(enabled && !reserved_bytes);
    sample_encode = (++sample_sequence & (FastRender::g_encode_sample_every - 1)) == 0;
    if (sample_encode)
        encode_start = std::chrono::steady_clock::now();
    const u32 size = Aligned(sizeof(Header) + bytes);
    ASSERT_MSG(size <= Capacity - sizeof(Header), "Recorder packet exceeds ring capacity");
    u32 offset = write_position % Capacity;
    const u32 padding = offset + size > Capacity ? Capacity - offset : 0;
    if (write_position + padding + size - completed.load(std::memory_order_acquire) > Capacity)
        Drain("ring_full");
    if (padding) {
        const Header pad{RecorderOpcode::Padding, padding};
        std::memcpy(ring.get() + offset, &pad, sizeof(pad));
        write_position += padding;
        offset = 0;
    }
    const Header header{opcode, size};
    std::memcpy(ring.get() + offset, &header, sizeof(header));
    reserved_bytes = size;
    if (prefetch_ahead) {
        static_assert((Capacity & (Capacity - 1)) == 0);
        // FAST bit 35: the lines after this packet, as bbport's PrefetchAhead (+384, +448).
        PrefetchWrite(ring.get() + ((offset + size + 384) & (Capacity - 1)));
        PrefetchWrite(ring.get() + ((offset + size + 448) & (Capacity - 1)));
    }
    return ring.get() + offset + sizeof(Header);
}
void CommandRecorder::Abandon() {
    // The unpublished header is overwritten by the next Allocate; ring padding stays valid.
    ASSERT(reserved_bytes);
    reserved_bytes = 0;
}
void CommandRecorder::Commit() {
    ASSERT(reserved_bytes);
    ++encoded_packets;
    ++committed_packets;
    encoded_bytes += reserved_bytes;
    write_position += reserved_bytes;
    reserved_bytes = 0;
    if (sample_encode) {
        encode_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - encode_start)
                         .count();
        ++encode_samples;
    }
    if (threaded && write_position - last_published >= g_publish_bytes) // FAST_RECORDER_PUBLISH_KB
        Publish();
}
void CommandRecorder::Drain(std::string_view reason, std::source_location site) {
    if (!enabled)
        return;
    const u64 end = write_position;
    u64 done = completed.load(std::memory_order_acquire);
    FastRender::NoteRecorderFlush(reason, site, done < end);
    PublishStats();
    if (done >= end)
        return;
    if (!threaded) {
        ReplayUntil(end);
        return;
    }
    Publish();
    const auto start = std::chrono::steady_clock::now();
    [[maybe_unused]] u64 spin_ns = 0;
#ifdef _MSC_VER
    if (g_drain_spin_pauses != 0) {
        for (u32 spin = 0; spin < g_drain_spin_pauses && done < end; ++spin) {
            _mm_pause();
            done = completed.load(std::memory_order_acquire);
        }
        if constexpr (Common::Log::Enabled) {
            spin_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start).count();
            FastRender::NoteRecorderSpin(spin_ns, FastRender::IsStageB());
        }
    }
#endif
    while (done < end) {
        completed.wait(done, std::memory_order_acquire);
        done = completed.load(std::memory_order_acquire);
    }
    const u64 wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count();
    FastRender::NoteRecorderWait(wait_ns);
    if constexpr (Common::Log::Enabled) {
        FastRender::NoteRecorderWaitReason(reason, site, wait_ns, spin_ns, FastRender::IsStageB());
    }
}
void CommandRecorder::ReplayUntil(u64 end) {
    const auto start = std::chrono::steady_clock::now();
    u64 position = completed.load(std::memory_order_relaxed);
    const bool census = ReplayCensusWanted(); // FAST_SKIP_EMPTY_FINISH_VERIFY only
    // Configure allocates the ring before the first packet and never replaces it (a control
    // handler replayed below cannot either): one load, not one per command.
    std::byte* const base = ring.get();
    while (position < end) {
        auto* at = base + position % Capacity;
        const auto header = *reinterpret_cast<const Header*>(at);
        ASSERT(header.size && header.size <= Capacity);
        if (header.opcode != RecorderOpcode::Padding) {
            if (census && header.opcode != RecorderOpcode::Control) {
                // FAST bit 56: a submit (a control packet) resets the census in SubmitLocked.
                ++replay_census.commands;
                if (header.opcode == RecorderOpcode::writeTimestamp2 ||
                    header.opcode == RecorderOpcode::resetQueryPool)
                    ++replay_census.stamps;
            }
            Replay(header.opcode, at + sizeof(Header));
        }
        position += header.size;
    }
    FastRender::NoteRecorderReplay(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
    completed.store(position, std::memory_order_release);
    completed.notify_all();
}

// FAST bit 35 (RecorderBindFilter). Vulkan keeps pipelines, vertex and index buffers, dynamic
// vertex input and push constants for the rest of the command buffer; the filter also forgets
// them at every render pass and raw access. Binding another pipeline forgets what that pipeline's
// static state or layout may replace: dynamic vertex input, dynamic strides, push constants.
// Ours encodes fixed POD packets already (bbport records closures, vk_scheduler.h:601-692);
// what remains per command is the Allocate/Commit and the replay's Vulkan call, so a command that
// changes nothing is dropped before either.
template <class Match>
const char* CommandRecorder::FindBound(RecorderBindKind kind, u64 at, RecorderOpcode opcode,
                                       bool follows_pipeline, const Match& match) const {
    // The ring as replay reads it: the tracked command is the first packet at its position (after
    // ring padding), equals the skipped call, and nothing encoded since replaced it. Replay
    // rewrites only descriptor payloads in place, never the headers or packets read here.
    if (at < segment_start || write_position - at > Capacity)
        return "tracked position outside this command buffer";
    bool found = false;
    vk::PipelineBindPoint point{};
    vk::PipelineLayout layout{};
    for (u64 position = at; position < write_position;) {
        const std::byte* packet = ring.get() + position % Capacity;
        Header header;
        std::memcpy(&header, packet, sizeof(header));
        if (!header.size || header.size > Capacity)
            return "corrupt packet header";
        position += header.size;
        const std::byte* data = packet + sizeof(Header);
        if (header.opcode == RecorderOpcode::Padding)
            continue;
        if (!found) {
            if (header.opcode != opcode || !match(data))
                return "the tracked packet differs from the call";
            found = true;
            if (opcode == RecorderOpcode::bindPipeline)
                point = reinterpret_cast<const RecorderPackets::bindPipeline*>(data)->bindPoint;
            else if (opcode == RecorderOpcode::PushConstants)
                layout = reinterpret_cast<const PushPacket*>(data)->layout;
            continue;
        }
        if (header.opcode == RecorderOpcode::BeginRendering)
            return "a render pass began after it";
        if (header.opcode == RecorderOpcode::bindPipeline) {
            const auto& bound = *reinterpret_cast<const RecorderPackets::bindPipeline*>(data);
            if (kind == RecorderBindKind::Pipeline ? bound.bindPoint == point : follows_pipeline)
                return "a pipeline was bound after it";
            continue;
        }
        if (kind == RecorderBindKind::Push &&
            ((header.opcode == RecorderOpcode::PushDescriptors &&
              reinterpret_cast<const DescriptorPacket*>(data)->layout != layout) ||
             (header.opcode == RecorderOpcode::bindDescriptorSets &&
              reinterpret_cast<const RecorderPackets::bindDescriptorSets*>(data)->layout !=
                  layout)))
            return "descriptors were bound with another layout after it";
        if (BindKindOf(header.opcode) == kind)
            return "a later command replaced it";
    }
    return found ? nullptr : "the tracked packet is missing";
}
template <class Match>
bool CommandRecorder::ConfirmSkip(RecorderBindKind kind, u64 at, RecorderOpcode opcode,
                                  bool follows_pipeline, const Match& match) {
    const u64 every = BindVerifyEvery();
    if (every && ++bind_verify_sequence % every == 0) {
        ++bind_verified;
        if (const char* failure = FindBound(kind, at, opcode, follows_pipeline, match)) {
            ++bind_mismatches;
            LOG_ERROR(Render_Vulkan, "FAST bind_filter: {} skip not confirmed by the ring ({}); "
                                     "encoded instead",
                      kBindKindNames[static_cast<size_t>(kind)], failure);
            return false; // the caller encodes it, as with the filter off
        }
    }
    ++bind_hits[static_cast<size_t>(kind)];
    NoteBindDecision();
    return true;
}
void CommandRecorder::NoteBindMiss(RecorderBindKind kind) {
    ++bind_misses[static_cast<size_t>(kind)];
    NoteBindDecision();
}
void CommandRecorder::NoteBindDecision() {
    // About five decisions per draw: one line every ~150 frames at ~1400 draws per frame.
    if ((++bind_decisions & ((u64{1} << 20) - 1)) != 0)
        return;
    u64 hits = 0, misses = 0;
    for (size_t i = 0; i < bind_hits.size(); ++i) {
        hits += bind_hits[i];
        misses += bind_misses[i];
    }
    const auto hit = [this](RecorderBindKind kind) { return bind_hits[static_cast<size_t>(kind)]; };
    const auto miss = [this](RecorderBindKind kind) {
        return bind_misses[static_cast<size_t>(kind)];
    };
    LOG_INFO(Render_Vulkan,
             "FAST bind_filter: hits={} misses={} pipeline={}/{} vertex_input={}/{} vertex={}/{} "
             "index={}/{} push={}/{} (hits/misses) verified={} mismatches={} prefetch={}",
             hits, misses, hit(RecorderBindKind::Pipeline), miss(RecorderBindKind::Pipeline),
             hit(RecorderBindKind::VertexInput), miss(RecorderBindKind::VertexInput),
             hit(RecorderBindKind::VertexBuffers), miss(RecorderBindKind::VertexBuffers),
             hit(RecorderBindKind::IndexBuffer), miss(RecorderBindKind::IndexBuffer),
             hit(RecorderBindKind::Push), miss(RecorderBindKind::Push), bind_verified,
             bind_mismatches, prefetch_ahead);
}
bool CommandRecorder::SkipPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) {
    const size_t slot = point == vk::PipelineBindPoint::eGraphics  ? 0
                        : point == vk::PipelineBindPoint::eCompute ? 1
                                                                   : binds.pipelines.size();
    if (slot < binds.pipelines.size() && pipeline && binds.pipelines[slot].handle == pipeline &&
        ConfirmSkip(RecorderBindKind::Pipeline, binds.pipelines[slot].at,
                    RecorderOpcode::bindPipeline, false, [&](const std::byte* data) {
                        const auto& p =
                            *reinterpret_cast<const RecorderPackets::bindPipeline*>(data);
                        return p.bindPoint == point && p.pipeline == pipeline;
                    }))
        return true;
    NoteBindMiss(RecorderBindKind::Pipeline);
    // Another pipeline: state it has static replaces dynamic vertex input and strides, and other
    // push constant ranges in its layout disturb the pushed values.
    binds.input.valid = false;
    binds.push.valid = false;
    if (binds.vertices.strides)
        binds.vertices.valid = false;
    if (slot < binds.pipelines.size())
        binds.pipelines[slot] = {pipeline, write_position};
    return false;
}
bool CommandRecorder::SkipIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset,
                                      vk::IndexType type) {
    auto& index = binds.index;
    if (index.valid && index.buffer == buffer && index.offset == offset && index.type == type &&
        ConfirmSkip(RecorderBindKind::IndexBuffer, index.at, RecorderOpcode::bindIndexBuffer,
                    false, [&](const std::byte* data) {
                        const auto& p =
                            *reinterpret_cast<const RecorderPackets::bindIndexBuffer*>(data);
                        return p.buffer == buffer && p.offset == offset && p.indexType == type;
                    }))
        return true;
    NoteBindMiss(RecorderBindKind::IndexBuffer);
    index = {buffer, offset, type, write_position, true};
    return false;
}
bool CommandRecorder::SkipVertexBuffers(u32 first, const vk::ArrayProxy<const vk::Buffer>& buffers,
                                        const vk::ArrayProxy<const vk::DeviceSize>& offsets,
                                        const vk::ArrayProxy<const vk::DeviceSize>& sizes,
                                        const vk::ArrayProxy<const vk::DeviceSize>& strides,
                                        bool two) {
    auto& v = binds.vertices;
    if (v.valid && v.two == two && v.first == first &&
        SameArray(buffers, v.buffer.data(), v.count) &&
        SameArray(offsets, v.offset.data(), v.count) && SameArray(sizes, v.size.data(), v.sizes) &&
        SameArray(strides, v.stride.data(), v.strides) &&
        ConfirmSkip(RecorderBindKind::VertexBuffers, v.at,
                    two ? RecorderOpcode::bindVertexBuffers2 : RecorderOpcode::bindVertexBuffers,
                    v.strides != 0, [&](const std::byte* data) {
                        if (two) {
                            const auto& p =
                                *reinterpret_cast<const RecorderPackets::bindVertexBuffers2*>(data);
                            const std::byte* tail = data + Aligned(sizeof(p));
                            const auto b = Load<vk::Buffer>(tail, p.buffersCount);
                            const auto o = Load<vk::DeviceSize>(tail, p.offsetsCount);
                            const auto s = Load<vk::DeviceSize>(tail, p.sizesCount);
                            const auto t = Load<vk::DeviceSize>(tail, p.stridesCount);
                            return p.firstBinding == first &&
                                   SameArray(buffers, b.data(), b.size()) &&
                                   SameArray(offsets, o.data(), o.size()) &&
                                   SameArray(sizes, s.data(), s.size()) &&
                                   SameArray(strides, t.data(), t.size());
                        }
                        const auto& p =
                            *reinterpret_cast<const RecorderPackets::bindVertexBuffers*>(data);
                        const std::byte* tail = data + Aligned(sizeof(p));
                        const auto b = Load<vk::Buffer>(tail, p.buffersCount);
                        const auto o = Load<vk::DeviceSize>(tail, p.offsetsCount);
                        return p.firstBinding == first && SameArray(buffers, b.data(), b.size()) &&
                               SameArray(offsets, o.data(), o.size()) && sizes.empty() &&
                               strides.empty();
                    }))
        return true;
    NoteBindMiss(RecorderBindKind::VertexBuffers);
    constexpr u32 Max = RecorderBoundVertices::Max;
    v.valid = buffers.size() <= Max && offsets.size() == buffers.size() && sizes.size() <= Max &&
              strides.size() <= Max;
    if (!v.valid)
        return false;
    v.two = two;
    v.first = first;
    v.count = buffers.size();
    v.sizes = sizes.size();
    v.strides = strides.size();
    v.at = write_position;
    std::copy(buffers.begin(), buffers.end(), v.buffer.begin());
    std::copy(offsets.begin(), offsets.end(), v.offset.begin());
    std::copy(sizes.begin(), sizes.end(), v.size.begin());
    std::copy(strides.begin(), strides.end(), v.stride.begin());
    return false;
}
bool CommandRecorder::SkipVertexInput(
    const vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT>& bindings,
    const vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT>& attributes) {
    auto& in = binds.input;
    if (in.valid && SameArray(bindings, in.binding.data(), in.bindings) &&
        SameArray(attributes, in.attribute.data(), in.attributes) &&
        ConfirmSkip(RecorderBindKind::VertexInput, in.at, RecorderOpcode::setVertexInputEXT, true,
                    [&](const std::byte* data) {
                        const auto& p =
                            *reinterpret_cast<const RecorderPackets::setVertexInputEXT*>(data);
                        const std::byte* tail = data + Aligned(sizeof(p));
                        const auto b =
                            Load<vk::VertexInputBindingDescription2EXT>(tail, p.bindingsCount);
                        const auto a =
                            Load<vk::VertexInputAttributeDescription2EXT>(tail, p.attributesCount);
                        return SameArray(bindings, b.data(), b.size()) &&
                               SameArray(attributes, a.data(), a.size());
                    }))
        return true;
    NoteBindMiss(RecorderBindKind::VertexInput);
    in.valid = bindings.size() <= RecorderBoundInput::Max &&
               attributes.size() <= RecorderBoundInput::Max;
    if (!in.valid)
        return false;
    in.bindings = bindings.size();
    in.attributes = attributes.size();
    in.at = write_position;
    std::copy(bindings.begin(), bindings.end(), in.binding.begin());
    std::copy(attributes.begin(), attributes.end(), in.attribute.begin());
    return false;
}
bool CommandRecorder::SkipPushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                        u32 offset, u32 size, const void* values) {
    auto& push = binds.push;
    if (push.valid && push.layout == layout && push.stages == stages && push.offset == offset &&
        push.size == size && std::memcmp(push.bytes.data(), values, size) == 0 &&
        ConfirmSkip(RecorderBindKind::Push, push.at, RecorderOpcode::PushConstants, true,
                    [&](const std::byte* data) {
                        const auto& p = *reinterpret_cast<const PushPacket*>(data);
                        return p.layout == layout && p.stages == stages && p.offset == offset &&
                               p.size == size &&
                               std::memcmp(data + Aligned(sizeof(p)), values, size) == 0;
                    }))
        return true;
    NoteBindMiss(RecorderBindKind::Push);
    push.valid = size <= push.bytes.size();
    if (!push.valid)
        return false;
    push.layout = layout;
    push.stages = stages;
    push.offset = offset;
    push.size = size;
    push.at = write_position;
    std::memcpy(push.bytes.data(), values, size);
    return false;
}

#include "video_core/renderer_vulkan/vk_recorder_encode.inc"

void RecorderProxy::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                  u32 offset, u32 size, const void* values) const {
    recorder.ForgetHeldPush(); // FAST_CPU_PUSH_SPLIT: this push may change what it holds
    if (!recorder.Enabled()) {
        recorder.Direct().pushConstants(layout, stages, offset, size, values);
        return;
    }
    if (recorder.BindFilterOn() && recorder.SkipPushConstants(layout, stages, offset, size, values))
        return;
    recorder.EncodePush(layout, stages, offset, size, values);
}
void CommandRecorder::EncodePush(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                 u32 offset, u32 size, const void* values) {
    auto* data = Allocate(RecorderOpcode::PushConstants, Aligned(sizeof(PushPacket)) + size);
    const PushPacket packet{layout, stages, offset, size};
    std::memcpy(data, &packet, sizeof(packet));
    std::memcpy(data + Aligned(sizeof(packet)), values, size);
    Commit();
}
void RecorderProxy::pushConstantsSplit(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                       u32 split, u32 size, const void* values) const {
    if (!recorder.SplitPushes() || !recorder.Enabled()) {
        pushConstants(layout, stages, 0, size, values); // the whole block, as before
        return;
    }
    recorder.PushSplit(layout, stages, split, size, values);
}
// FAST_CPU_PUSH_SPLIT. Push constant values last for the rest of the command buffer, and a pipeline
// reads those pushed through any layout with the same ranges (Vulkan's "compatible for push
// constants"): every native graphics layout has the one range 0..size, so the block one draw
// pushed is still there for the next, whatever pipelines are bound between them. Everything else
// that could change it passes here first: another push (RecorderProxy::pushConstants), a raw
// borrow, a control packet whose replay may record and a new command buffer forget the held
// block. Replay keeps the encoded order, so the held block is what the command buffer holds at
// this point of the stream.
void CommandRecorder::PushSplit(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 split,
                                u32 size, const void* values) {
    const auto* bytes = static_cast<const std::byte*>(values);
    if (!HeldPush::Fits(split, size)) {
        ForgetHeldPush(); // not a block this holds: a plain push
        if (!(bind_filter && SkipPushConstants(layout, stages, 0, size, values)))
            EncodePush(layout, stages, 0, size, values);
        return;
    }
    const u32 mask = static_cast<u32>(stages);
    if (push_split_verify) [[unlikely]]
        VerifyPushSplit(mask, split, size, bytes);
    const HeldPush::Encode encode = held_push.Next(mask, split, size, bytes);
    if constexpr (Common::Log::Enabled) {
        ++split_calls;
        split_tails += encode != HeldPush::Encode::Whole;
        split_wholes += encode == HeldPush::Encode::None;
        if ((split_calls & ((u64{1} << 19) - 1)) == 0)
            LOG_INFO(Render_Vulkan,
                     "FAST_CPU_PUSH_SPLIT: calls={} tail_held={} whole_held={} misses={} "
                     "verified={} mismatches={} forgets_needed={} forgets_spare={} verify={}",
                     split_calls, split_tails, split_wholes, split_calls - split_tails,
                     split_verified, split_mismatches, forgets_needed, forgets_spare,
                     push_split_verify);
    }
    // Bit 35 does not track these packets (the held block filters them instead): its tracked
    // push must not match a later one.
    binds.push.valid = false;
    if (encode == HeldPush::Encode::Whole) {
        held_at = write_position; // the command buffer holds the whole block from this packet
        held_raw = raw_borrows;
    } else if (!push_split_verify) {
        if (encode == HeldPush::Encode::Head)
            EncodePush(layout, stages, 0, split, values); // the head alone
        return;
    }
    // The whole block (verify: every time, while the held block keeps what production holds).
    EncodePush(layout, stages, 0, size, values);
}
void CommandRecorder::VerifyPushSplit(u32 stages, u32 split, u32 size, const std::byte* values) {
    // Production's decision (the held block is kept as it would be, although verify pushes every
    // block whole): a leave-out must be what the ring shows the command buffer holding.
    const HeldPush::Encode encode = held_push.Peek(stages, split, size, values);
    if (encode != HeldPush::Encode::Whole) {
        ++split_verified;
        const char* failure =
            CheckHeldPush(held_push, held_at, held_raw, encode == HeldPush::Encode::None, values);
        if (failure && ++split_mismatches <= 16)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_PUSH_SPLIT verify: {} would be left out, but {}; pushed whole",
                      encode == HeldPush::Encode::None ? "the block" : "the tail", failure);
    }
    // The shadow, forgotten only by a new command buffer: where production pushes the whole
    // block again after a forget, does the ring show a reason (needed) or not (spare)?
    const HeldPush::Encode shadow = verify_held.Peek(stages, split, size, values);
    if (encode == HeldPush::Encode::Whole && shadow != HeldPush::Encode::Whole) {
        const bool needed = CheckHeldPush(verify_held, verify_at, verify_raw,
                                          shadow == HeldPush::Encode::None, values) != nullptr;
        ++(needed ? forgets_needed : forgets_spare);
    }
    if (shadow == HeldPush::Encode::Whole) {
        verify_at = write_position; // PushSplit encodes the whole block next
        verify_raw = raw_borrows;
    }
    verify_held.Next(stages, split, size, values);
}
// The ring as replay reads it, from the packet that pushed the held block: that packet is the
// whole block with this tail, every later push is PushSplit's own (a head, or the whole block with
// the same tail), no control packet that may record and no raw borrow came between, and with
// `head` the last push carried this head. Pushes do not depend on render passes or pipeline binds.
const char* CommandRecorder::CheckHeldPush(const HeldPush& held, u64 at, u64 raw_at, bool head,
                                           const std::byte* values) const {
    if (raw_borrows != raw_at)
        return "a raw borrow came after the held packet";
    if (at < segment_start || write_position - at > Capacity)
        return "the held packet is outside this command buffer";
    const std::byte* last = nullptr; // the values of the last push so far
    for (u64 position = at; position < write_position;) {
        const std::byte* packet = ring.get() + position % Capacity;
        Header header;
        std::memcpy(&header, packet, sizeof(header));
        if (!header.size || header.size > Capacity)
            return "corrupt packet header";
        position += header.size;
        const std::byte* data = packet + sizeof(Header);
        if (header.opcode == RecorderOpcode::Padding)
            continue;
        if (header.opcode == RecorderOpcode::Control) {
            if (reinterpret_cast<const ControlPacket*>(data)->records)
                return "a control packet that may record came after the held packet";
            continue;
        }
        if (header.opcode != RecorderOpcode::PushConstants) {
            if (!last)
                return "the held packet is not a push";
            continue;
        }
        const auto& p = *reinterpret_cast<const PushPacket*>(data);
        const std::byte* pushed = data + Aligned(sizeof(p));
        const bool all = p.size == held.size;
        if (static_cast<u32>(p.stages) != held.stages || p.offset != 0 ||
            (!all && (!last || p.size != held.split)))
            return last ? "another push came after the held packet"
                        : "the held packet is not the whole block";
        if (all &&
            std::memcmp(pushed + held.split, values + held.split, held.size - held.split) != 0)
            return last ? "a later push changed the tail" : "the held packet pushed another tail";
        last = pushed;
    }
    if (!last)
        return "the held packet is missing";
    if (head && std::memcmp(last, values, held.split) != 0)
        return "the last push carried another head";
    return nullptr;
}
void RecorderProxy::pushDescriptorSetKHR(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    const vk::ArrayProxy<const vk::WriteDescriptorSet>& writes) const {
    if (!recorder.Enabled()) {
        recorder.Direct().pushDescriptorSetKHR(bind_point, layout, set, writes);
        return;
    }
    if (recorder.BindFilterOn())
        recorder.NoteDescriptorLayout(layout);
    u32 bytes =
        Aligned(sizeof(DescriptorPacket)) + Aligned(writes.size() * sizeof(vk::WriteDescriptorSet));
    for (const auto& w : writes) {
        const u32 n = DescriptorBytes(w.descriptorType, w.descriptorCount);
        if (w.pNext || !n) {
            Raw("descriptor_extension").pushDescriptorSetKHR(bind_point, layout, set, writes);
            return;
        }
        bytes += Aligned(n);
    }
    auto* data = recorder.Allocate(RecorderOpcode::PushDescriptors, bytes);
    const DescriptorPacket packet{layout, bind_point, set, writes.size()};
    std::memcpy(data, &packet, sizeof(packet));
    auto* copies = reinterpret_cast<vk::WriteDescriptorSet*>(data + Aligned(sizeof(packet)));
    auto* tail = reinterpret_cast<std::byte*>(copies) + Aligned(writes.size() * sizeof(*copies));
    for (u32 i = 0; i < writes.size(); ++i) {
        copies[i] = writes.data()[i];
        copies[i].pNext = nullptr;
        copies[i].pBufferInfo = nullptr;
        copies[i].pImageInfo = nullptr;
        copies[i].pTexelBufferView = nullptr;
        const u32 n = DescriptorBytes(copies[i].descriptorType, copies[i].descriptorCount);
        std::memcpy(tail, DescriptorData(writes.data()[i]), n);
        tail += Aligned(n);
    }
    recorder.Commit();
}
void RecorderProxy::pushDescriptorSetBulk(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    const vk::ArrayProxy<const vk::WriteDescriptorSet>& writes,
    std::span<const vk::DescriptorBufferInfo> buffers,
    std::span<const vk::DescriptorImageInfo> images) const {
    if (!recorder.Enabled()) {
        recorder.Direct().pushDescriptorSetKHR(bind_point, layout, set, writes);
        return;
    }
    if (recorder.BindFilterOn())
        recorder.NoteDescriptorLayout(layout);
    static const u64 verify_every = [] {
        const char* value = std::getenv("FAST_PUSH_BULK_VERIFY");
        return value ? std::strtoull(value, nullptr, 0) : 0ULL;
    }();
    struct Stats {
        u64 hits{}, misses{}, verified{}, bytes{};
    };
    thread_local Stats stats;
    const auto report = [] {
        if (((stats.hits + stats.misses) & 65535) == 0)
            LOG_INFO(Render_Vulkan, "FAST push_bulk: hits={} misses={} verified={} bytes={}",
                     stats.hits, stats.misses, stats.verified, stats.bytes);
    };
    // No sizing pass over the writes: the packet size follows from the three array sizes.
    const u32 count = writes.size();
    const u32 buffer_at = Aligned(sizeof(DescriptorPacket)) +
                          Aligned(size_t{count} * sizeof(vk::WriteDescriptorSet));
    const u32 image_at = buffer_at + Aligned(buffers.size_bytes());
    const u32 bytes = image_at + Aligned(images.size_bytes());
    auto* data = recorder.Allocate(RecorderOpcode::PushDescriptors, bytes);
    const DescriptorPacket packet{layout, bind_point, set, count, 1};
    std::memcpy(data, &packet, sizeof(packet));
    auto* copies = reinterpret_cast<vk::WriteDescriptorSet*>(data + Aligned(sizeof(packet)));
    if (count)
        std::memcpy(copies, writes.data(), size_t{count} * sizeof(*copies));
    if (!buffers.empty())
        std::memcpy(data + buffer_at, buffers.data(), buffers.size_bytes());
    if (!images.empty())
        std::memcpy(data + image_at, images.data(), images.size_bytes());
    // One pass validates each write and turns its info pointer into a payload offset.
    for (u32 i = 0; i < count; ++i) {
        auto& w = copies[i];
        const BulkInfo kind = w.pNext ? BulkInfo::Other : BulkInfoOf(w.descriptorType);
        const size_t at = kind == BulkInfo::Buffer
                              ? OffsetWithin(w.pBufferInfo, w.descriptorCount, buffers)
                          : kind == BulkInfo::Image
                              ? OffsetWithin(w.pImageInfo, w.descriptorCount, images)
                              : NotWithin;
        if (at == NotWithin) {
            recorder.Abandon();
            ++stats.misses;
            report();
            pushDescriptorSetKHR(bind_point, layout, set, writes);
            return;
        }
        w.pImageInfo = nullptr;
        w.pBufferInfo = nullptr;
        w.pTexelBufferView = nullptr;
        if (kind == BulkInfo::Buffer)
            w.pBufferInfo =
                reinterpret_cast<const vk::DescriptorBufferInfo*>(std::uintptr_t{buffer_at} + at);
        else
            w.pImageInfo =
                reinterpret_cast<const vk::DescriptorImageInfo*>(std::uintptr_t{image_at} + at);
    }
    if (verify_every && (stats.hits + 1) % verify_every == 0) {
        ++stats.verified;
        const u32 bad = VerifyBulk(data, bytes, writes);
        if (bad != count)
            LOG_ERROR(Render_Vulkan, "FAST push_bulk: write {} of {} differs from per-write encode",
                      bad, count);
        ASSERT_MSG(bad == count, "FAST push_bulk: bulk descriptors differ from per-write encode");
    }
    ++stats.hits;
    stats.bytes += bytes;
    report();
    recorder.Commit();
}
bool RecorderProxy::pushDescriptorSetTemplate(
    vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
    vk::DescriptorUpdateTemplate update_template,
    std::span<const vk::DescriptorBufferInfo> buffers,
    std::span<const vk::DescriptorImageInfo> images, const std::byte** recorded) const {
    if (!recorder.Enabled()) {
        return false;
    }
    if (recorder.BindFilterOn())
        recorder.NoteDescriptorLayout(layout);
    // The bulk packet less its writes: ~64 bytes per write fewer to encode and replay, and no
    // write to rebase or for the driver to walk.
    const u32 payload = static_cast<u32>(PushTemplate::PayloadBytes(buffers, images));
    auto* data = recorder.Allocate(RecorderOpcode::PushDescriptors, TemplatePayloadAt + payload);
    const DescriptorPacket packet{layout, bind_point, set, payload, TemplatePush};
    std::memcpy(data, &packet, sizeof(packet));
    std::memcpy(data + TemplateAt, &update_template, sizeof(update_template));
    PushTemplate::WritePayload(data + TemplatePayloadAt, buffers, images);
    if (recorded) {
        // FAST_CPU_PUSH_TEMPLATE_VERIFY reads it back: the replay only reads this packet, and
        // the ring reuses it only after this thread allocates again.
        *recorded = data + TemplatePayloadAt;
    }
    recorder.Commit();
    return true;
}
void RecorderProxy::writeDescriptorSet(DescriptorHeap& heap, vk::DescriptorSetLayout set_layout,
                                       vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                                       u32 set, std::span<vk::WriteDescriptorSet> writes) const {
    recorder.WriteDescriptorSet(heap, set_layout, bind_point, layout, set, writes, site);
}
// FAST_CPU_DRIVER_CALLS. The heap needs one thread at a time: the replay's sets come only from
// packets, and this thread writes a set itself only while nothing is queued (the recorder off, or
// Raw drained it), so the two never overlap; publishing a packet (release) and Drain (acquire on
// `completed`) order the heap's state between them. The replay commits after the producer may have
// started a later command buffer: the heap retires a full pool at that later tick, which only
// keeps it a little longer.
void CommandRecorder::WriteDescriptorSet(DescriptorHeap& heap, vk::DescriptorSetLayout set_layout,
                                         vk::PipelineBindPoint bind_point,
                                         vk::PipelineLayout layout, u32 set,
                                         std::span<vk::WriteDescriptorSet> writes,
                                         std::source_location site) {
    const u32 encoded = Enabled() ? SetWriteCodec::EncodedBytes(writes) : 0;
    if (encoded == 0) {
        // As Pipeline::BindResources recorded it before: commit, write and bind on this thread.
        vk::CommandBuffer target = command;
        if (Enabled()) {
            // A write the packet cannot carry (an extension structure): nothing may stay queued.
            target = Raw("descriptor_extension", site);
            if constexpr (Common::Log::Enabled) {
                ++set_writes_raw;
            }
        }
        const vk::DescriptorSet desc_set = HeapWrite(heap, set_layout, writes);
        target.bindDescriptorSets(bind_point, layout, set, desc_set, {});
        if constexpr (Common::Log::Enabled) {
            ++set_writes_direct;
            ReportSetWrites();
        }
        return;
    }
    if (bind_filter)
        NoteDescriptorLayout(layout); // as bindDescriptorSets
    constexpr u32 head_at = Aligned(sizeof(DescriptorPacket));
    constexpr u32 codec_at = head_at + Aligned(sizeof(SetWriteHead));
    auto* data = Allocate(RecorderOpcode::PushDescriptors, codec_at + encoded);
    const DescriptorPacket packet{layout, bind_point, set, static_cast<u32>(writes.size()),
                                  SetWritePacket};
    std::memcpy(data, &packet, sizeof(packet));
    const SetWriteHead head{&heap, set_layout,
                            g_driver_calls_verify ? SetWriteCodec::Hash(writes) : 0};
    std::memcpy(data + head_at, &head, sizeof(head));
    SetWriteCodec::Encode(data + codec_at, writes);
    Commit();
    if constexpr (Common::Log::Enabled) {
        ++set_writes_encoded;
        set_write_bytes += codec_at + encoded;
        ReportSetWrites();
    }
}
void CommandRecorder::ReportSetWrites() {
    if (((set_writes_encoded + set_writes_direct) & 0xffff) != 0)
        return;
    LOG_INFO(Render_Vulkan,
             "FAST_CPU_DRIVER_CALLS: descriptor sets written by the replay={} ({} bytes/set "
             "encoded), by the recording thread={} (raw drains for them {})",
             set_writes_encoded, set_writes_encoded ? set_write_bytes / set_writes_encoded : 0,
             set_writes_direct, set_writes_raw);
}
void CommandRecorder::ReplaySetWrite(const std::byte* data) {
    const auto& p = *reinterpret_cast<const DescriptorPacket*>(data);
    SetWriteHead head;
    std::memcpy(&head, data + Aligned(sizeof(p)), sizeof(head));
    replay_writes.resize(p.count);
    SetWriteCodec::Decode(data + Aligned(sizeof(p)) + Aligned(sizeof(head)), p.count,
                          replay_writes.data());
    if (g_driver_calls_verify) {
        ++set_write_verified;
        const u64 hash = SetWriteCodec::Hash(replay_writes);
        if (hash != head.hash) {
            if (++set_write_mismatches <= 16 || (set_write_mismatches & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_DRIVER_CALLS verify: set of layout {:#x} ({} writes) replays "
                          "hash {:#x}, the caller wrote {:#x}; mismatch #{}",
                          std::bit_cast<u64>(head.set_layout), p.count, hash, head.hash,
                          set_write_mismatches);
            }
        }
        if ((set_write_verified & 0xffff) == 0) {
            LOG_INFO(Render_Vulkan, "FAST_CPU_DRIVER_CALLS verify: verified={} mismatches={}",
                     set_write_verified, set_write_mismatches);
        }
    }
    const vk::DescriptorSet desc_set = HeapWrite(*head.heap, head.set_layout, replay_writes);
    command.bindDescriptorSets(p.bind_point, p.layout, p.set, desc_set, {});
}
void RecorderProxy::pipelineBarrier2(const vk::DependencyInfo& info) const {
    if (!recorder.Enabled()) {
        recorder.Direct().pipelineBarrier2(info);
        return;
    }
    const vk::ArrayProxy<const vk::MemoryBarrier2> memory{info.memoryBarrierCount,
                                                          info.pMemoryBarriers};
    const vk::ArrayProxy<const vk::BufferMemoryBarrier2> buffers{info.bufferMemoryBarrierCount,
                                                                 info.pBufferMemoryBarriers};
    const vk::ArrayProxy<const vk::ImageMemoryBarrier2> images{info.imageMemoryBarrierCount,
                                                               info.pImageMemoryBarriers};
    if (info.pNext || HasNext(memory) || HasNext(buffers) || HasNext(images)) {
        Raw("barrier_extension").pipelineBarrier2(info);
        return;
    }
    auto* data = recorder.Allocate(RecorderOpcode::Barrier2,
                                   Aligned(sizeof(Barrier2Packet)) +
                                       Aligned(memory.size() * sizeof(vk::MemoryBarrier2)) +
                                       Aligned(buffers.size() * sizeof(vk::BufferMemoryBarrier2)) +
                                       Aligned(images.size() * sizeof(vk::ImageMemoryBarrier2)));
    const Barrier2Packet packet{info.dependencyFlags, memory.size(), buffers.size(), images.size()};
    std::memcpy(data, &packet, sizeof(packet));
    auto* tail = data + Aligned(sizeof(packet));
    Store(tail, memory);
    Store(tail, buffers);
    Store(tail, images);
    recorder.Commit();
}
void RecorderProxy::pipelineBarrier(
    vk::PipelineStageFlags src, vk::PipelineStageFlags dst, vk::DependencyFlags flags,
    const vk::ArrayProxy<const vk::MemoryBarrier>& memory,
    const vk::ArrayProxy<const vk::BufferMemoryBarrier>& buffers,
    const vk::ArrayProxy<const vk::ImageMemoryBarrier>& images) const {
    if (!recorder.Enabled()) {
        recorder.Direct().pipelineBarrier(src, dst, flags, memory, buffers, images);
        return;
    }
    if (HasNext(memory) || HasNext(buffers) || HasNext(images)) {
        Raw("barrier_extension").pipelineBarrier(src, dst, flags, memory, buffers, images);
        return;
    }
    auto* data = recorder.Allocate(RecorderOpcode::Barrier,
                                   Aligned(sizeof(BarrierPacket)) +
                                       Aligned(memory.size() * sizeof(vk::MemoryBarrier)) +
                                       Aligned(buffers.size() * sizeof(vk::BufferMemoryBarrier)) +
                                       Aligned(images.size() * sizeof(vk::ImageMemoryBarrier)));
    const BarrierPacket packet{src, dst, flags, memory.size(), buffers.size(), images.size()};
    std::memcpy(data, &packet, sizeof(packet));
    auto* tail = data + Aligned(sizeof(packet));
    Store(tail, memory);
    Store(tail, buffers);
    Store(tail, images);
    recorder.Commit();
}
void RecorderProxy::beginRendering(const vk::RenderingInfo& info) const {
    if (!recorder.Enabled()) {
        recorder.Direct().beginRendering(info);
        return;
    }
    // FAST bit 35: nothing tracked before a render pass is carried into it.
    recorder.ResetBinds();
    const vk::ArrayProxy<const vk::RenderingAttachmentInfo> colors{info.colorAttachmentCount,
                                                                   info.pColorAttachments};
    if (info.pNext || HasNext(colors) || (info.pDepthAttachment && info.pDepthAttachment->pNext) ||
        (info.pStencilAttachment && info.pStencilAttachment->pNext)) {
        Raw("rendering_extension").beginRendering(info);
        return;
    }
    const u32 depth = info.pDepthAttachment != nullptr,
              stencil = info.pStencilAttachment != nullptr;
    auto* data = recorder.Allocate(RecorderOpcode::BeginRendering,
                                   Aligned(sizeof(RenderingPacket)) +
                                       (colors.size() + depth + stencil) *
                                           Aligned(sizeof(vk::RenderingAttachmentInfo)));
    RenderingPacket packet{info, depth, stencil};
    packet.info.pColorAttachments = nullptr;
    packet.info.pDepthAttachment = nullptr;
    packet.info.pStencilAttachment = nullptr;
    std::memcpy(data, &packet, sizeof(packet));
    auto* tail = data + Aligned(sizeof(packet));
    Store(tail, colors);
    Store(tail, vk::ArrayProxy<const vk::RenderingAttachmentInfo>{depth, info.pDepthAttachment});
    Store(tail,
          vk::ArrayProxy<const vk::RenderingAttachmentInfo>{stencil, info.pStencilAttachment});
    recorder.Commit();
}
void RecorderProxy::setBlendConstants(const float* values) const {
    if (!recorder.Enabled()) {
        recorder.Direct().setBlendConstants(values);
        return;
    }
    auto* data = recorder.Allocate(RecorderOpcode::BlendConstants, 4 * sizeof(float));
    std::memcpy(data, values, 4 * sizeof(float));
    recorder.Commit();
}
void RecorderProxy::beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
    if (!recorder.Enabled()) {
        recorder.Direct().beginDebugUtilsLabelEXT(label);
        return;
    }
    if (label.pNext) {
        Raw("label_extension").beginDebugUtilsLabelEXT(label);
        return;
    }
    const u32 size = static_cast<u32>(std::strlen(label.pLabelName)) + 1;
    auto* data = recorder.Allocate(RecorderOpcode::BeginLabel, Aligned(sizeof(LabelPacket)) + size);
    const LabelPacket packet{label.color};
    std::memcpy(data, &packet, sizeof(packet));
    std::memcpy(data + Aligned(sizeof(packet)), label.pLabelName, size);
    recorder.Commit();
}
void RecorderProxy::insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
    if (!recorder.Enabled()) {
        recorder.Direct().insertDebugUtilsLabelEXT(label);
        return;
    }
    if (label.pNext) {
        Raw("label_extension").insertDebugUtilsLabelEXT(label);
        return;
    }
    const u32 size = static_cast<u32>(std::strlen(label.pLabelName)) + 1;
    auto* data =
        recorder.Allocate(RecorderOpcode::InsertLabel, Aligned(sizeof(LabelPacket)) + size);
    const LabelPacket packet{label.color};
    std::memcpy(data, &packet, sizeof(packet));
    std::memcpy(data + Aligned(sizeof(packet)), label.pLabelName, size);
    recorder.Commit();
}
void CommandRecorder::Replay(RecorderOpcode opcode, std::byte* data) {
    switch (opcode) {
#include "video_core/renderer_vulkan/vk_recorder_replay.inc"
    case RecorderOpcode::Control: {
        const auto& p = *reinterpret_cast<const ControlPacket*>(data);
        ASSERT(control_handler);
        control_handler(control_context, p.kind, data + Aligned(sizeof(p)));
        break;
    }
    case RecorderOpcode::PushConstants: {
        const auto& p = *reinterpret_cast<const PushPacket*>(data);
        command.pushConstants(p.layout, p.stages, p.offset, p.size, data + Aligned(sizeof(p)));
        break;
    }
    case RecorderOpcode::PushDescriptors: {
        const auto& p = *reinterpret_cast<const DescriptorPacket*>(data);
        if (p.bulk == TemplatePush) {
            // FAST_CPU_PUSH_TEMPLATE. The raw entry point: Vulkan-Hpp's typed overload would
            // pass the address of its data argument instead of the payload.
            vk::DescriptorUpdateTemplate update_template;
            std::memcpy(&update_template, data + TemplateAt, sizeof(update_template));
            VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDescriptorSetWithTemplateKHR(
                static_cast<VkCommandBuffer>(command),
                static_cast<VkDescriptorUpdateTemplate>(update_template),
                static_cast<VkPipelineLayout>(p.layout), p.set, data + TemplatePayloadAt);
            break;
        }
        if (p.bulk == SetWritePacket) {
            ReplaySetWrite(data); // FAST_CPU_DRIVER_CALLS
            break;
        }
        auto* writes = reinterpret_cast<vk::WriteDescriptorSet*>(data + Aligned(sizeof(p)));
        if (p.bulk) {
            for (u32 i = 0; i < p.count; ++i)
                RebaseBulk(writes[i], data);
        } else {
            auto* tail = reinterpret_cast<std::byte*>(writes) + Aligned(p.count * sizeof(*writes));
            for (u32 i = 0; i < p.count; ++i) {
                SetDescriptorData(writes[i], tail);
                tail +=
                    Aligned(DescriptorBytes(writes[i].descriptorType, writes[i].descriptorCount));
            }
        }
        command.pushDescriptorSetKHR(p.bind_point, p.layout, p.set, {p.count, writes});
        break;
    }
    case RecorderOpcode::Barrier2: {
        const auto& p = *reinterpret_cast<const Barrier2Packet*>(data);
        const auto* tail = data + Aligned(sizeof(p));
        const auto memory = Load<vk::MemoryBarrier2>(tail, p.memory);
        const auto buffers = Load<vk::BufferMemoryBarrier2>(tail, p.buffers);
        const auto images = Load<vk::ImageMemoryBarrier2>(tail, p.images);
        command.pipelineBarrier2(vk::DependencyInfo{.dependencyFlags = p.flags,
                                                    .memoryBarrierCount = p.memory,
                                                    .pMemoryBarriers = memory.data(),
                                                    .bufferMemoryBarrierCount = p.buffers,
                                                    .pBufferMemoryBarriers = buffers.data(),
                                                    .imageMemoryBarrierCount = p.images,
                                                    .pImageMemoryBarriers = images.data()});
        break;
    }
    case RecorderOpcode::Barrier: {
        const auto& p = *reinterpret_cast<const BarrierPacket*>(data);
        const auto* tail = data + Aligned(sizeof(p));
        const auto memory = Load<vk::MemoryBarrier>(tail, p.memory);
        const auto buffers = Load<vk::BufferMemoryBarrier>(tail, p.buffers);
        const auto images = Load<vk::ImageMemoryBarrier>(tail, p.images);
        command.pipelineBarrier(p.src, p.dst, p.flags, memory, buffers, images);
        break;
    }
    case RecorderOpcode::BeginRendering: {
        const auto& p = *reinterpret_cast<const RenderingPacket*>(data);
        const auto* tail = data + Aligned(sizeof(p));
        auto info = p.info;
        info.pColorAttachments =
            Load<vk::RenderingAttachmentInfo>(tail, info.colorAttachmentCount).data();
        const auto depth = Load<vk::RenderingAttachmentInfo>(tail, p.depth);
        const auto stencil = Load<vk::RenderingAttachmentInfo>(tail, p.stencil);
        info.pDepthAttachment = p.depth ? depth.data() : nullptr;
        info.pStencilAttachment = p.stencil ? stencil.data() : nullptr;
        command.beginRendering(info);
        break;
    }
    case RecorderOpcode::BlendConstants:
        command.setBlendConstants(reinterpret_cast<const float*>(data));
        break;
    case RecorderOpcode::BeginLabel:
    case RecorderOpcode::InsertLabel: {
        const auto& p = *reinterpret_cast<const LabelPacket*>(data);
        const vk::DebugUtilsLabelEXT label{
            .pLabelName = reinterpret_cast<const char*>(data + Aligned(sizeof(p))),
            .color = p.color};
        if (opcode == RecorderOpcode::BeginLabel)
            command.beginDebugUtilsLabelEXT(label);
        else
            command.insertDebugUtilsLabelEXT(label);
        break;
    }
    default:
        UNREACHABLE_MSG("Invalid recorder opcode {}", static_cast<u32>(opcode));
    }
}
} // namespace Vulkan
