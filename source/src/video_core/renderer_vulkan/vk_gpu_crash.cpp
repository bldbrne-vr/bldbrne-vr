// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_gpu_crash.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>
#include <fmt/format.h>

#include "common/crash_record.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/render_diagnostic.h"
#include "common/scm_rev.h"
#include "common/thread.h"

#ifdef _WIN32
#include <windows.h>
#include <winevt.h>
#pragma comment(lib, "wevtapi.lib")
#endif

namespace Vulkan::GpuCrash {
namespace {

s64 SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const s64 g_start_ns = SteadyNs();

// The markers' ring: written by the recording thread (passes, dispatches, draws) and the replay
// thread (command buffer begin and end), read only by the report. A slot is valid while its id is
// the one asked for (32,768 markers: several frames of loading's work).
struct Entry {
    std::atomic<u64> id{0};
    Kind kind{};
    Source source{};
    u32 x{}, y{}, extra{};
    u64 tick{};
    u64 a{};
    s64 ns{};
    u32 draws{};
    u64 first_vs{}, first_ps{}, last_vs{}, last_ps{};
};
constexpr u64 RingSize = u64{1} << 15;
std::unique_ptr<Entry[]> g_ring;
std::atomic<u64> g_next_id{0};
std::atomic<u64> g_last_pass{0};

struct SubmitNote {
    u64 tick;
    u32 waits;
    u64 sparse_wait;
    s64 ns;
    Source source;
};
constexpr u64 SubmitRing = 16;
std::array<SubmitNote, SubmitRing> g_submits{};
std::atomic<u64> g_submit_count{0};

struct BindNote {
    u32 binds;
    u64 value;
    s64 ns;
};
constexpr u64 BindRing = 8;
std::array<BindNote, BindRing> g_binds{};
std::atomic<u64> g_bind_count{0};

vk::PhysicalDevice g_physical;
vk::Device g_device;
bool g_fault = false;
bool g_budget = false;
std::vector<NamedQueue> g_queues;
std::atomic<bool> g_reported{false};
std::atomic<bool> g_written{false};
vk::Semaphore g_timeline{};
std::atomic<u64> g_game_submits{0};
std::atomic<u64> g_game_last_tick{0};

const char* SourceName(Source s) {
    return s == Source::Game ? "game" : "presenter";
}

std::string Seconds(s64 ns) {
    return fmt::format("{:.3f} s", static_cast<double>(ns - g_start_ns) * 1e-9);
}

std::string Describe(u64 id) {
    if (id == 0) {
        return "none";
    }
    if (!g_ring) {
        return fmt::format("#{} (no markers)", id);
    }
    const Entry& e = g_ring[id & (RingSize - 1)];
    if (e.id.load(std::memory_order_acquire) != id) {
        return fmt::format("#{} (overwritten: older than the last {} markers)", id, RingSize);
    }
    std::string text;
    switch (e.kind) {
    case Kind::CommandsBegin:
        text = fmt::format("#{} {} tick {}: command buffer begins", id, SourceName(e.source), e.a);
        break;
    case Kind::CommandsEnd:
        text = fmt::format("#{} {} tick {}: command buffer ends", id, SourceName(e.source), e.a);
        break;
    case Kind::RenderPass: {
        text = fmt::format("#{} {} tick {}: render pass {}x{}, {} layer(s), {} colour, {}, view "
                           "mask {}",
                           id, SourceName(e.source), e.tick, e.x, e.y, e.extra & 0xffff, e.extra >> 16,
                           (e.a >> 8) & 1 ? "depth" : "no depth", e.a & 0xff);
        if (e.draws != 0) {
            text += fmt::format(", {} draw(s): first vs {:#018x} ps {:#018x}, last vs {:#018x} "
                                "ps {:#018x}",
                                e.draws, e.first_vs, e.first_ps, e.last_vs, e.last_ps);
        } else {
            text += ", no draws noted";
        }
        break;
    }
    case Kind::Work:
        text = fmt::format("#{} {} tick {}: dispatch / pass {:#018x} ({} x {})", id,
                           SourceName(e.source), e.tick, e.a, e.x, e.y);
        break;
    default:
        text = fmt::format("#{}: ?", id);
        break;
    }
    return text + fmt::format(" [recorded at {}]", Seconds(e.ns));
}

const char* AddressType(vk::DeviceFaultAddressTypeEXT type) {
    switch (type) {
    case vk::DeviceFaultAddressTypeEXT::eNone:
        return "none";
    case vk::DeviceFaultAddressTypeEXT::eReadInvalid:
        return "READ of an invalid address";
    case vk::DeviceFaultAddressTypeEXT::eWriteInvalid:
        return "WRITE to an invalid address";
    case vk::DeviceFaultAddressTypeEXT::eExecuteInvalid:
        return "EXECUTE of an invalid address";
    case vk::DeviceFaultAddressTypeEXT::eInstructionPointerUnknown:
        return "instruction pointer (unknown)";
    case vk::DeviceFaultAddressTypeEXT::eInstructionPointerInvalid:
        return "instruction pointer (invalid)";
    case vk::DeviceFaultAddressTypeEXT::eInstructionPointerFault:
        return "instruction pointer (at the fault)";
    default:
        return "?";
    }
}

void AddGpu(std::vector<std::string>& out) {
    const auto chain = g_physical.getProperties2<vk::PhysicalDeviceProperties2,
                                                 vk::PhysicalDeviceVulkan12Properties>();
    const auto& p = chain.get<vk::PhysicalDeviceProperties2>().properties;
    const auto& v12 = chain.get<vk::PhysicalDeviceVulkan12Properties>();
    out.push_back(fmt::format("GPU: {} (vendor {:#x}, device {:#x}), driver {} {} (raw {:#x}), "
                              "Vulkan {}.{}.{}",
                              p.deviceName.data(), p.vendorID, p.deviceID, v12.driverName.data(),
                              v12.driverInfo.data(), p.driverVersion,
                              VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                              VK_VERSION_PATCH(p.apiVersion)));
}

void AddMemory(std::vector<std::string>& out) {
    if (g_budget) {
        const auto chain = g_physical.getMemoryProperties2<vk::PhysicalDeviceMemoryProperties2,
                                                           vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
        const auto& props = chain.get<vk::PhysicalDeviceMemoryProperties2>().memoryProperties;
        const auto& budget = chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
        for (u32 i = 0; i < props.memoryHeapCount; ++i) {
            out.push_back(fmt::format(
                "Memory heap {}{}: {} MiB, budget {} MiB, used {} MiB", i,
                props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal ? " (VRAM)"
                                                                                  : "",
                props.memoryHeaps[i].size >> 20, budget.heapBudget[i] >> 20,
                budget.heapUsage[i] >> 20));
        }
        return;
    }
    const auto props = g_physical.getMemoryProperties();
    for (u32 i = 0; i < props.memoryHeapCount; ++i) {
        out.push_back(fmt::format("Memory heap {}: {} MiB (no budget extension)", i,
                                  props.memoryHeaps[i].size >> 20));
    }
}

// The address report (WantAddressReport): every object's GPU range as the driver bound it, the
// live ones by base, the last ones freed with the name they had.
struct AddressRange {
    u64 base{}, size{};
    u64 handle{};
    vk::ObjectType type{};
    bool internal{};
    s64 bound_ns{}, freed_ns{};
    std::string name; // set when freed (a live one's is looked up: names come after the bind)
};
constexpr size_t FreedKept = 32768;
std::mutex g_addr_mutex;
std::multimap<u64, AddressRange> g_live;
std::deque<AddressRange> g_freed;
std::unordered_map<u64, std::string> g_names;
u64 g_addr_binds = 0, g_addr_unbinds = 0, g_addr_widest = 0;
std::atomic<bool> g_addr_on{false};

bool KeepsName(vk::ObjectType type) {
    return type == vk::ObjectType::eBuffer || type == vk::ObjectType::eImage ||
           type == vk::ObjectType::eDeviceMemory || type == vk::ObjectType::eAccelerationStructureKHR;
}

void NoteBinding(u64 base, u64 size, bool bind, bool internal, vk::ObjectType type, u64 handle) {
    const s64 now = SteadyNs();
    std::scoped_lock lock{g_addr_mutex};
    if (bind) {
        ++g_addr_binds;
        g_addr_widest = std::max(g_addr_widest, size);
        g_live.emplace(base, AddressRange{base, size, handle, type, internal, now, 0, {}});
        return;
    }
    ++g_addr_unbinds;
    AddressRange freed{base, size, handle, type, internal, 0, now, {}};
    for (auto [it, end] = g_live.equal_range(base); it != end; ++it) {
        if (it->second.handle == handle) {
            freed.bound_ns = it->second.bound_ns;
            freed.size = std::max(freed.size, it->second.size);
            g_live.erase(it);
            break;
        }
    }
    if (const auto name = g_names.find(handle); name != g_names.end()) {
        freed.name = std::move(name->second);
        g_names.erase(name);
    }
    g_freed.push_back(std::move(freed));
    if (g_freed.size() > FreedKept) {
        g_freed.pop_front();
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL AddressCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT,
                                               vk::DebugUtilsMessageTypeFlagsEXT type,
                                               const vk::DebugUtilsMessengerCallbackDataEXT* data,
                                               void*) {
    if (!(type & vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding) || data == nullptr) {
        return VK_FALSE;
    }
    for (auto* p = static_cast<const VkBaseInStructure*>(data->pNext); p != nullptr; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            continue;
        }
        const auto* b = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(p);
        const bool has_object = data->objectCount != 0 && data->pObjects != nullptr;
        NoteBinding(b->baseAddress, b->size, b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT,
                    (b->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0,
                    has_object ? data->pObjects[0].objectType : vk::ObjectType::eUnknown,
                    has_object ? data->pObjects[0].objectHandle : 0);
    }
    return VK_FALSE;
}

// Under g_addr_mutex.
std::string DescribeRange(const AddressRange& r, bool live) {
    std::string name = r.name;
    if (live) {
        if (const auto it = g_names.find(r.handle); it != g_names.end()) {
            name = it->second;
        }
    }
    std::string text = fmt::format("{} {:#x}{} [{:#x}, {:#x}) {} KiB, bound at {}", vk::to_string(r.type),
                                   r.handle, name.empty() ? "" : fmt::format(" \"{}\"", name), r.base,
                                   r.base + r.size, r.size >> 10, r.bound_ns ? Seconds(r.bound_ns) : "?");
    if (!live) {
        text += fmt::format(", FREED at {}", Seconds(r.freed_ns));
    }
    if (r.internal) {
        text += " (the driver's own)";
    }
    return text;
}

// Who held the faulting range [low, high): the live objects in it, the nearest live ones below and
// above, the freed ones that held it (the newest first).
void AddOwners(std::vector<std::string>& out, u64 low, u64 high) {
    if (!g_addr_on.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{g_addr_mutex};
    const AddressRange* below = nullptr;
    u32 inside = 0;
    for (auto it = g_live.lower_bound(low > g_addr_widest ? low - g_addr_widest : 0);
         it != g_live.end() && it->first < high; ++it) {
        const AddressRange& r = it->second;
        const u64 end = r.base + r.size;
        if (end > low) {
            out.push_back(fmt::format("    inside a live {} (+{:#x})", DescribeRange(r, true),
                                      low - std::min(low, r.base)));
            ++inside;
        } else if (below == nullptr || end > below->base + below->size) {
            below = &r;
        }
    }
    if (inside == 0) {
        out.push_back("    no live object holds it");
        if (below != nullptr) {
            out.push_back(fmt::format("    the nearest below ends {:#x} bytes before it: {}",
                                      low - (below->base + below->size), DescribeRange(*below, true)));
        }
        if (const auto above = g_live.lower_bound(high); above != g_live.end()) {
            out.push_back(fmt::format("    the nearest above starts {:#x} bytes after it: {}",
                                      above->first - high, DescribeRange(above->second, true)));
        }
    }
    u32 freed = 0;
    for (auto it = g_freed.rbegin(); it != g_freed.rend() && freed < 6; ++it) {
        if (it->base < high && it->base + it->size > low) {
            out.push_back(fmt::format("    held before by a {}", DescribeRange(*it, false)));
            ++freed;
        }
    }
}

void AddAddressSummary(std::vector<std::string>& out) {
    if (!g_addr_on.load(std::memory_order_relaxed)) {
        out.push_back("Address report: off (VK_EXT_device_address_binding_report: with the GPU "
                      "markers, where the driver has it)");
        return;
    }
    std::scoped_lock lock{g_addr_mutex};
    out.push_back(fmt::format("Address report: {} binds, {} frees seen; {} live object range(s), the "
                              "widest {} MiB; the last {} freed kept",
                              g_addr_binds, g_addr_unbinds, g_live.size(), g_addr_widest >> 20,
                              g_freed.size()));
}

void AddFault(std::vector<std::string>& out) {
    if (!g_fault) {
        out.push_back("Driver fault report: not available on this device (VK_EXT_device_fault)");
        return;
    }
    vk::DeviceFaultCountsEXT counts{};
    vk::Result r = g_device.getFaultInfoEXT(&counts, nullptr);
    if (r != vk::Result::eSuccess && r != vk::Result::eIncomplete) {
        out.push_back(fmt::format("Driver fault report: the count query failed ({})",
                                  vk::to_string(r)));
        return;
    }
    std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<vk::DeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    const u64 binary_size = counts.vendorBinarySize;
    counts.vendorBinarySize = 0; // the vendor's binary dump is not asked for
    vk::DeviceFaultInfoEXT info{};
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    r = g_device.getFaultInfoEXT(&counts, &info);
    if (r != vk::Result::eSuccess && r != vk::Result::eIncomplete) {
        out.push_back(fmt::format("Driver fault report: the query failed ({})", vk::to_string(r)));
        return;
    }
    out.push_back(fmt::format("Driver fault report: \"{}\" ({} address(es), {} vendor record(s), "
                              "a {} byte vendor dump not kept)",
                              info.description.data(), counts.addressInfoCount,
                              counts.vendorInfoCount, binary_size));
    for (u32 i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
        const auto& a = addresses[i];
        const u64 precision = std::max<u64>(a.addressPrecision, 1);
        const u64 low = a.reportedAddress & ~(precision - 1);
        out.push_back(fmt::format("  fault address {}: {} at {:#x} (between {:#x} and {:#x})", i,
                                  AddressType(a.addressType), a.reportedAddress, low,
                                  low + precision));
        if (a.addressType == vk::DeviceFaultAddressTypeEXT::eReadInvalid ||
            a.addressType == vk::DeviceFaultAddressTypeEXT::eWriteInvalid ||
            a.addressType == vk::DeviceFaultAddressTypeEXT::eExecuteInvalid) {
            AddOwners(out, low, low + precision);
        }
    }
    AddAddressSummary(out);
    for (u32 i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i) {
        const auto& v = vendors[i];
        out.push_back(fmt::format("  vendor record {}: \"{}\" code {:#x} data {:#x}", i,
                                  v.description.data(), v.vendorFaultCode, v.vendorFaultData));
    }
}

void AddCheckpoints(std::vector<std::string>& out) {
    if (!CheckpointsOn()) {
        out.push_back("GPU markers: off (VR_GPU_CHECKPOINTS=1 turns them on; NVIDIA only)");
        return;
    }
    out.push_back(fmt::format("GPU markers: {} recorded", g_next_id.load()));
    for (const auto& q : g_queues) {
        u32 count = 0;
        q.queue.getCheckpointDataNV(&count, nullptr);
        std::vector<vk::CheckpointDataNV> data(count);
        if (count != 0) {
            q.queue.getCheckpointDataNV(&count, data.data());
        }
        u64 top = 0, bottom = 0;
        for (u32 i = 0; i < count; ++i) {
            const u64 id = static_cast<u64>(reinterpret_cast<uintptr_t>(data[i].pCheckpointMarker));
            out.push_back(fmt::format("  {} queue, {}: {}", q.name, vk::to_string(data[i].stage),
                                      Describe(id)));
            if (data[i].stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                top = id;
            } else if (data[i].stage == vk::PipelineStageFlagBits::eBottomOfPipe) {
                bottom = id;
            }
        }
        if (count == 0) {
            out.push_back(fmt::format("  {} queue: no marker reached", q.name));
        }
        // What the GPU was in: the passes and dispatches recorded for the command buffers from
        // the last marker finished to the last started (by tick: the recording thread runs ahead
        // of the replay that begins and ends the command buffers).
        const Entry* t = top ? &g_ring[top & (RingSize - 1)] : nullptr;
        const Entry* b = bottom ? &g_ring[bottom & (RingSize - 1)] : nullptr;
        if (t && b && t->id.load() == top && b->id.load() == bottom && t->source == b->source) {
            const u64 low = std::min(t->tick, b->tick), high = std::max(t->tick, b->tick);
            std::vector<u64> ids;
            for (u64 i = 0; i < RingSize; ++i) {
                const Entry& e = g_ring[i];
                const u64 id = e.id.load(std::memory_order_acquire);
                if (id != 0 && e.source == t->source && e.tick >= low && e.tick <= high &&
                    (e.kind == Kind::RenderPass || e.kind == Kind::Work)) {
                    ids.push_back(id);
                }
            }
            std::sort(ids.begin(), ids.end());
            out.push_back(fmt::format("  {} queue: the {} tick(s) {}..{} hold {} pass(es) and "
                                      "dispatch(es); the GPU was at or after the marker it started "
                                      "last, before the next:",
                                      q.name, SourceName(t->source), low, high, ids.size()));
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids.size() > 64 && i == 16) {
                    out.push_back(fmt::format("    ... {} more ...", ids.size() - 64));
                    i = ids.size() - 48;
                }
                out.push_back(fmt::format("    {}{}", ids[i] == top ? "> " : "", Describe(ids[i])));
            }
        }
    }
    // The newest markers recorded, whatever reached the GPU.
    const u64 newest = g_next_id.load();
    out.push_back("GPU markers recorded last (the newest at the bottom):");
    for (u64 id = newest > 24 ? newest - 23 : 1; id <= newest; ++id) {
        out.push_back("    " + Describe(id));
    }
}

void AddTimeline(std::vector<std::string>& out) {
    const u64 submitted = g_game_last_tick.load();
    if (!g_timeline) {
        out.push_back(fmt::format("Game timeline: not watched (last submitted tick {})", submitted));
        return;
    }
    u64 done = 0;
    const vk::Result r = g_device.getSemaphoreCounterValue(g_timeline, &done);
    if (r != vk::Result::eSuccess) {
        out.push_back(fmt::format("Game timeline: unreadable ({}); last submitted tick {}",
                                  vk::to_string(r), submitted));
        return;
    }
    out.push_back(fmt::format(
        "Game timeline: the GPU finished tick {} of {} submitted: {}", done, submitted,
        done >= submitted ? "the GPU has done all it was given (the CPU side stopped submitting)"
                          : fmt::format("the GPU is {} tick(s) behind (busy or stuck on tick {})",
                                        submitted - done, done + 1)));
}

void AddSubmits(std::vector<std::string>& out) {
    const u64 n = g_submit_count.load();
    out.push_back(fmt::format("Submits: {} in all ({} the game's), the last ones:", n,
                              g_game_submits.load()));
    for (u64 i = n > SubmitRing ? n - SubmitRing : 0; i < n; ++i) {
        const auto& s = g_submits[i % SubmitRing];
        out.push_back(fmt::format("  {} tick {} at {}: {} wait(s){}", SourceName(s.source), s.tick,
                                  Seconds(s.ns), s.waits,
                                  s.sparse_wait ? fmt::format(", sparse binds {}", s.sparse_wait)
                                                : std::string{}));
    }
    const u64 b = g_bind_count.load();
    if (b != 0) {
        out.push_back(fmt::format("Sparse bind batches: {} in all, the last ones:", b));
        for (u64 i = b > BindRing ? b - BindRing : 0; i < b; ++i) {
            const auto& s = g_binds[i % BindRing];
            out.push_back(fmt::format("  value {} at {}: {} bind(s)", s.value, Seconds(s.ns),
                                      s.binds));
        }
    }
}

#ifdef _WIN32
std::string Utf8(const wchar_t* text, int length = -1) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

std::string Between(const std::string& xml, std::string_view open, std::string_view close,
                    size_t& from) {
    const size_t a = xml.find(open, from);
    if (a == std::string::npos) {
        from = std::string::npos;
        return {};
    }
    const size_t b = xml.find(close, a + open.size());
    if (b == std::string::npos) {
        from = std::string::npos;
        return {};
    }
    from = b + close.size();
    return xml.substr(a + open.size(), b - a - open.size());
}

// One event, as a line: its time, source, id and texts.
std::string EventLine(EVT_HANDLE event) {
    DWORD used = 0, properties = 0;
    EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &used, &properties);
    if (used == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(used / sizeof(wchar_t) + 1);
    if (!EvtRender(nullptr, event, EvtRenderEventXml, static_cast<DWORD>(buffer.size() *
                                                                         sizeof(wchar_t)),
                   buffer.data(), &used, &properties)) {
        return {};
    }
    const std::string xml = Utf8(buffer.data());
    size_t at = 0;
    const std::string provider = Between(xml, "Provider Name='", "'", at);
    at = 0;
    std::string id = Between(xml, "<EventID", "</EventID>", at);
    if (const size_t gt = id.find('>'); gt != std::string::npos) {
        id = id.substr(gt + 1);
    }
    at = 0;
    const std::string time = Between(xml, "SystemTime='", "'", at);
    // Every <Data> or <Data Name='...'> value (a TDR's names the driver; NVIDIA's carry the Xid).
    std::string texts;
    for (size_t from = 0; texts.size() < 1500;) {
        const size_t open = xml.find("<Data", from);
        if (open == std::string::npos) {
            break;
        }
        const size_t gt = xml.find('>', open);
        if (gt == std::string::npos) {
            break;
        }
        from = gt + 1;
        const char after = xml[open + 5];
        if ((after != '>' && after != ' ') || xml[gt - 1] == '/') {
            continue; // <DataItem ...>, or an empty <Data/>
        }
        const size_t close = xml.find("</Data>", gt);
        if (close == std::string::npos) {
            break;
        }
        from = close + 7;
        const std::string data = xml.substr(gt + 1, close - gt - 1);
        if (!data.empty()) {
            texts += (texts.empty() ? "" : " | ") + data;
        }
    }
    // Event data that are binary (NVIDIA's Xid detail) or not named <Data>: keep a short tail.
    if (texts.empty()) {
        at = 0;
        texts = Between(xml, "<EventData>", "</EventData>", at).substr(0, 600);
    }
    return fmt::format("{} {} event {}: {}", time, provider, id, texts);
}

// Windows' display driver events of the last ten minutes (System log): NVIDIA's (nvlddmkm, its
// Xid), AMD's, Intel's, and a TDR's ("Display" 4101). The newest first, at most eight.
std::vector<std::string> QueryDriverEvents() {
    const wchar_t* query =
        L"*[System[(Provider[@Name='nvlddmkm'] or Provider[@Name='Display'] or "
        L"Provider[@Name='amdkmdag'] or Provider[@Name='amdkmdap'] or Provider[@Name='igfx'] or "
        L"Provider[@Name='igfxn']) and TimeCreated[timediff(@SystemTime) <= 600000]]]";
    EVT_HANDLE results =
        EvtQuery(nullptr, L"System", query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) {
        return {fmt::format("Windows display driver events: the query failed (error {})",
                            GetLastError())};
    }
    std::vector<std::string> lines;
    std::array<EVT_HANDLE, 8> events{};
    DWORD returned = 0;
    if (EvtNext(results, static_cast<DWORD>(events.size()), events.data(), 2000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            std::string line = EventLine(events[i]);
            if (!line.empty()) {
                lines.push_back("  " + line);
            }
            EvtClose(events[i]);
        }
    }
    EvtClose(results);
    return lines;
}
#endif

void AddDriverEvents(std::vector<std::string>& out) {
#ifdef _WIN32
    // The kernel driver logs its event as it resets; the user-mode driver's lost device can come
    // first. A few short waits for one newer than this process's start.
    const std::time_t started = std::time(nullptr) -
                                static_cast<std::time_t>((SteadyNs() - g_start_ns) / 1'000'000'000);
    std::tm utc{};
    gmtime_s(&utc, &started);
    char since[32]{};
    std::strftime(since, sizeof(since), "%Y-%m-%dT%H:%M:%S", &utc);
    std::vector<std::string> lines;
    for (int attempt = 0; attempt < 4; ++attempt) {
        lines = QueryDriverEvents();
        const bool fresh = std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
            return l.size() > 2 + 19 && l.compare(2, 19, since) >= 0;
        });
        if (fresh) {
            // The reset's own event ("Display" 4101, a TDR) follows NVIDIA's by about a second.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            lines = QueryDriverEvents();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
    out.push_back(fmt::format("Windows display driver events, last 10 minutes (UTC; this run "
                              "started {}Z):",
                              since));
    if (lines.empty()) {
        out.push_back("  none");
    }
    out.insert(out.end(), lines.begin(), lines.end());
#endif
}

void Write(const std::vector<std::string>& lines) {
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "gpu-crash.txt";
    if (std::filesystem::file_size(path, ec) > 1024 * 1024 && !ec) {
        std::filesystem::remove(dir / "gpu-crash.previous.txt", ec);
        std::filesystem::rename(path, dir / "gpu-crash.previous.txt", ec);
    }
#ifdef _WIN32
    FILE* file = _wfopen(path.c_str(), L"ab");
#else
    FILE* file = std::fopen(path.c_str(), "ab");
#endif
    if (!file) {
        return;
    }
    for (const auto& line : lines) {
        std::fwrite(line.data(), 1, line.size(), file);
        std::fputc('\n', file);
    }
    std::fputc('\n', file);
    std::fclose(file);
}

void WriteReport(std::string_view where) {
    std::vector<std::string> lines;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    lines.push_back(fmt::format("==== {} GPU device lost: {} (thread {}), {} after start", stamp,
                                where, Common::GetCurrentThreadName(), Seconds(SteadyNs())));
    lines.push_back(fmt::format("Build {} {} ({})", Common::g_version,
                                Common::g_scm_desc[0] ? Common::g_scm_desc : "",
                                Common::g_scm_rev[0] ? Common::g_scm_rev : "unknown"));
    lines.push_back(Common::RenderDiagnostic::Game());
    const auto step = [&](const char* what, auto&& add) {
        try {
            add(lines);
        } catch (const std::exception& e) {
            lines.push_back(fmt::format("({} failed: {})", what, e.what()));
        } catch (...) {
            lines.push_back(fmt::format("({} failed)", what));
        }
    };
    step("GPU", AddGpu);
    step("fault report", AddFault);
    step("timeline", AddTimeline);
    step("markers", AddCheckpoints);
    step("submits", AddSubmits);
    step("memory", AddMemory);
    step("driver events", AddDriverEvents);

    Write(lines);
    if constexpr (Common::Log::Enabled) {
        for (const auto& line : lines) {
            LOG_CRITICAL(Render_Vulkan, "GPU crash: {}", line);
        }
    }
    Common::CrashRecord::Note(
        fmt::format("GPU device lost ({}): the details are in gpu-crash.txt", where).c_str());
    if (Common::CrashRecord::DumpsEnabled()) {
        const std::string dump = Common::CrashRecord::WriteDump("gpu-lost");
        if (!dump.empty()) {
            Common::CrashRecord::Note(fmt::format("a dump of every thread: {}", dump).c_str());
        }
    }
    Common::RenderDiagnostic::Write(fmt::format("GPU device lost: {} (gpu-crash.txt)", where));
    Common::Log::Flush();
}

void AddRecent(std::vector<std::string>& out) {
    if (!g_ring) {
        return;
    }
    const u64 newest = g_next_id.load();
    out.push_back("GPU markers recorded last (the newest at the bottom):");
    for (u64 id = newest > 32 ? newest - 31 : 1; id <= newest; ++id) {
        out.push_back("    " + Describe(id));
    }
}

void WriteFreezeReport(s64 stood_ns) {
    std::vector<std::string> lines;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    lines.push_back(fmt::format("==== {} FREEZE: the game has submitted nothing to the GPU for "
                                "{:.0f} s ({} after start; the process still runs)",
                                stamp, static_cast<double>(stood_ns) * 1e-9, Seconds(SteadyNs())));
    lines.push_back(fmt::format("Build {} {} ({})", Common::g_version,
                                Common::g_scm_desc[0] ? Common::g_scm_desc : "",
                                Common::g_scm_rev[0] ? Common::g_scm_rev : "unknown"));
    lines.push_back(Common::RenderDiagnostic::Game());
    const auto step = [&](const char* what, auto&& add) {
        try {
            add(lines);
        } catch (const std::exception& e) {
            lines.push_back(fmt::format("({} failed: {})", what, e.what()));
        } catch (...) {
            lines.push_back(fmt::format("({} failed)", what));
        }
    };
    step("GPU", AddGpu);
    step("timeline", AddTimeline);
    step("markers", AddRecent);
    step("submits", AddSubmits);
    step("memory", AddMemory);
    step("driver events", AddDriverEvents);
    std::string dump;
    if (Common::CrashRecord::DumpsEnabled()) {
        dump = Common::CrashRecord::WriteDump("freeze");
    }
    lines.push_back(dump.empty() ? std::string{"No dump of the threads (dumps off, or too many)"}
                                 : fmt::format("A dump of every thread: {}", dump));
    Write(lines);
    if constexpr (Common::Log::Enabled) {
        for (const auto& line : lines) {
            LOG_CRITICAL(Render_Vulkan, "GPU freeze: {}", line);
        }
    }
    Common::CrashRecord::Note(
        fmt::format("freeze: no GPU work for {:.0f} s, the details are in gpu-crash.txt",
                    static_cast<double>(stood_ns) * 1e-9)
            .c_str());
    Common::RenderDiagnostic::Write("Freeze: no GPU work for 15 s (gpu-crash.txt)");
    Common::Log::Flush();
}

bool WantFreezeWatch() {
    const char* v = std::getenv("VR_FREEZE_WATCH");
    if (v != nullptr && *v != '\0') {
        return v[0] != '0';
    }
    return Common::Log::Enabled;
}

// Once a second: the game's submits. Armed by the first; 15 s without one is a freeze, reported
// once until the game submits again.
void FreezeWatch() {
    Common::SetCurrentThreadName("bbvr:FreezeWatch");
    constexpr s64 Limit = 15'000'000'000;
    u64 seen = 0;
    s64 changed = SteadyNs();
    bool frozen = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_reported.load(std::memory_order_acquire)) {
            return; // the lost device's report has it all
        }
        const u64 n = g_game_submits.load(std::memory_order_acquire);
        const s64 now = SteadyNs();
        if (n != seen) {
            if (frozen) {
                LOG_WARNING(Render_Vulkan, "GPU freeze: over, the game submits again after {:.1f} s",
                            static_cast<double>(now - changed) * 1e-9);
                Common::RenderDiagnostic::Write(
                    fmt::format("Freeze over after {:.1f} s",
                                static_cast<double>(now - changed) * 1e-9));
                Common::Log::Flush();
            }
            seen = n;
            changed = now;
            frozen = false;
            continue;
        }
        if (n != 0 && !frozen && now - changed >= Limit) {
            frozen = true;
            try {
                WriteFreezeReport(now - changed);
            } catch (...) {
            }
        }
    }
}

} // namespace

