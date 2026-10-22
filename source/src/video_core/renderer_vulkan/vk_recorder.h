// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <source_location>
#include <span>
#include <string_view>
#include <thread>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_push_split.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {
enum class RecorderOpcode : u32;
class CommandRecorder;
class DescriptorHeap;

// FAST bit 35 (RecorderBindFilter): the binding state the commands encoded since the last
// command-buffer, render-pass or raw-access boundary leave bound, with the write position of the
// command that bound it (FAST_BIND_FILTER_VERIFY finds that command in the ring). A proxy call
// that would bind the same state again is not encoded.
enum class RecorderBindKind : u32 {
    Pipeline,
    VertexInput,
    VertexBuffers,
    IndexBuffer,
    Push,
    Count,
};
struct RecorderBoundPipeline {
    vk::Pipeline handle{};
    u64 at{};
};
struct RecorderBoundIndex {
    vk::Buffer buffer{};
    vk::DeviceSize offset{};
    vk::IndexType type{};
    u64 at{};
    bool valid{};
};
struct RecorderBoundVertices {
    static constexpr u32 Max = 16; // longer calls are encoded and leave nothing tracked
    u32 first{}, count{}, sizes{}, strides{};
    u64 at{};
    bool valid{}, two{}; // two: bindVertexBuffers2
    std::array<vk::Buffer, Max> buffer{};
    std::array<vk::DeviceSize, Max> offset{}, size{}, stride{};
};
struct RecorderBoundInput {
    static constexpr u32 Max = 16;
    u32 bindings{}, attributes{};
    u64 at{};
    bool valid{};
    std::array<vk::VertexInputBindingDescription2EXT, Max> binding{};
    std::array<vk::VertexInputAttributeDescription2EXT, Max> attribute{};
};
struct RecorderBoundPush {
    vk::PipelineLayout layout{};
    vk::ShaderStageFlags stages{};
    u32 offset{}, size{};
    u64 at{};
    bool valid{};
    std::array<std::byte, 256> bytes{};
};
struct RecorderBindState {
    std::array<RecorderBoundPipeline, 2> pipelines{}; // graphics, compute
    RecorderBoundIndex index{};
    RecorderBoundVertices vertices{};
    RecorderBoundInput input{};
    RecorderBoundPush push{};
    void Reset() noexcept {
        pipelines = {};
        index.valid = vertices.valid = input.valid = push.valid = false;
    }
};

