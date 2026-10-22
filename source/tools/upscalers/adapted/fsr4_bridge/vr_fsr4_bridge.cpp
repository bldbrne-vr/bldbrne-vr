// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Stereo host adapter using bbport's v07 resource/dispatch conventions.
// Provider sources and notices remain pinned under upstream/fsr-vulkan.
#include "vr_fsr4_bridge.h"
#include "ffx_vk_fsr4_v07.h"
#include "ffx_vk_fsr4_v07_assets.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// FAST bit 45 (EvaluatePair). The backend records one eye's whole frame in one ExecuteGpuJobs
// call: uploads and layout transitions, about fifteen compute jobs each followed by a global
// barrier, then the imports' restores. Each barrier drains the GPU, and at 1456x1576 per eye the
// small model passes cannot fill it. Recording the two eyes' job k back to back under one
// barrier lets the eyes overlap and needs half the drains. The pinned backend source cannot be
// changed (IMPORT_FSR.json), so CMakeLists.txt renames its vkCmd* calls to the vrFsr4TapeCmd*
// functions below. With a real command buffer they call Vulkan with the same arguments. With a
// tape's handle, which only EvaluatePair passes, they append the command, arguments copied by
// value, to that eye's tape; EvaluatePair then issues both tapes into the real command buffer.
class Tape {
public:
  enum class Op : uint8_t {
    Barrier,
    BindPipeline,
    BindSets,
    Dispatch,
    CopyBuffer,
    FillBuffer,
    ClearColor
  };
  struct Command {
    Op op{};
    // Barrier; its structures are memory/buffers/images[first, first + count).
    VkPipelineStageFlags src_stage{}, dst_stage{};
    VkDependencyFlags dependency{};
    uint32_t memory_first{}, memory_count{}, buffer_first{}, buffer_count{};
    uint32_t image_first{}, image_count{};
    // BindPipeline, BindSets.
    VkPipelineBindPoint bind_point{};
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    uint32_t first_set{}, set_first{}, set_count{}, offset_first{}, offset_count{};
    // Dispatch.
    uint32_t x{}, y{}, z{};
    // CopyBuffer, FillBuffer, ClearColor.
    VkBuffer source{}, destination{};
    uint32_t region_first{}, region_count{};
    VkDeviceSize fill_offset{}, fill_size{};
    uint32_t fill_data{};
    VkImage image{};
    VkImageLayout image_layout{};
    VkClearColorValue color{};
    uint32_t range_first{}, range_count{};
  };
  VkCommandBuffer Handle() { return reinterpret_cast<VkCommandBuffer>(this); }
  // The backend that calls the vrFsr4TapeCmd* functions is C: an exception (an allocation
  // failure while storing) must not unwind through it. The tape is marked broken instead.
  template <class F> void Guard(F &&store) noexcept {
    try {
      store();
    } catch (...) {
      broken = true;
    }
  }
  // Capacity is kept, so a steady-state frame does not allocate.
  void Clear() {
    commands.clear();
    memory.clear();
    buffers.clear();
    images.clear();
    sets.clear();
    offsets.clear();
    regions.clear();
    ranges.clear();
    broken = false;
  }
  void Barrier(VkPipelineStageFlags src, VkPipelineStageFlags dst,
               VkDependencyFlags dependency, uint32_t memory_count,
               const VkMemoryBarrier *memory_barriers, uint32_t buffer_count,
               const VkBufferMemoryBarrier *buffer_barriers, uint32_t image_count,
               const VkImageMemoryBarrier *image_barriers) {
    Command c{};
    c.op = Op::Barrier;
    c.src_stage = src;
    c.dst_stage = dst;
    c.dependency = dependency;
    c.memory_first = Append(memory, memory_barriers, memory_count);
    c.memory_count = memory_count;
    c.buffer_first = Append(buffers, buffer_barriers, buffer_count);
    c.buffer_count = buffer_count;
    c.image_first = Append(images, image_barriers, image_count);
    c.image_count = image_count;
    commands.push_back(c);
  }
  void BindPipeline(VkPipelineBindPoint point, VkPipeline bound) {
    Command c{};
    c.op = Op::BindPipeline;
    c.bind_point = point;
    c.pipeline = bound;
    commands.push_back(c);
  }
  void BindSets(VkPipelineBindPoint point, VkPipelineLayout bound_layout, uint32_t first_set,
                uint32_t set_count, const VkDescriptorSet *bound_sets, uint32_t offset_count,
                const uint32_t *dynamic_offsets) {
    Command c{};
    c.op = Op::BindSets;
    c.bind_point = point;
    c.layout = bound_layout;
    c.first_set = first_set;
    c.set_first = Append(sets, bound_sets, set_count);
    c.set_count = set_count;
    c.offset_first = Append(offsets, dynamic_offsets, offset_count);
    c.offset_count = offset_count;
    commands.push_back(c);
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) {
    Command c{};
    c.op = Op::Dispatch;
    c.x = x;
    c.y = y;
    c.z = z;
    commands.push_back(c);
  }
  void CopyBuffer(VkBuffer source, VkBuffer destination, uint32_t count,
                  const VkBufferCopy *copies) {
    Command c{};
    c.op = Op::CopyBuffer;
    c.source = source;
    c.destination = destination;
    c.region_first = Append(regions, copies, count);
    c.region_count = count;
    commands.push_back(c);
  }
  void FillBuffer(VkBuffer destination, VkDeviceSize offset, VkDeviceSize size,
                  uint32_t data) {
    Command c{};
    c.op = Op::FillBuffer;
    c.destination = destination;
    c.fill_offset = offset;
    c.fill_size = size;
    c.fill_data = data;
    commands.push_back(c);
  }
  void ClearColor(VkImage image, VkImageLayout layout, const VkClearColorValue *color,
                  uint32_t count, const VkImageSubresourceRange *cleared) {
    Command c{};
    c.op = Op::ClearColor;
    c.image = image;
    c.image_layout = layout;
    if (color)
      c.color = *color;
    else
      broken = true;
    c.range_first = Append(ranges, cleared, count);
    c.range_count = count;
    commands.push_back(c);
  }
  // Issues one captured command exactly as the backend called it.
  void Replay(VkCommandBuffer command, const Command &c) const {
    switch (c.op) {
    case Op::Barrier:
      vkCmdPipelineBarrier(command, c.src_stage, c.dst_stage, c.dependency, c.memory_count,
                           At(memory, c.memory_first, c.memory_count), c.buffer_count,
                           At(buffers, c.buffer_first, c.buffer_count), c.image_count,
                           At(images, c.image_first, c.image_count));
      break;
    case Op::BindPipeline:
      vkCmdBindPipeline(command, c.bind_point, c.pipeline);
      break;
    case Op::BindSets:
      vkCmdBindDescriptorSets(command, c.bind_point, c.layout, c.first_set, c.set_count,
                              At(sets, c.set_first, c.set_count), c.offset_count,
                              At(offsets, c.offset_first, c.offset_count));
      break;
    case Op::Dispatch:
      vkCmdDispatch(command, c.x, c.y, c.z);
      break;
    case Op::CopyBuffer:
      vkCmdCopyBuffer(command, c.source, c.destination, c.region_count,
                      At(regions, c.region_first, c.region_count));
      break;
    case Op::FillBuffer:
      vkCmdFillBuffer(command, c.destination, c.fill_offset, c.fill_size, c.fill_data);
      break;
    case Op::ClearColor:
      vkCmdClearColorImage(command, c.image, c.image_layout, &c.color, c.range_count,
                           At(ranges, c.range_first, c.range_count));
      break;
    }
  }
  std::vector<Command> commands;
  std::vector<VkMemoryBarrier> memory;
  std::vector<VkBufferMemoryBarrier> buffers;
  std::vector<VkImageMemoryBarrier> images;
  std::vector<VkDescriptorSet> sets;
  std::vector<uint32_t> offsets;
  std::vector<VkBufferCopy> regions;
  std::vector<VkImageSubresourceRange> ranges;
  bool broken{}; // a command could not be copied; the tape must not be issued

private:
  template <class T>
  uint32_t Append(std::vector<T> &to, const T *from, uint32_t count) {
    const auto first = uint32_t(to.size());
    if (count && !from)
      broken = true;
    for (uint32_t i = 0; from && i < count; ++i) {
      // The backend passes plain structures; an extension chain would not survive the copy.
      if constexpr (requires(const T &t) { t.pNext; })
        broken |= from[i].pNext != nullptr;
      to.push_back(from[i]);
    }
    return first;
  }
  template <class T>
  static const T *At(const std::vector<T> &from, uint32_t first, uint32_t count) {
    return count ? from.data() + first : nullptr;
  }
};
// A run of non-barrier commands and the barriers after it: work [work_begin, work_end),
// barriers [work_end, barrier_end).
struct Segment {
  uint32_t work_begin{}, work_end{}, barrier_end{};
};
struct PairScratch {
  std::vector<Segment> left, right;
  std::vector<VkBufferMemoryBarrier> buffers;
  std::vector<VkImageMemoryBarrier> images;
};
// The tape being captured on this thread (only inside EvaluatePair), else null.
thread_local Tape *taping;
Tape *Taped(VkCommandBuffer command) {
  return taping && command == taping->Handle() ? taping : nullptr;
}
} // namespace

