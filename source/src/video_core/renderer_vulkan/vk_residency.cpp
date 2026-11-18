// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_residency.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#endif

namespace Vulkan::Residency {

#ifdef _WIN32
namespace {
// pdh.dll loaded here, not linked: the emulator needs it for nothing else.
struct Pdh {
    decltype(&PdhOpenQueryW) open{};
    decltype(&PdhAddEnglishCounterW) add{};
    decltype(&PdhCollectQueryData) collect{};
    decltype(&PdhGetFormattedCounterValue) value{};
    decltype(&PdhCloseQuery) close{};

    bool Load() {
        const HMODULE pdh = LoadLibraryW(L"pdh.dll");
        if (!pdh) {
            return false;
        }
        open = reinterpret_cast<decltype(open)>(GetProcAddress(pdh, "PdhOpenQueryW"));
        add = reinterpret_cast<decltype(add)>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
        collect = reinterpret_cast<decltype(collect)>(GetProcAddress(pdh, "PdhCollectQueryData"));
        value = reinterpret_cast<decltype(value)>(GetProcAddress(pdh, "PdhGetFormattedCounterValue"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(pdh, "PdhCloseQuery"));
        return open && add && collect && value && close;
    }
};
} // namespace

Reader::Reader(const std::array<u8, 8>& luid) {
    auto pdh = std::make_shared<Pdh>();
    if (!pdh->Load()) {
        return;
    }
    LUID id{};
    std::memcpy(&id, luid.data(), sizeof(id));
    // Task Manager's per-process GPU memory counters, for this process on this adapter: what
    // Windows keeps in the GPU's memory and what it holds in system memory now.
    wchar_t instance[96];
    std::swprintf(instance, std::size(instance), L"pid_%lu_luid_0x%08X_0x%08X_phys_0",
                  GetCurrentProcessId(), static_cast<unsigned>(id.HighPart),
                  static_cast<unsigned>(id.LowPart));
    const std::wstring local_path =
        std::wstring(L"\\GPU Process Memory(") + instance + L")\\Local Usage";
    const std::wstring non_local_path =
        std::wstring(L"\\GPU Process Memory(") + instance + L")\\Non Local Usage";
    found = true;
    // Counter reads take milliseconds: their own thread, every PeriodMs; Read takes the last.
    sampler = std::jthread([this, pdh, local_path, non_local_path](std::stop_token stop) {
        PDH_HQUERY query{};
        PDH_HCOUNTER local{};
        PDH_HCOUNTER non_local{};
        std::mutex wait_mutex;
        std::condition_variable_any wait;
        while (!stop.stop_requested()) {
            // The instance exists once the process has GPU memory: added until it does.
            if (!query && pdh->open(nullptr, 0, &query) != ERROR_SUCCESS) {
                query = nullptr;
            }
            if (query && !local &&
                (pdh->add(query, local_path.c_str(), 0, &local) != ERROR_SUCCESS ||
                 pdh->add(query, non_local_path.c_str(), 0, &non_local) != ERROR_SUCCESS)) {
                pdh->close(query);
                query = nullptr;
                local = non_local = nullptr;
            }
            if (query && local && pdh->collect(query) == ERROR_SUCCESS) {
                PDH_FMT_COUNTERVALUE l{};
                PDH_FMT_COUNTERVALUE n{};
                if (pdh->value(local, PDH_FMT_LARGE, nullptr, &l) == ERROR_SUCCESS &&
                    pdh->value(non_local, PDH_FMT_LARGE, nullptr, &n) == ERROR_SUCCESS &&
                    l.CStatus == ERROR_SUCCESS && n.CStatus == ERROR_SUCCESS) {
                    std::scoped_lock lock{reading_mutex};
                    reading = {static_cast<u64>(l.largeValue), static_cast<u64>(n.largeValue),
                               reading.sample + 1};
                    read_at = std::chrono::steady_clock::now();
                    have_reading = true;
                }
            }
            std::unique_lock lock{wait_mutex};
            wait.wait_for(lock, stop, std::chrono::milliseconds(PeriodMs), [] { return false; });
        }
        if (query) {
            pdh->close(query);
        }
    });
}

Reader::~Reader() = default; // the sampler stops and joins

std::optional<Reader::Usage> Reader::Read() const {
    std::scoped_lock lock{reading_mutex};
    if (!have_reading ||
        std::chrono::steady_clock::now() - read_at > std::chrono::milliseconds(StaleMs)) {
        return std::nullopt; // none yet, or the counters have failed since
    }
    return reading;
}
#else
Reader::Reader(const std::array<u8, 8>&) {}
Reader::~Reader() = default;
std::optional<Reader::Usage> Reader::Read() const {
    return std::nullopt;
}
#endif

} // namespace Vulkan::Residency