bool WantCheckpoints() {
    const char* v = std::getenv("VR_GPU_CHECKPOINTS");
    if (v != nullptr && *v != '\0') {
        return v[0] != '0';
    }
    return Common::Log::Enabled;
}

bool WantAddressReport() {
    const char* v = std::getenv("VR_GPU_ADDRESSES");
    return WantCheckpoints() && (v == nullptr || v[0] != '0');
}

vk::UniqueDebugUtilsMessengerEXT AddressMessenger(vk::Instance instance) {
    if (!WantAddressReport()) {
        return {};
    }
    const vk::DebugUtilsMessengerCreateInfoEXT ci = {
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding,
        .pfnUserCallback = AddressCallback,
    };
    auto [result, messenger] = instance.createDebugUtilsMessengerEXTUnique(ci);
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "GPU crash report: no address messenger ({})",
                    vk::to_string(result));
        return {};
    }
    return std::move(messenger);
}

void SetAddressReport(bool on) {
    g_addr_on.store(on, std::memory_order_relaxed);
    names_on.store(on, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "GPU crash report: address report {}", on ? "on" : "off");
}

void NoteName(vk::ObjectType type, u64 handle, std::string_view name) {
    if (!KeepsName(type)) {
        return;
    }
    std::scoped_lock lock{g_addr_mutex};
    g_names[handle] = std::string(name);
}