// A lightweight producer handle. No Vulkan command-buffer handle escapes this API unless
// Raw() (or the compatibility conversion) first drains all preceding packets.
class RecorderProxy {
public:
    RecorderProxy(CommandRecorder& recorder, std::source_location site)
        : recorder{recorder}, site{site} {}
    vk::CommandBuffer Raw(std::string_view reason = "raw_api") const;
    operator vk::CommandBuffer() const {
        return Raw();
    }

#include "video_core/renderer_vulkan/vk_recorder_declarations.inc"

    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                       const void* values) const;
    // FAST_CPU_PUSH_SPLIT (Pipeline::BindResources): native stereo graphics push constants 0..size
    // through a layout whose one range is exactly 0..size for `stages` (every graphics layout's, so
    // the values outlive the pipeline binds between them). Bytes from `split` on
    // (Shader::VrPushData), and the whole block when it repeats, are left out while the command
    // buffer still holds them (CommandRecorder::PushSplit).
    void pushConstantsSplit(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 split,
                            u32 size, const void* values) const;
    void pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                              const vk::ArrayProxy<const vk::WriteDescriptorSet>& writes) const;
    // FAST bit 28: each write's infos lie in `buffers` or `images`; both arrays are copied once
    // (bbport vk_pipeline_common.cpp:49-90). Any other write takes pushDescriptorSetKHR.
    void pushDescriptorSetBulk(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                               u32 set, const vk::ArrayProxy<const vk::WriteDescriptorSet>& writes,
                               std::span<const vk::DescriptorBufferInfo> buffers,
                               std::span<const vk::DescriptorImageInfo> images) const;
    // FAST_CPU_PUSH_TEMPLATE (Pipeline::BulkTemplateFor, vk_push_template.h): bit 28's two info
    // arrays alone, pushed through `update_template`, whose entries read them as one payload (the
    // buffer infos, then the image infos). Returns false, encoding nothing, while commands are
    // recorded inline (the caller then pushes the writes). `recorded` (FAST_CPU_PUSH_TEMPLATE_VERIFY):
    // the payload as encoded, the bytes the replay hands vkCmdPushDescriptorSetWithTemplateKHR;
    // valid until the next command is recorded.
    bool pushDescriptorSetTemplate(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                                   u32 set, vk::DescriptorUpdateTemplate update_template,
                                   std::span<const vk::DescriptorBufferInfo> buffers,
                                   std::span<const vk::DescriptorImageInfo> images,
                                   const std::byte** recorded = nullptr) const;
    // FAST_CPU_DRIVER_CALLS (Pipeline::BindResources, a layout without push descriptors): the
    // replay commits a set from `heap`, writes `writes` into it and binds it, so the recording
    // thread makes neither driver call (SetWriteCodec). While the recorder is off or borrowed raw
    // (or for a write the packet cannot carry, after a raw drain) this thread does all three, as
    // before. Either way the writes' dstSet may become the set.
    void writeDescriptorSet(DescriptorHeap& heap, vk::DescriptorSetLayout set_layout,
                            vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                            std::span<vk::WriteDescriptorSet> writes) const;
    void pipelineBarrier2(const vk::DependencyInfo& info) const;
    void pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
                         vk::DependencyFlags flags,
                         const vk::ArrayProxy<const vk::MemoryBarrier>& memory,
                         const vk::ArrayProxy<const vk::BufferMemoryBarrier>& buffers,
                         const vk::ArrayProxy<const vk::ImageMemoryBarrier>& images) const;
    void beginRendering(const vk::RenderingInfo& info) const;
    void setBlendConstants(const float* values) const;
    void beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const;
    void insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const;
    void bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const {
        bindVertexBuffers(first, {count, buffers}, {count, offsets});
    }
    void bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const {
        bindVertexBuffers2(first, {count, buffers}, {count, offsets}, {sizes ? count : 0, sizes},
                           {strides ? count : 0, strides});
    }

private:
    CommandRecorder& recorder;
    std::source_location site;
};

// Single producer, single replay owner. Packets contain only values and owned inline
// arrays. Absolute positions avoid ABA when the byte ring wraps. R1 replays in Drain;
// R2 can publish these exact packets to a consumer without changing call sites.
class CommandRecorder {
public:
    explicit CommandRecorder(vk::CommandBuffer& command, bool allowed);
    ~CommandRecorder();
    CommandRecorder(const CommandRecorder&) = delete;
    CommandRecorder& operator=(const CommandRecorder&) = delete;
    bool Enabled() const noexcept {
        return enabled && !raw_borrowed;
    }
    void Configure(); // producer command-buffer boundary; drains when mode changes
    bool Threaded() const noexcept {
        return threaded;
    }
    using ControlHandler = void (*)(void* context, u32 kind, const std::byte* payload);
    void SetControlHandler(void* context, ControlHandler handler) {
        control_context = context;
        control_handler = handler;
    }
    // `records`: its replay may record into the command buffer (false: FAST_CPU_PUSH_SPLIT keeps
    // the held push constants across it).
    void QueueControl(u32 kind, const void* payload, u32 bytes, bool publish = true,
                      bool records = true);
    void Publish();
    void Stop();
    vk::CommandBuffer Direct() const noexcept {
        return command;
    }
    vk::CommandBuffer Raw(std::string_view reason, std::source_location site);
    /// Ends a Raw() borrow early: later proxy calls queue again instead of recording inline
    /// until submit. Raw() drained everything queued before it, so packets queued from here on
    /// replay after the raw commands. Only for a caller that knows no raw handle of this
    /// command buffer is used afterwards (FAST bit 37: the temporal provider evaluation).
    /// Producer thread only, like raw_borrowed itself.
    void ReleaseRaw() noexcept {
        raw_borrowed = false;
    }
    void Drain(std::string_view reason,
               std::source_location site = std::source_location::current());
    std::byte* Allocate(RecorderOpcode opcode, u32 bytes);
    void Commit();
    void Abandon(); // drops the reservation of the last Allocate; nothing is published

