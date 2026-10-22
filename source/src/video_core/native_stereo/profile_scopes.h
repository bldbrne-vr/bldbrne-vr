// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Profiling only. Inclusive CPU spans; non-nesting GPU categories.
#pragma once
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include "video_core/fast/lean_scopes.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
namespace ScratchProfile {
// Namespace-scope constants: no thread-safe static guard per scope (~45 scopes per draw).
inline const bool g_cpu_on = std::getenv("VR_CPU_PROFILE") != nullptr;
inline const unsigned long long g_cpu_mask = [] {
    if (!g_cpu_on) return 0ULL;
    const char* v = std::getenv("VR_CPU_PROFILE_MASK");
    return v ? std::strtoull(v, nullptr, 0) : ~0ULL; }();
// FAST_CPU_LEAN_SCOPES: the scopes that start a span and call End (out of line) are the profiled
// ones, so one that is off stores a flag and tests it twice; '0': all of them, as the inline
// constructor and out-of-line destructor did before.
inline const unsigned long long g_cpu_call_mask = FastRender::g_lean_scopes ? g_cpu_mask : ~0ULL;
inline bool CpuOn() { return g_cpu_on; }
inline bool CpuScopeOn(int id) { return (g_cpu_mask >> id) & 1; }
struct CpuData { std::array<double,64> us{}; std::array<unsigned long long,64> calls{}; std::array<const char*,64> names{}; std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now(); int depth{}; };
inline thread_local CpuData cpu;
struct Cpu {
 // The fields after `call` are written and read only when it is set (no zeroing per scope).
 bool call; bool on; int id; const char* name; std::chrono::steady_clock::rep start;
 Cpu(int i,const char* n):call((g_cpu_call_mask >> i) & 1) {
  if(call) [[unlikely]] {id=i;name=n;on=CpuScopeOn(i);if(on){start=std::chrono::steady_clock::now().time_since_epoch().count();++cpu.depth;}}
 }
 ~Cpu(){ if(call) [[unlikely]] End(); }
 SHAD_NO_INLINE void End(){if(!on)return;auto now=std::chrono::steady_clock::now();cpu.us[id]+=std::chrono::duration<double,std::micro>(now.time_since_epoch()-std::chrono::steady_clock::duration(start)).count();++cpu.calls[id];cpu.names[id]=name;
 if(--cpu.depth==0&&std::chrono::duration<double>(now-cpu.last).count()>=5){std::string s;for(int i=0;i<64;++i)if(cpu.calls[i])s+=fmt::format("; {} {:.3f} ms / {} calls",cpu.names[i],cpu.us[i]/1000,cpu.calls[i]);LOG_INFO(Render_Vulkan,"VR scratch CPU: {:.3f} s inclusive{}",std::chrono::duration<double>(now-cpu.last).count(),s);cpu.us={};cpu.calls={};cpu.last=now;}}
};
// FAST_GPU_PASS_PROFILE (bit 38): the categories' labels in the `FAST gpu passes:` line.
inline const char* GpuName(u64 id) {
 switch(id){
 case 10: return "buffer cache: CopyBuffer (guest DMA copy)";
 case 11: return "buffer cache: SynchronizeBuffer upload";
 case 12: return "buffer cache: WriteDataBuffer";
 case 13: return "buffer cache: DownloadBufferMemory";
 case 20: return "image upload";
 case 21: return "image download";
 case 22: return "image copy";
 case 23: return "image copy through a buffer";
 case 24: return "image clear";
 case 30: return "tile manager: TileImage (re-tile compute)";
 case 40: return "native stereo: buffer copy (both eyes)";
 default: return nullptr;
 }
}
inline Vulkan::GpuPassProfiler::Group GpuGroup(u64 id) { return id==40 ? Vulkan::GpuPassProfiler::Group::NativeStereo : Vulkan::GpuPassProfiler::Group::Transfer; }
struct Gpu { Vulkan::Scheduler& scheduler;bool open;Vulkan::GpuPassProfiler::Scope pass;
 Gpu(Vulkan::Scheduler& s,u64 id,bool pass_profile=true):scheduler(s),open(s.GpuCategoryBegin(id)),pass(s,0x5C47'0000'0000ull+id,GpuGroup(id),pass_profile?GpuName(id):nullptr){}
 ~Gpu(){if(open)scheduler.GpuWorkEnd();} };
}