void Init(vk::PhysicalDevice physical, vk::Device device, bool fault, bool checkpoints,
          bool memory_budget, std::span<const NamedQueue> queues) {
    g_physical = physical;
    g_device = device;
    g_fault = fault;
    g_budget = memory_budget;
    g_queues.clear();
    for (const auto& q : queues) {
        const bool seen = std::any_of(g_queues.begin(), g_queues.end(),
                                      [&](const NamedQueue& o) { return o.queue == q.queue; });
        if (q.queue && !seen) {
            g_queues.push_back(q);
        }
    }
    if (checkpoints && !g_ring) {
        g_ring = std::make_unique<Entry[]>(RingSize);
    }
    checkpoints_on.store(checkpoints, std::memory_order_relaxed);
    const bool watch = WantFreezeWatch();
    static std::atomic<bool> watching{false};
    if (watch && !watching.exchange(true)) {
        std::thread(FreezeWatch).detach();
    }
    LOG_INFO(Render_Vulkan,
             "GPU crash report: driver fault report {}, GPU markers {} ({} queue(s)), freeze "
             "watch {}, dumps {}",
             fault ? "on" : "unavailable", checkpoints ? "on" : "off", g_queues.size(),
             watch ? "on" : "off", Common::CrashRecord::DumpsEnabled() ? "on" : "off");
    RENDER_DIAGNOSTIC_ONCE("GPU crash report: driver fault report {}, GPU markers {}, freeze watch "
                           "{}, dumps {}",
                           fault ? "on" : "unavailable", checkpoints ? "on" : "off",
                           watch ? "on" : "off",
                           Common::CrashRecord::DumpsEnabled() ? "on" : "off");
}

