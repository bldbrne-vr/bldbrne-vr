// SPDX-License-Identifier: GPL-2.0-or-later
#include "vr_render_core_placement.h"

#ifdef _WIN32
#include <algorithm>
#include <cstdlib>
#include <windows.h>

namespace Vulkan {
namespace {
bool ThreadUnrestricted(HANDLE thread, DWORD_PTR system_mask) {
    GROUP_AFFINITY affinity{};
    ULONG count{};
    return GetThreadGroupAffinity(thread, &affinity) && affinity.Group == 0 &&
           affinity.Mask == system_mask && GetThreadSelectedCpuSets(thread, nullptr, 0, &count) &&
           count == 0;
}

void ClearOwnedThreadSet(HANDLE thread, ULONG installed_id) {
    ULONG id{}, count{};
    if (thread && GetThreadSelectedCpuSets(thread, &id, 1, &count) && count == 1 &&
        id == installed_id) {
        SetThreadSelectedCpuSets(thread, nullptr, 0);
    }
}
} // namespace

bool RenderCorePlacement::Apply(HANDLE stage, HANDLE recorder) {
    const char* reserve = std::getenv("FAST_STAGE_B_CORE_RESERVE");
    const char* any = std::getenv("VR_ANY_CORE");
    if (!stage || !recorder || stage == recorder || stage_thread || (reserve && *reserve == '0') ||
        (any && *any == '1')) {
        return false;
    }
    // Enable only on the measured topology: eight identical SMT pairs sharing an LLC and NUMA
    // node. Leave hybrid scheduling, custom CPU sets and hard affinity alone.
    constexpr size_t logical_count = 16;
    HANDLE process = GetCurrentProcess();
    DWORD_PTR process_mask{}, system_mask{};
    ULONG count{};
    if (GetActiveProcessorGroupCount() != 1 || GetActiveProcessorCount(0) != logical_count ||
        !GetProcessAffinityMask(process, &process_mask, &system_mask) ||
        process_mask != system_mask ||
        !GetProcessDefaultCpuSets(process, nullptr, 0, &count) || count != 0 ||
        !ThreadUnrestricted(stage, system_mask) || !ThreadUnrestricted(recorder, system_mask)) {
        return false;
    }
    std::array<SYSTEM_CPU_SET_INFORMATION, logical_count> sets{};
    ULONG bytes{};
    if (!GetSystemCpuSetInformation(sets.data(), sizeof(sets), &bytes, process, 0) ||
        bytes != sizeof(sets)) {
        return false;
    }
    DWORD_PTR seen{};
    for (const auto& entry : sets) {
        const auto& cpu = entry.CpuSet;
        if (entry.Size != sizeof(entry) || entry.Type != CpuSetInformation || cpu.Group != 0 ||
            cpu.EfficiencyClass != sets[0].CpuSet.EfficiencyClass ||
            cpu.LastLevelCacheIndex != sets[0].CpuSet.LastLevelCacheIndex ||
            cpu.NumaNodeIndex != sets[0].CpuSet.NumaNodeIndex || cpu.Parked || cpu.Allocated ||
            cpu.RealTime || cpu.LogicalProcessorIndex >= sizeof(seen) * 8) {
            return false;
        }
        seen |= DWORD_PTR{1} << cpu.LogicalProcessorIndex;
    }
    if (seen != system_mask)
        return false;

    std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) {
        if (a.CpuSet.CoreIndex != b.CpuSet.CoreIndex)
            return a.CpuSet.CoreIndex < b.CpuSet.CoreIndex;
        return a.CpuSet.LogicalProcessorIndex < b.CpuSet.LogicalProcessorIndex;
    });
    for (size_t i = 0; i < sets.size(); i += 2) {
        if (sets[i].CpuSet.CoreIndex != sets[i + 1].CpuSet.CoreIndex ||
            (i && sets[i].CpuSet.CoreIndex == sets[i - 1].CpuSet.CoreIndex)) {
            return false;
        }
    }
    constexpr size_t stage_index = 8, recorder_index = 10;
    stage_id = sets[stage_index].CpuSet.Id;
    recorder_id = sets[recorder_index].CpuSet.Id;
    size_t next = 0;
    for (const auto& entry : sets) {
        if (entry.CpuSet.CoreIndex != sets[stage_index].CpuSet.CoreIndex)
            other_ids[next++] = entry.CpuSet.Id;
    }
    std::sort(other_ids.begin(), other_ids.end());
    if (!SetThreadSelectedCpuSets(stage, &stage_id, 1))
        return false;
    stage_thread = stage;
    if (SetThreadSelectedCpuSets(recorder, &recorder_id, 1)) {
        recorder_thread = recorder;
        installed_defaults =
            SetProcessDefaultCpuSets(process, other_ids.data(), static_cast<ULONG>(other_ids.size()));
    }
    if (!installed_defaults)
        Reset();
    return installed_defaults;
}

void RenderCorePlacement::Reset() {
    if (installed_defaults) {
        // A user can change scheduling while playing. Undo only settings we still own.
        decltype(other_ids) ids{};
        ULONG count{};
        if (GetProcessDefaultCpuSets(GetCurrentProcess(), ids.data(),
                                     static_cast<ULONG>(ids.size()), &count) &&
            count == ids.size()) {
            std::sort(ids.begin(), ids.end());
            if (ids == other_ids)
                SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
        }
        installed_defaults = false;
    }
    ClearOwnedThreadSet(recorder_thread, recorder_id);
    ClearOwnedThreadSet(stage_thread, stage_id);
    stage_thread = recorder_thread = nullptr;
}
} // namespace Vulkan
#endif
