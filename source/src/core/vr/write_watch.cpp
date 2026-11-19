// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/write_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace VR::WriteWatch {

namespace {

constexpr u32 MaxHits = 4096;
constexpr int StackWords = 64; // the return-address scan reads this many qwords above rsp

std::array<Hit, MaxHits> g_hits{};
std::atomic<u32> g_count{0};
std::array<std::atomic<const u8*>, Slots> g_address{};
std::array<u64, Slots> g_stride{};
const u8* g_module = nullptr;
u64 g_size = 0;
std::mutex g_mutex; // Arm / Disarm

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#ifdef _WIN32
void* g_handler = nullptr;

// First in the chain: only our two data breakpoints are handled; everything else goes on.
LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* const c = info->ContextRecord;
    int slot = -1;
    for (int k = 0; k < Slots && slot < 0; ++k) {
        slot = (c->Dr6 & (1ull << k)) != 0 ? k : -1;
    }
    if (slot < 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    c->Dr6 = 0;
    const u8* const address = g_address[slot].load(std::memory_order_acquire);
    const u32 i = address != nullptr ? g_count.fetch_add(1, std::memory_order_relaxed) : MaxHits;
    if (i < MaxHits) {
        Hit& h = g_hits[i];
        h.ns = NowNs();
        h.thread = GetCurrentThreadId();
        h.slot = slot;
        h.rip = c->Rip;
        for (int k = 0; k < 3; ++k) {
            std::memcpy(&h.value[k], address + k * g_stride[slot], sizeof(float));
        }
        h.stack_count = 0;
        const u64* const sp = reinterpret_cast<const u64*>(c->Rsp);
        const u64 lo = reinterpret_cast<u64>(g_module), hi = lo + g_size;
        for (int k = 0; k < StackWords && h.stack_count < h.stack.size(); ++k) {
            if (sp[k] > lo && sp[k] < hi) {
                h.stack[h.stack_count++] = sp[k];
            }
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets DR0 .. DR2 (null: off) on every thread of the process but the calling one, each suspended
// for the moment it takes. Runs on its own thread so the caller's thread is set as well.
bool SetAll(const std::array<const void*, Slots>& addresses) {
    bool ok = true;
    std::thread worker([&] {
        const DWORD self = GetCurrentThreadId();
        const DWORD pid = GetCurrentProcessId();
        std::vector<DWORD> ids;
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            ok = false;
            return;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snap, &entry); more; more = Thread32Next(snap, &entry)) {
            if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) {
                ids.push_back(entry.th32ThreadID);
            }
        }
        CloseHandle(snap);
        for (const DWORD id : ids) {
            const HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                            THREAD_SET_CONTEXT,
                                        FALSE, id);
            if (t == nullptr) {
                continue; // it ended meanwhile
            }
            if (SuspendThread(t) != static_cast<DWORD>(-1)) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(t, &c)) {
                    // DR7 per slot k: Lk on (bit 2k), R/W = 01 (writes) and LEN = 11 (4 bytes)
                    // at bits 16 + 4k.
                    DWORD64* const dr[Slots] = {&c.Dr0, &c.Dr1, &c.Dr2};
                    for (int k = 0; k < Slots; ++k) {
                        c.Dr7 &= ~((3ull << (2 * k)) | (0xFull << (16 + 4 * k)));
                        *dr[k] = reinterpret_cast<u64>(addresses[k]);
                        if (addresses[k] != nullptr) {
                            c.Dr7 |= (1ull << (2 * k)) | (0xDull << (16 + 4 * k));
                        }
                    }
                    c.Dr6 = 0;
                    ok = SetThreadContext(t, &c) != 0 && ok;
                }
                ResumeThread(t);
            }
            CloseHandle(t);
        }
    });
    worker.join();
    return ok;
}
#endif

} // namespace

bool Arm(const std::array<const void*, Slots>& addresses, const std::array<u64, Slots>& strides,
         const void* module, u64 size) {
#ifdef _WIN32
    std::scoped_lock lock{g_mutex};
    if (g_handler == nullptr) {
        g_handler = AddVectoredExceptionHandler(1, OnException);
        if (g_handler == nullptr) {
            return false;
        }
    }
    g_stride = strides;
    g_module = static_cast<const u8*>(module);
    g_size = size;
    g_count.store(0, std::memory_order_relaxed);
    for (int k = 0; k < Slots; ++k) {
        g_address[k].store(static_cast<const u8*>(addresses[k]), std::memory_order_release);
    }
    return SetAll(addresses);
#else
    (void)addresses;
    (void)strides;
    (void)module;
    (void)size;
    return false;
#endif
}

void Disarm() {
#ifdef _WIN32
    std::scoped_lock lock{g_mutex};
    for (auto& address : g_address) {
        address.store(nullptr, std::memory_order_release);
    }
    SetAll({});
#endif
}

bool Armed() {
    for (const auto& address : g_address) {
        if (address.load(std::memory_order_acquire) != nullptr) {
            return true;
        }
    }
    return false;
}

std::vector<Hit> TakeHits(u32& dropped) {
    const u32 n = g_count.load(std::memory_order_acquire);
    dropped = n > MaxHits ? n - MaxHits : 0;
    return {g_hits.begin(), g_hits.begin() + std::min(n, MaxHits)};
}

} // namespace VR::WriteWatch
