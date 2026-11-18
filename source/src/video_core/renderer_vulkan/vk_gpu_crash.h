// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What the GPU was doing when the device was lost (VK_ERROR_DEVICE_LOST). A player's GPU crash
// (2026-11-10, RTX 4080, as the first load began: "a check failed at vk_scheduler.cpp:2253", then
// a breakpoint in nvoglv64.dll) left nothing but crash.txt's two lines. On the first lost device,
// Report writes gpu-crash.txt beside the log (every build, logs off too):
// - the game (id, version, add-on), the GPU and its driver;
// - the driver's own report (VK_EXT_device_fault): the faulting GPU address and its kind;
// - the last marker each queue's GPU work reached, at its start and at its end
//   (VK_NV_device_diagnostic_checkpoints): which command buffer (tick), render pass (size,
//   attachments, its draws' shaders) or dispatch (shader hash, groups) was running;
// - the last game submits and sparse binds, VRAM per heap;
// - Windows' display driver events of the last minutes (NVIDIA's Xid, a TDR's 4101).
// crash.txt gets one line pointing at it; logs-on builds log every line too.
//
// The markers cost a little on every pass and dispatch: on in logs-on builds (VR_GPU_CHECKPOINTS=0
// turns them off), off in release unless VR_GPU_CHECKPOINTS=1. The fault report is a query made
// only after the loss: on wherever the device has it.
//
// A freeze (the same player, later: no crash, the game just stops as the load begins) reaches no
// lost device: a watchdog (logs-on builds, or VR_FREEZE_WATCH=1; =0 off) wakes once a second and,
// when the game has submitted nothing for 15 s, writes a freeze report to gpu-crash.txt (how far
// the GPU got on the game's timeline, the last work recorded and submitted, VRAM, the driver's
// events), flushes the log and writes a minidump of every thread (CrashRecord::WriteDump). Once
// per freeze; a resumed game logs how long it stood.

#include <atomic>
#include <span>
#include <string_view>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::GpuCrash {

enum class Kind : u8 {
    None,
    CommandsBegin, // a: the command buffer's tick
    CommandsEnd,   // a: the command buffer's tick
    RenderPass,    // a: view mask | depth << 8; x, y: size; extra: layers | colours << 16
    Work,          // a: the dispatch's shader hash (or a host pass's key); x, y: its size
};

struct NamedQueue {
    vk::Queue queue;
    const char* name;
};

// Before the device: whether it should get the checkpoint extension (build and environment).
[[nodiscard]] bool WantCheckpoints();

// Who owned a faulting GPU address (2026-11-13: an AO pass read 0x1dc00000000, a page no buffer
// of ours was known to hold). With the markers (VR_GPU_ADDRESSES=0 turns it off) the driver tells
// the instance's messenger every object's GPU address range as it is bound and freed
// (VK_EXT_device_address_binding_report); the report names the object at each fault address, the
// one ending just below it, and any freed one that held it, with their debug names.
[[nodiscard]] bool WantAddressReport();
// The instance's messenger for those reports (null when not wanted; the instance keeps it).
[[nodiscard]] vk::UniqueDebugUtilsMessengerEXT AddressMessenger(vk::Instance instance);
// After the device: whether it took the extension.
void SetAddressReport(bool on);
// An object's debug name (vk_platform.h SetObjectName), kept while the address report is on.
inline std::atomic<bool> names_on{false};
void NoteName(vk::ObjectType type, u64 handle, std::string_view name);

// After the device: what it enabled, and its queues (distinct ones; their names for the report).
void Init(vk::PhysicalDevice physical, vk::Device device, bool fault, bool checkpoints,
          bool memory_budget, std::span<const NamedQueue> queues);

inline std::atomic<bool> checkpoints_on{false};

[[nodiscard]] inline bool CheckpointsOn() {
    return checkpoints_on.load(std::memory_order_relaxed);
}

// Who recorded a marker or made a submit: the game's scheduler, or one of the desktop
// presenter's (their ticks count on their own timelines).
enum class Source : u8 { Game, Presenter };

// A marker for the GPU's work, kept in a ring for the report: its id (never 0), the checkpoint's
// value. `tick`: the command buffer it is recorded into (0: unknown).
[[nodiscard]] u64 Mark(Kind kind, u64 tick, u64 a, u32 x, u32 y, u32 extra = 0,
                       Source source = Source::Game);
// A draw in the render pass marked last (the recording thread): its vertex and pixel shaders.
void NoteDraw(u64 vs, u64 ps);
// A submit (the submitting thread) and a sparse bind batch.
void NoteSubmit(u64 tick, u32 waits, u64 sparse_wait, Source source = Source::Game);
void NoteSparseBind(u32 binds, u64 value);
// The game's timeline semaphore (its ticks): the reports read how far the GPU got.
void WatchTimeline(vk::Semaphore timeline);

// `result` of a Vulkan call (`where`: what it was). On the first VK_ERROR_DEVICE_LOST of the
// process the report is written (once; later losses only return true). True when lost.
bool Report(vk::Result result, std::string_view where);
inline bool Report(VkResult result, std::string_view where) {
    return Report(static_cast<vk::Result>(result), where);
}

} // namespace Vulkan::GpuCrash