// The backend's vkCmd* calls, renamed in CMakeLists.txt. Outside EvaluatePair `taping` is null
// and each call reaches Vulkan unchanged.
extern "C" {
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdPipelineBarrier(
    VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags dependency, uint32_t memory_count, const VkMemoryBarrier *memory,
    uint32_t buffer_count, const VkBufferMemoryBarrier *buffers, uint32_t image_count,
    const VkImageMemoryBarrier *images) {
  if (auto *tape = Taped(command))
    tape->Guard([&] {
      tape->Barrier(src, dst, dependency, memory_count, memory, buffer_count, buffers,
                    image_count, images);
    });
  else
    vkCmdPipelineBarrier(command, src, dst, dependency, memory_count, memory, buffer_count,
                         buffers, image_count, images);
}
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdBindPipeline(VkCommandBuffer command,
                                                    VkPipelineBindPoint point,
                                                    VkPipeline pipeline) {
  if (auto *tape = Taped(command))
    tape->Guard([&] { tape->BindPipeline(point, pipeline); });
  else
    vkCmdBindPipeline(command, point, pipeline);
}
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdBindDescriptorSets(
    VkCommandBuffer command, VkPipelineBindPoint point, VkPipelineLayout layout,
    uint32_t first_set, uint32_t set_count, const VkDescriptorSet *sets,
    uint32_t offset_count, const uint32_t *offsets) {
  if (auto *tape = Taped(command))
    tape->Guard([&] {
      tape->BindSets(point, layout, first_set, set_count, sets, offset_count, offsets);
    });
  else
    vkCmdBindDescriptorSets(command, point, layout, first_set, set_count, sets, offset_count,
                            offsets);
}
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdDispatch(VkCommandBuffer command, uint32_t x,
                                                uint32_t y, uint32_t z) {
  if (auto *tape = Taped(command))
    tape->Guard([&] { tape->Dispatch(x, y, z); });
  else
    vkCmdDispatch(command, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdCopyBuffer(VkCommandBuffer command, VkBuffer source,
                                                  VkBuffer destination, uint32_t count,
                                                  const VkBufferCopy *regions) {
  if (auto *tape = Taped(command))
    tape->Guard([&] { tape->CopyBuffer(source, destination, count, regions); });
  else
    vkCmdCopyBuffer(command, source, destination, count, regions);
}
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdFillBuffer(VkCommandBuffer command,
                                                  VkBuffer destination, VkDeviceSize offset,
                                                  VkDeviceSize size, uint32_t data) {
  if (auto *tape = Taped(command))
    tape->Guard([&] { tape->FillBuffer(destination, offset, size, data); });
  else
    vkCmdFillBuffer(command, destination, offset, size, data);
}
VKAPI_ATTR void VKAPI_CALL vrFsr4TapeCmdClearColorImage(
    VkCommandBuffer command, VkImage image, VkImageLayout layout,
    const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *ranges) {
  if (auto *tape = Taped(command))
    tape->Guard([&] { tape->ClearColor(image, layout, color, count, ranges); });
  else
    vkCmdClearColorImage(command, image, layout, color, count, ranges);
}
} // extern "C"

namespace {
thread_local std::string creation_error;
struct Eye {
  FfxInterface backend{};
  ffxContext context{};
  std::vector<std::max_align_t> scratch;
  std::deque<std::pair<uint64_t, uint64_t>>
      flight; // dispatch id, caller ticket
  VrFsr4Desc description{};
  uint64_t next_id{1}, completed{}, last_ticket{};
  bool backend_ok{}, context_ok{}, reset{true}, failed{};
  std::string problem;
  Tape tape; // EvaluatePair's capture of this eye's commands, reused every frame
  ~Eye() {
    if (context_ok)
      ffxFsr4V07DestroyContext(&context, nullptr);
    if (backend_ok)
      ffxFsr4VkDestroyContext(
          reinterpret_cast<FfxFsr4VkContext *>(scratch.data()));
  }
};
bool Read(const std::filesystem::path &path, std::vector<uint32_t> &out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto bytes = file ? file.tellg() : std::streampos(-1);
  if (bytes <= 0 || bytes > 128 * 1024 * 1024 || size_t(bytes) % 4)
    return false;
  out.resize(size_t(bytes) / 4);
  file.seekg(0);
  return bool(
      file.read(reinterpret_cast<char *>(out.data()), std::streamsize(bytes)));
}
void *Create(VkDevice device, VkPhysicalDevice physical,
             const wchar_t *directory, const VrFsr4Desc *desc) {
  creation_error.clear();
  if (!device || !physical || !directory || !desc || desc->profile > 3 ||
      (desc->flags & ~(VR_FSR4_FLAG_OPTIMIZED_POST | VR_FSR4_FLAG_FENCED_SPD))) {
    creation_error = "invalid FSR create arguments";
    return nullptr;
  }
  const bool optimized_post = (desc->flags & VR_FSR4_FLAG_OPTIMIZED_POST) != 0;
  const bool fenced_spd = (desc->flags & VR_FSR4_FLAG_FENCED_SPD) != 0;
  const FfxFsr4ModelPreset presets[] = {FFX_FSR4_MODEL_PRESET_PERFORMANCE,
                                        FFX_FSR4_MODEL_PRESET_BALANCED,
                                        FFX_FSR4_MODEL_PRESET_QUALITY,
                                        FFX_FSR4_MODEL_PRESET_NATIVE_AA};
  FfxFsr4V07AssetSet set{};
  if (!ffxFsr4V07BuildAssetSet(presets[desc->profile], desc->width,
                               desc->height, &set)) {
    creation_error = "unsupported FSR output dimensions";
    return nullptr;
  }
  std::array<std::vector<uint32_t>, FFX_FSR4_VK_PASS_COUNT> code;
  std::vector<uint32_t> initializer, weights;
  const std::filesystem::path base(directory);
  auto load = [&](const char *file, std::vector<uint32_t> &data) {
    // bbport's pass 11 writes beyond a row at the 1080 tier. Require the guarded
    // variant; never silently fall back to the original racing shader.
    const bool guarded = std::string_view(file).ends_with("_pass11.spv");
    if (Read((guarded ? base / "opt" : base) / file, data))
      return true;
    creation_error = std::string("missing or invalid FSR asset: ") + file;
    return false;
  };
  if (!load(set.pre, code[0]))
    return nullptr;
  for (uint32_t i = 0; i < FFX_FSR4_MODEL_PASS_COUNT; ++i)
    if (!load(set.model[i], code[i + 1]))
      return nullptr;
  // The host's FAST bit 37 requests bbport's shared-memory post pass (same bindings and
  // results, contiguous stores), built into opt/ by prepare_fsr4_post_lds.py. A requested
  // but missing file fails creation instead of silently running the original.
  if (optimized_post && !Read(base / "opt" / set.post, code[13])) {
    creation_error = std::string("missing optimized FSR post pass: opt/") + set.post;
    return nullptr;
  }
  // FAST bit 37 also requests prepare_fsr4_post_lds.py's fenced SPD auto-exposure: the same
  // arithmetic, with each workgroup's tile store ordered before its counter increment (the
  // original races on NVIDIA, so two contexts fed identical frames can drift apart). Like
  // the post pass, a requested but missing file fails creation.
  if (fenced_spd && !Read(base / "opt" / set.spdAutoExposure, code[15])) {
    creation_error =
        std::string("missing fenced FSR SPD pass: opt/") + set.spdAutoExposure;
    return nullptr;
  }
  if ((!optimized_post && !load(set.post, code[13])) || !load(set.rcas, code[14]) ||
      (!fenced_spd && !load(set.spdAutoExposure, code[15])) ||
      !load(set.initializer, initializer) || !load(set.prePassWeights, weights))
    return nullptr;
  if (initializer.size() * 4 != FFX_FSR4_V07_INITIALIZER_BYTES ||
      weights.size() * 4 != FFX_FSR4_V07_PRE_PASS_WEIGHTS_BYTES) {
    creation_error = "FSR model weight size mismatch";
    return nullptr;
  }
  auto eye = std::make_unique<Eye>();
  eye->description = *desc;
  const auto bytes = ffxFsr4VkGetScratchMemorySize();
  eye->scratch.resize((bytes + sizeof(std::max_align_t) - 1) /
                      sizeof(std::max_align_t));
  FfxFsr4VkCreateInfo ci{};
  ci.device = device;
  ci.physicalDevice = physical;
  ci.scratchBuffer = eye->scratch.data();
  ci.scratchBufferSize = eye->scratch.size() * sizeof(std::max_align_t);
  ci.modelInitializer = initializer.data();
  ci.modelInitializerSize = initializer.size() * 4;
  ci.prePassWeights = weights.data();
  ci.prePassWeightsSize = weights.size() * 4;
  std::array<std::string, FFX_FSR4_VK_PASS_COUNT> entries;
  entries.fill("main");
  for (uint32_t i = 1; i <= FFX_FSR4_MODEL_PASS_COUNT; ++i)
    entries[i] = "fsr4_model_v07_i8_pass" + std::to_string(i);
  for (uint32_t i = 0; i < code.size(); ++i) {
    ci.shaders[i] = {code[i].data(), code[i].size() * 4, entries[i].c_str()};
    if (ffxFsr4VkValidateShaderLayout(&ci.shaders[i], i) != VK_SUCCESS) {
      creation_error =
          "FSR shader layout mismatch in pass " + std::to_string(i);
      return nullptr;
    }
  }
  if (ffxFsr4VkCreateContext(&ci, &eye->backend) != VK_SUCCESS) {
    creation_error = "FSR Vulkan backend creation failed";
    return nullptr;
  }
  eye->backend_ok = true;
  ffxCreateContextDescUpscale d{};
  d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  d.flags =
      FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
  if (desc->depth_inverted)
    d.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  d.maxRenderSize = {(desc->width + 7) & ~7u, (desc->height + 7) & ~7u};
  d.maxUpscaleSize = d.maxRenderSize;
  ffxFsr4V07SetBackendInterface(&eye->backend);
  const auto result =
      ffxFsr4V07CreateContext(&eye->context, &d.header, nullptr);
  ffxFsr4V07SetBackendInterface(nullptr);
  if (result != FFX_API_RETURN_OK) {
    creation_error = "FSR provider context creation failed";
    return nullptr;
  }
  eye->context_ok = true;
  return eye.release();
}
void Completed(void *handle, uint64_t ticket) {
  if (!handle)
    return;
  auto &e = *static_cast<Eye *>(handle);
  if (ticket > e.completed)
    e.completed = ticket;
  while (!e.flight.empty() && e.flight.front().second <= e.completed) {
    ffxFsr4VkRetireFrame(&e.backend, e.flight.front().first);
    e.flight.pop_front();
  }
}
// Evaluate's input checks.
bool Invalid(const Eye &e, const VrFsr4Frame &f) {
  auto valid = [](const VrFsr4Image &i) {
    return i.image && i.view && i.width && i.height;
  };
  return !f.command || f.ticket <= e.completed || f.ticket < e.last_ticket ||
         !valid(f.color) || !valid(f.depth) || !valid(f.motion) ||
         !valid(f.output) || f.output.width != e.description.width ||
         f.output.height != e.description.height ||
         f.color.width > f.output.width || f.color.height > f.output.height ||
         f.depth.width != f.color.width || f.depth.height != f.color.height ||
         f.motion.width != f.color.width || f.motion.height != f.color.height ||
         !std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y) ||
         !std::isfinite(f.frame_ms) || f.frame_ms <= 0 ||
         !std::isfinite(f.near_plane) || f.near_plane <= 0 ||
         !std::isfinite(f.far_plane) || f.far_plane <= f.near_plane ||
         !std::isfinite(f.vertical_fov) || f.vertical_fov <= 0 ||
         f.vertical_fov >= 3.141593f;
}
// Evaluate's frame setup: a dispatch slot and the four imports' states. Records nothing.
// 1 ready, 0 failed, 2 no free slot.
int32_t Open(Eye &e, const VrFsr4Frame &f) {
  const uint64_t id = e.next_id;
  const auto begin = ffxFsr4VkBeginFrame(&e.backend, id);
  if (begin == VK_NOT_READY)
    return 2;
  if (begin != VK_SUCCESS) {
    e.failed = true;
    e.problem = "FSR begin-frame failed";
    return 0;
  }
  ++e.next_id;
  e.last_ticket = f.ticket;
  e.flight.emplace_back(id, f.ticket);
  const VrFsr4Image *images[] = {&f.color, &f.depth, &f.motion, &f.output};
  for (unsigned i = 0; i < 4; ++i) {
    const auto &image = *images[i];
    const VkAccessFlags access =
        VK_ACCESS_SHADER_READ_BIT | (i == 3 ? VK_ACCESS_SHADER_WRITE_BIT : 0);
    const FfxFsr4VkExternalImageState state{
        sizeof(FfxFsr4VkExternalImageState),
        image.image,
        image.view,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        access,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        access};
    if (ffxFsr4VkSetExternalImageState(&e.backend, &state) != VK_SUCCESS) {
      e.failed = true;
      e.problem = "FSR image registration failed";
      return 0;
    }
  }
  return 1;
}
// Evaluate's provider dispatch, recorded into `command`: the frame's command buffer, or a
// tape's handle. 1 recorded, 0 failed (possibly after recording part of the frame).
int32_t Record(Eye &e, const VrFsr4Frame &f, VkCommandBuffer command) {
  auto resource = [](const VrFsr4Image &i, uint32_t format, uint32_t state) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void *>(i.view);
    r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = format;
    r.description.width = i.width;
    r.description.height = i.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.state = state;
    return r;
  };
  ffxDispatchDescUpscale d{};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = reinterpret_cast<void *>(command);
  d.color = resource(f.color, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                     FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.depth = resource(f.depth, FFX_SURFACE_FORMAT_R32_FLOAT,
                     FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.motionVectors = resource(f.motion, FFX_SURFACE_FORMAT_R16G16_FLOAT,
                             FFX_API_RESOURCE_STATE_COMPUTE_READ);
  d.output = resource(f.output, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  d.jitterOffset = {f.jitter_x, f.jitter_y};
  d.motionVectorScale = {1, 1};
  d.renderSize = {f.color.width, f.color.height};
  d.upscaleSize = {f.output.width, f.output.height};
  d.enableAutoExposure = true;
  d.frameTimeDelta = f.frame_ms;
  d.preExposure = 1;
  d.reset = e.reset || f.reset;
  d.cameraNear = f.near_plane;
  d.cameraFar = f.far_plane;
  d.cameraFovAngleVertical = f.vertical_fov;
  d.viewSpaceToMetersFactor = 1;
  if (ffxFsr4V07Dispatch(&e.context, &d.header) != FFX_API_RETURN_OK) {
    e.failed = true;
    e.problem = "FSR dispatch failed";
    return 0;
  }
  e.reset = false;
  e.problem.clear();
  return 1;
}
int32_t Evaluate(void *handle, const VrFsr4Frame *frame) {
  if (!handle || !frame)
    return 0;
  auto &e = *static_cast<Eye *>(handle);
  const auto &f = *frame;
  if (e.failed)
    return 0;
  if (Invalid(e, f)) {
    e.problem = "invalid FSR frame inputs";
    return 0;
  }
  const auto opened = Open(e, f);
  if (opened != 1)
    return opened;
  return Record(e, f, f.command);
}

// ---- FAST bit 45: both eyes in lockstep (EvaluatePair) ----
//
// Why the output is bit-identical to Evaluate(left), Evaluate(right):
//  - Each eye goes through the same steps (checks, slot, imports, provider dispatch), so the
//    provider stages the same constants, allocates and writes the same descriptor sets and
//    schedules the same jobs. Only the command buffer differs: the eye's tape. The replay
//    issues every captured command with its captured arguments, so the same dispatches read
//    and write the same resources.
//  - Interleaving changes only the order between the two eyes and how barriers are grouped.
//    Each eye's own commands keep their order, and every barrier group that separated two of
//    them still does: the merged barrier is the union of both eyes' stages and accesses, so it
//    is at least as strong as each original barrier (and as two of them in a row).
//  - The eyes share no resource that either writes, so running their dispatches concurrently
//    cannot change a value. Each Eye owns its backend context (activation scratch, history,
//    recurrent pair, reprojected, RCAS temp, exposure, SPD counter and SPD mip-5 images, model
//    initializer copy), its host-visible constant ring, its three descriptor pools, pipelines
//    and sampler (FfxFsr4VkContext lives in Eye::scratch), and its provider context (calloc in
//    ffxFsr4V07CreateContext). The provider's only global, g_fsr4_backend_interface, is read
//    during creation alone. The host's color, depth, motion and output images are distinct per
//    eye (VrTemporalPair::Validate), and the inputs are only sampled. The verification twin
//    (FAST_UPSCALER_VERIFY, FAST_FSR4_INTERLEAVE_VERIFY) is a third context, evaluated alone.
//  - Not an ordering effect: SPD auto-exposure has a last-workgroup race inside one context
//    (research_fsr4_bitexact.md), so on NVIDIA even the per-eye order is not reproducible on
//    every frame until that pass is fenced.
constexpr size_t kDispatchSlots = 3;  // ffxFsr4VkBeginFrame: at most three unretired dispatches
std::atomic<bool> pair_unsupported{}; // a captured command could not be issued (sticky)

// The preconditions for interleaving, checked before anything is recorded. None is needed for
// correctness (any two tapes interleave safely), but they keep the two eyes' command streams
// alike and every error path identical to the per-eye order.
uint32_t PairBlocker(const Eye &l, const Eye &r, const VrFsr4Frame &lf,
                     const VrFsr4Frame &rf) {
  if (pair_unsupported.load(std::memory_order_relaxed))
    return VR_FSR4_PAIR_UNSUPPORTED;
  const auto &a = l.description, &b = r.description;
  if (a.width != b.width || a.height != b.height || a.profile != b.profile ||
      a.depth_inverted != b.depth_inverted || a.flags != b.flags)
    return VR_FSR4_PAIR_DESC;
  if (lf.command != rf.command || lf.ticket != rf.ticket ||
      lf.color.width != rf.color.width || lf.color.height != rf.color.height ||
      (l.reset || lf.reset != 0) != (r.reset || rf.reset != 0) || Invalid(l, lf) ||
      Invalid(r, rf))
    return VR_FSR4_PAIR_FRAME;
  if (l.failed || r.failed || l.flight.size() >= kDispatchSlots ||
      r.flight.size() >= kDispatchSlots)
    return VR_FSR4_PAIR_STATE;
  return VR_FSR4_PAIR_INTERLEAVED;
}
// Captures one eye's provider dispatch on its tape.
int32_t Capture(Eye &e, const VrFsr4Frame &f) {
  struct Scope {
    explicit Scope(Tape &tape) { taping = &tape; }
    ~Scope() { taping = nullptr; }
  } scope(e.tape);
  return Record(e, f, e.tape.Handle());
}
void Split(const Tape &tape, std::vector<Segment> &out) {
  out.clear();
  const auto &c = tape.commands;
  const auto n = uint32_t(c.size());
  for (uint32_t i = 0; i < n;) {
    Segment s{i, i, i};
    while (i < n && c[i].op != Tape::Op::Barrier)
      ++i;
    s.work_end = i;
    while (i < n && c[i].op == Tape::Op::Barrier)
      ++i;
    s.barrier_end = i;
    out.push_back(s);
  }
}
// The same job list: as many segments, each with the same sequence of command kinds, and every
// dispatch bound (pipeline and descriptor sets) inside its own segment, so the other eye's
// binds issued in between cannot change what it runs.
bool SameShape(const Tape &l, const std::vector<Segment> &ls, const Tape &r,
               const std::vector<Segment> &rs) {
  if (ls.size() != rs.size())
    return false;
  for (size_t i = 0; i < ls.size(); ++i) {
    const auto &a = ls[i], &b = rs[i];
    if (a.work_end - a.work_begin != b.work_end - b.work_begin)
      return false;
    bool pipeline = false, sets = false;
    for (uint32_t k = 0; k < a.work_end - a.work_begin; ++k) {
      const auto op = l.commands[a.work_begin + k].op;
      if (op != r.commands[b.work_begin + k].op)
        return false;
      pipeline |= op == Tape::Op::BindPipeline;
      sets |= op == Tape::Op::BindSets;
      if (op == Tape::Op::Dispatch && !(pipeline && sets))
        return false;
    }
  }
  return true;
}
uint32_t CountBarriers(const Tape &tape) {
  uint32_t n = 0;
  for (const auto &c : tape.commands)
    n += c.op == Tape::Op::Barrier;
  return n;
}
void ReplayRange(VkCommandBuffer command, const Tape &tape, uint32_t begin, uint32_t end,
                 VrFsr4PairResult &out) {
  for (uint32_t i = begin; i < end; ++i) {
    const auto &c = tape.commands[i];
    tape.Replay(command, c);
    out.barriers += c.op == Tape::Op::Barrier;
    out.dispatches += c.op == Tape::Op::Dispatch;
  }
}
// Both eyes' barrier groups as one vkCmdPipelineBarrier: the union of their stage masks,
// dependency flags and global memory accesses, with every buffer and image barrier kept as
// captured (layouts, accesses, ranges). Records nothing and returns false when a buffer or an
// image appears twice, since one call cannot order two transitions of the same subresource.
bool MergeBarriers(VkCommandBuffer command, const Tape &l, const Segment &a, const Tape &r,
                   const Segment &b, PairScratch &s, VrFsr4PairResult &out) {
  if (a.work_end == a.barrier_end && b.work_end == b.barrier_end)
    return true;
  VkPipelineStageFlags src = 0, dst = 0;
  VkDependencyFlags dependency = 0;
  VkMemoryBarrier memory{};
  memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  uint32_t memory_count = 0;
  s.buffers.clear();
  s.images.clear();
  const auto gather = [&](const Tape &tape, const Segment &segment) {
    for (uint32_t i = segment.work_end; i < segment.barrier_end; ++i) {
      const auto &c = tape.commands[i];
      src |= c.src_stage;
      dst |= c.dst_stage;
      dependency |= c.dependency;
      for (uint32_t k = 0; k < c.memory_count; ++k) {
        memory.srcAccessMask |= tape.memory[c.memory_first + k].srcAccessMask;
        memory.dstAccessMask |= tape.memory[c.memory_first + k].dstAccessMask;
        memory_count = 1;
      }
      for (uint32_t k = 0; k < c.buffer_count; ++k)
        s.buffers.push_back(tape.buffers[c.buffer_first + k]);
      for (uint32_t k = 0; k < c.image_count; ++k)
        s.images.push_back(tape.images[c.image_first + k]);
    }
  };
  gather(l, a);
  gather(r, b);
  for (size_t i = 0; i < s.buffers.size(); ++i)
    for (size_t j = i + 1; j < s.buffers.size(); ++j)
      if (s.buffers[i].buffer == s.buffers[j].buffer)
        return false;
  for (size_t i = 0; i < s.images.size(); ++i)
    for (size_t j = i + 1; j < s.images.size(); ++j)
      if (s.images[i].image == s.images[j].image)
        return false;
  vkCmdPipelineBarrier(command, src, dst, dependency, memory_count,
                       memory_count ? &memory : nullptr, uint32_t(s.buffers.size()),
                       s.buffers.empty() ? nullptr : s.buffers.data(),
                       uint32_t(s.images.size()), s.images.empty() ? nullptr : s.images.data());
  ++out.barriers;
  return true;
}
// Segment i of both eyes: the left eye's work, the right eye's work, then one barrier for both.
// A steady-state frame is 16 segments per eye (the imports' transitions, then 15 jobs, the
// restores folded into the last), so 76 barrier calls become 16 and the 30 GPU drains between
// dependent jobs become 15, each now behind two dispatches.
void ReplayInterleaved(VkCommandBuffer command, const Tape &l, const Tape &r, PairScratch &s,
                       VrFsr4PairResult &out) {
  for (size_t i = 0; i < s.left.size(); ++i) {
    const auto &a = s.left[i], &b = s.right[i];
    ReplayRange(command, l, a.work_begin, a.work_end, out);
    ReplayRange(command, r, b.work_begin, b.work_end, out);
    if (!MergeBarriers(command, l, a, r, b, s, out)) {
      ReplayRange(command, l, a.work_end, a.barrier_end, out);
      ReplayRange(command, r, b.work_end, b.barrier_end, out);
    }
  }
}
int32_t EvaluatePair(void *left, void *right, const VrFsr4Frame *left_frame,
                     const VrFsr4Frame *right_frame, VrFsr4PairResult *result) {
  if (!result)
    return 0;
  auto &out = *result;
  out = VrFsr4PairResult{};
  out.eye[0] = out.eye[1] = -1;
  if (!left || !right || left == right || !left_frame || !right_frame) {
    out.eye[0] = 0;
    return 0;
  }
  auto &l = *static_cast<Eye *>(left);
  auto &r = *static_cast<Eye *>(right);
  const auto &lf = *left_frame;
  const auto &rf = *right_frame;
  // Any unmet precondition: exactly Evaluate(left), then Evaluate(right) after a recorded left.
  out.fallback = PairBlocker(l, r, lf, rf);
  if (out.fallback != VR_FSR4_PAIR_INTERLEAVED) {
    out.eye[0] = Evaluate(left, left_frame);
    if (out.eye[0] == 1)
      out.eye[1] = Evaluate(right, right_frame);
    return out.eye[1] >= 0 ? out.eye[1] : out.eye[0];
  }
  // The per-eye steps in the per-eye order, each provider dispatch captured on the eye's tape:
  // the right eye is attempted only after the left recorded.
  l.tape.Clear();
  r.tape.Clear();
  out.eye[0] = Open(l, lf);
  if (out.eye[0] == 1)
    out.eye[0] = Capture(l, lf);
  if (out.eye[0] == 1) {
    out.eye[1] = Open(r, rf);
    if (out.eye[1] == 1)
      out.eye[1] = Capture(r, rf);
  }
  // A tape that misses a command cannot be issued: that eye's frame is lost and it fails like
  // a provider error (the host recreates both contexts); later pairs stay per-eye.
  const auto lost = [&](Eye &e, int32_t &status) {
    if (!e.tape.broken)
      return false;
    e.failed = true;
    e.problem = "FSR pair capture could not copy a command";
    if (status >= 0)
      status = 0;
    pair_unsupported.store(true, std::memory_order_relaxed);
    return true;
  };
  const bool left_lost = lost(l, out.eye[0]);
  const bool right_lost = lost(r, out.eye[1]);
  out.sequential_barriers = CountBarriers(l.tape) + CountBarriers(r.tape);
  thread_local PairScratch scratch;
  bool interleave = out.eye[0] == 1 && out.eye[1] == 1;
  if (interleave) {
    Split(l.tape, scratch.left);
    Split(r.tape, scratch.right);
    interleave = SameShape(l.tape, scratch.left, r.tape, scratch.right);
    if (!interleave)
      out.fallback = VR_FSR4_PAIR_SHAPE;
  } else {
    out.fallback = left_lost || right_lost ? VR_FSR4_PAIR_UNSUPPORTED : VR_FSR4_PAIR_RECORD;
  }
  if (interleave) {
    ReplayInterleaved(lf.command, l.tape, r.tape, scratch, out);
    out.interleaved = 1;
  } else {
    // The per-eye order, command for command (including a failed eye's partial frame, as the
    // per-eye path would have left it in the command buffer).
    if (!left_lost)
      ReplayRange(lf.command, l.tape, 0, uint32_t(l.tape.commands.size()), out);
    if (!right_lost)
      ReplayRange(lf.command, r.tape, 0, uint32_t(r.tape.commands.size()), out);
  }
  return out.eye[1] >= 0 ? out.eye[1] : out.eye[0];
}
int32_t Release(void *handle) {
  if (!handle)
    return 1;
  auto *eye = static_cast<Eye *>(handle);
  if (!eye->flight.empty())
    return 0;
  delete eye;
  return 1;
}
const char *Problem(void *handle) {
  return handle ? static_cast<Eye *>(handle)->problem.c_str()
                : creation_error.c_str();
}
const VrFsr4Api api{VR_FSR4_ABI, Create, Evaluate, Completed, Release, Problem};
const VrFsr4PairApi pair_api{VR_FSR4_PAIR_ABI, EvaluatePair};
} // namespace
extern "C" __declspec(dllexport) const VrFsr4Api *VrFsr4GetStereoApi() {
  return &api;
}
// The flags Create accepts (vr_fsr4_bridge.h); hosts that predate it never request them.
extern "C" __declspec(dllexport) uint32_t VrFsr4GetStereoFlags() {
  return VR_FSR4_FLAG_OPTIMIZED_POST | VR_FSR4_FLAG_FENCED_SPD;
}
// FAST bit 45 (VR_FSR4_PAIR_EXPORT): optional, so VrFsr4Api keeps ABI 2 and older hosts still
// load this bridge.
extern "C" __declspec(dllexport) const VrFsr4PairApi *VrFsr4GetStereoPairApi() {
  return &pair_api;
}