u64 Mark(Kind kind, u64 tick, u64 a, u32 x, u32 y, u32 extra, Source source) {
    if (!g_ring) {
        return 0;
    }
    const u64 id = g_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
    Entry& e = g_ring[id & (RingSize - 1)];
    e.id.store(0, std::memory_order_relaxed);
    e.kind = kind;
    e.source = source;
    e.tick = tick;
    e.a = a;
    e.x = x;
    e.y = y;
    e.extra = extra;
    e.ns = SteadyNs();
    e.draws = 0;
    e.first_vs = e.first_ps = e.last_vs = e.last_ps = 0;
    e.id.store(id, std::memory_order_release);
    if (kind == Kind::RenderPass) {
        g_last_pass.store(id, std::memory_order_relaxed);
    }
    return id;
}

void NoteDraw(u64 vs, u64 ps) {
    const u64 id = g_last_pass.load(std::memory_order_relaxed);
    if (id == 0 || !g_ring) {
        return;
    }
    Entry& e = g_ring[id & (RingSize - 1)];
    if (e.id.load(std::memory_order_relaxed) != id) {
        return;
    }
    if (e.draws++ == 0) {
        e.first_vs = vs;
        e.first_ps = ps;
    }
    e.last_vs = vs;
    e.last_ps = ps;
}

