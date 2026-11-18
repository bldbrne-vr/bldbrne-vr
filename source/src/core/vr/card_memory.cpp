// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/card_memory.h"

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace VR {

namespace {
u64 Ask() {
    if (const char* const env = std::getenv("VR_CARD_GB"); env != nullptr && env[0] != 0) {
        const double gib = std::clamp(std::atof(env), 0.0, 1024.0);
        return static_cast<u64>(gib * 1073741824.0);
    }
#ifdef _WIN32
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        return 0;
    }
    u64 most = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        if (adapter == nullptr) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            most = std::max<u64>(most, desc.DedicatedVideoMemory);
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return most;
#else
    return 0;
#endif
}
} // namespace

u64 CardMemoryBytes() {
    static const u64 bytes = Ask();
    return bytes;
}

} // namespace VR