    // FAST bit 56 (SkipEmptyFinish, vk_scheduler.cpp). Producer: monotonic counts of the encoded
    // Vulkan command packets (control packets excluded) and of the Raw() borrows; while neither
    // moves, nothing reaches the command buffer through this recorder.
    u64 CommandPackets() const noexcept {
        return committed_packets - control_packets;
    }
    u64 RawBorrows() const noexcept {
        return raw_borrows;
    }
    // Replay owner: the command packets replayed into the current command buffer, and how many of
    // them were query pool resets or timestamps. Counted only while FAST_SKIP_EMPTY_FINISH_VERIFY
    // is set (zero otherwise), so the default replay loop stays as it was.
    struct ReplayCensus {
        u64 commands, stamps;
    };
    ReplayCensus ReplayedCommands() const noexcept {
        return replay_census;
    }
    void ResetReplayedCommands() noexcept {
        replay_census = {};
    }

    // FAST bit 35, latched per command buffer in Configure (encoding only). Skip* returns true
    // when the call would bind what the encoded stream already binds; otherwise it records the
    // call as the bound state and the caller encodes it.
    bool BindFilterOn() const noexcept {
        return bind_filter;
    }
    bool SkipPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline);
    bool SkipIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type);
    bool SkipVertexBuffers(u32 first, const vk::ArrayProxy<const vk::Buffer>& buffers,
                           const vk::ArrayProxy<const vk::DeviceSize>& offsets,
                           const vk::ArrayProxy<const vk::DeviceSize>& sizes,
                           const vk::ArrayProxy<const vk::DeviceSize>& strides, bool two);
    bool SkipVertexInput(
        const vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT>& bindings,
        const vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT>& attributes);
    bool SkipPushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset,
                           u32 size, const void* values);
    // Descriptor sets bound or pushed with another layout than the tracked push constants'.
    void NoteDescriptorLayout(vk::PipelineLayout layout) noexcept {
        if (binds.push.valid && binds.push.layout != layout)
            binds.push.valid = false;
    }
    void ResetBinds() noexcept {
        if (bind_filter)
            binds.Reset();
    }

    // Encodes one push constants packet (Enabled() only; bit 35's filter is up to the caller).
    void EncodePush(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                    const void* values);
    // FAST_CPU_PUSH_SPLIT, latched per command buffer in Configure (encoding only).
    bool SplitPushes() const noexcept {
        return push_split;
    }
    // FAST_CPU_PUSH_SPLIT (RecorderProxy::pushConstantsSplit, Enabled() only): encodes only what
    // the command buffer does not hold yet of the block: the head (bytes 0..split) when only the
    // tail repeats, nothing when all of it does.
    void PushSplit(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 split, u32 size,
                   const void* values);
    // Any other push, a raw borrow, a control packet that may record or a new command buffer.
    void ForgetHeldPush() noexcept {
        held_push.Forget();
    }

    // FAST_CPU_DRIVER_CALLS (RecorderProxy::writeDescriptorSet): encodes the set's commit, write
    // and bind for the replay (ReplaySetWrite) while Enabled(); records them on this thread
    // otherwise, and after a raw drain for a write the packet cannot carry.
    void WriteDescriptorSet(DescriptorHeap& heap, vk::DescriptorSetLayout set_layout,
                            vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                            std::span<vk::WriteDescriptorSet> writes, std::source_location site);