void NoteSubmit(u64 tick, u32 waits, u64 sparse_wait, Source source) {
    // Submits from the game's and the presenter's schedulers may come at once: a slot each.
    const u64 n = g_submit_count.fetch_add(1, std::memory_order_acq_rel);
    g_submits[n % SubmitRing] = {tick, waits, sparse_wait, SteadyNs(), source};
    if (source == Source::Game) {
        g_game_last_tick.store(tick, std::memory_order_relaxed);
        g_game_submits.fetch_add(1, std::memory_order_release);
    }
}

void WatchTimeline(vk::Semaphore timeline) {
    g_timeline = timeline;
}

void NoteSparseBind(u32 binds, u64 value) {
    const u64 n = g_bind_count.load(std::memory_order_relaxed);
    g_binds[n % BindRing] = {binds, value, SteadyNs()};
    g_bind_count.store(n + 1, std::memory_order_release);
}

bool Report(vk::Result result, std::string_view where) {
    if (result != vk::Result::eErrorDeviceLost) {
        return false;
    }
    if (g_reported.exchange(true, std::memory_order_acq_rel)) {
        // Another thread saw it first (the VR frame thread, say) and is writing the report: this
        // one (a game submit about to end the process) waits for it, at most 10 s.
        for (int i = 0; i < 200 && !g_written.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
    try {
        WriteReport(where);
    } catch (...) {
        // Nothing more can be done while the device is gone.
    }
    g_written.store(true, std::memory_order_release);
    return true;
}

} // namespace Vulkan::GpuCrash
