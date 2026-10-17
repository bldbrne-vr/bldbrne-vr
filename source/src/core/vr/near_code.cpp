// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>
#include <mutex>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/memory.h"
#include "core/vr/near_code.h"

namespace VR::Hook {

namespace {

constexpr std::size_t RegionSize = 64_KB;
constexpr u64 TrampolineAreaSize = 8_MB; // Module::LoadModuleToMemory's cpu_patches area

std::mutex g_mutex;
u8* g_region = nullptr;
std::size_t g_used = 0;
bool g_failed = false;

} // Anonymous namespace

bool Rel32(u64 next, u64 target, s32& out) {
    const s64 delta = static_cast<s64>(target - next);
    if (delta < std::numeric_limits<s32>::min() || delta > std::numeric_limits<s32>::max()) {
        return false;
    }
    out = static_cast<s32>(delta);
    return true;
}

u8* NearAlloc(u64 module_base, u64 module_size, std::size_t bytes, std::size_t align) {
    std::scoped_lock lk{g_mutex};
    if (g_region == nullptr) {
        if (g_failed) {
            return nullptr;
        }
        g_failed = true; // until the mapping below succeeds
        auto* const memory = Core::Memory::Instance();
        const u64 hint = Common::AlignUp(
            module_base + Common::AlignUp(module_size, u64{0x1000}) + TrampolineAreaSize, u64{64_KB});
        void* address = nullptr;
        const s32 result = memory->MapMemory(&address, hint, RegionSize,
                                             Core::MemoryProt::CpuReadWrite |
                                                 Core::MemoryProt::CpuExec,
                                             Core::MemoryMapFlags::NoFlags, Core::VMAType::Code,
                                             "VR near code");
        if (result != ORBIS_OK || address == nullptr) {
            LOG_WARNING(Core, "VR near code: no memory after the eboot (hint {:#x}, result {:#x})",
                        hint, static_cast<u32>(result));
            return nullptr;
        }
        const u64 start = reinterpret_cast<u64>(address);
        s32 unused{};
        // Every byte of the region must be reachable from every byte of the module.
        if (!Rel32(module_base, start + RegionSize, unused) ||
            !Rel32(module_base + module_size, start, unused) ||
            !Rel32(start + RegionSize, module_base, unused)) {
            LOG_WARNING(Core,
                        "VR near code: the region at {:#x} is out of rel32 reach of the eboot "
                        "({:#x}..{:#x}); not used",
                        start, module_base, module_base + module_size);
            return nullptr;
        }
        g_region = static_cast<u8*>(address);
        std::memset(g_region, 0, RegionSize);
        g_failed = false;
        LOG_INFO(Core, "VR near code: {} KB at {:#x} (eboot {:#x}, +{:#x})", RegionSize / 1024,
                 start, module_base, start - module_base);
    }
    const std::size_t offset = Common::AlignUp(g_used, align == 0 ? 1 : align);
    if (offset + bytes > RegionSize) {
        LOG_WARNING(Core, "VR near code: region full ({} of {} bytes used, {} asked)", g_used,
                    RegionSize, bytes);
        return nullptr;
    }
    g_used = offset + bytes;
    return g_region + offset;
}

} // namespace VR::Hook