private:
    // FAST_CPU_DRIVER_CALLS: the replay of WriteDescriptorSet's packet.
    void ReplaySetWrite(const std::byte* data);
    void ReportSetWrites();
    // FAST_CPU_PUSH_SPLIT_VERIFY, before PushSplit decides: checks the held block's leave-out
    // against the ring, and counts the forgets the shadow block finds needed or spare.
    void VerifyPushSplit(u32 stages, u32 split, u32 size, const std::byte* values);
    // Why the ring from the packet at `at` (raw borrows then: `raw_at`) does not show the command
    // buffer holding `held`'s tail of `values` (and, with `head`, the head); null when it does.
    const char* CheckHeldPush(const HeldPush& held, u64 at, u64 raw_at, bool head,
                              const std::byte* values) const;
    template <class Match>
    bool ConfirmSkip(RecorderBindKind kind, u64 at, RecorderOpcode opcode, bool follows_pipeline,
                     const Match& match);
    template <class Match>
    const char* FindBound(RecorderBindKind kind, u64 at, RecorderOpcode opcode,
                          bool follows_pipeline, const Match& match) const;
    void NoteBindMiss(RecorderBindKind kind);
    void NoteBindDecision();
    void Replay(RecorderOpcode opcode, std::byte* data);
    void ReplayUntil(u64 end);
    void Run();
    void PublishStats();
    static constexpr u32 Capacity = 8 * 1024 * 1024;
    // Cache lines by writer, as bbport keeps its pipe's cursors (vk_draw_pipe.h:255-258). The
    // first line is read by the producer and the replay alike and written only while the replay is
    // idle or not yet started (construction, Configure's mode change after its drain, Stop).
    vk::CommandBuffer& command;
    std::unique_ptr<std::byte[]> ring; // allocated by Configure before any packet, never replaced
    void* control_context{};
    ControlHandler control_handler{};
    const bool allowed;
    bool enabled{};
    bool threaded{};
    std::atomic<bool> stopping{};
    std::jthread worker;
    // Written together by the producer's Publish; read by the replay when it runs dry.
    alignas(64) std::atomic<u64> published{};
    std::atomic<u32> wake_sequence{};
    // Written by the replay once per batch; read by the producer's Allocate and Drain.
    alignas(64) std::atomic<u64> completed{};
    alignas(64) ReplayCensus replay_census{}; // FAST bit 56, replay owner
    // FAST_CPU_DRIVER_CALLS, replay owner: the decoded writes (capacity kept from packet to
    // packet) and, with _VERIFY, the packets compared and those that differed.
    std::vector<vk::WriteDescriptorSet> replay_writes;
    u64 set_write_verified{}, set_write_mismatches{};
    // Producer only from here: Allocate and Commit write these for every command, on lines the
    // replay neither reads nor spins on.
    // A raw handle may remain in a caller local until submit. Keep intervening
    // proxy calls inline so no queued command overtakes later raw calls.
    alignas(64) bool raw_borrowed{};
    u64 write_position{};
    u64 last_published{};
    u64 encoded_packets{}, encoded_bytes{}, encode_ns{}, encode_samples{};
    u64 committed_packets{}, control_packets{}, raw_borrows{}; // FAST bit 56, producer
    u64 sample_sequence{};
    bool sample_encode{};
    std::chrono::steady_clock::time_point encode_start{};
    u32 reserved_bytes{};
    // FAST bit 35 (producer-owned, like write_position).
    bool bind_filter{}, prefetch_ahead{};
    u64 segment_start{}; // write_position at the command buffer's Configure
    RecorderBindState binds{};
    std::array<u64, static_cast<size_t>(RecorderBindKind::Count)> bind_hits{}, bind_misses{};
    u64 bind_decisions{}, bind_verify_sequence{}, bind_verified{}, bind_mismatches{};
    // FAST_CPU_PUSH_SPLIT (producer-owned, like binds): the block PushSplit's packets left in the
    // command buffer. `held_at` is the packet that pushed all of it, `held_raw` the raw borrows
    // then (the verify reads the ring from there).
    bool push_split{}, push_split_verify{};
    HeldPush held_push{};
    u64 held_at{}, held_raw{};
    // FAST_CPU_PUSH_SPLIT_VERIFY: the same block forgotten only by a new command buffer.
    HeldPush verify_held{};
    u64 verify_at{}, verify_raw{};
    u64 split_calls{}, split_tails{}, split_wholes{}, split_verified{}, split_mismatches{};
    u64 forgets_needed{}, forgets_spare{};
    // FAST_CPU_DRIVER_CALLS (producer-owned): sets written by the replay, the bytes their packets
    // took, sets written by the recording thread (recorder off or raw) and raw drains it forced.
    u64 set_writes_encoded{}, set_write_bytes{}, set_writes_direct{}, set_writes_raw{};
};
} // namespace Vulkan
