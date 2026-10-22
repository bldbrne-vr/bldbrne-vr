// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include "common/slow_scope.h"

#if !defined(BBVR_NO_LOGS) && defined(_WIN32)
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <future>
#include <memory>
#include <thread>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>

namespace Common::Slow {
namespace {

// Discovery walks the system thread list and can take tens of milliseconds. Run it on a
// worker; frame publication only consumes a ready snapshot and reads CPU counters.
struct CloseThreadHandle {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using ThreadHandle = std::unique_ptr<void, CloseThreadHandle>;

// The threads a long frame reports: one each, or (prefix) the sum of all whose name starts so.
struct Group {
    const wchar_t* name;
    const char* label;
    bool prefix;
    std::vector<ThreadHandle> handles{};
    std::vector<u64> last{};
};
using Groups = std::array<Group, 8>;
Groups EmptyGroups() {
    return Groups{{
        {L"Game:Main", "game main", false},
        {L"GXRenderThread", "game render", false},
        {L"GXWorker:", "game gfx workers", true},
        {L"Core.Res.", "game loaders", true},
        {L"shadPS4:GpuCommandProcessor", "gpu commands", false},
        {L"fast:StageB", "stageB", false},
        {L"fast:Recorder", "recorder", false},
        {L"shadPS4:PipeAsync", "pipeline compiles", true},
    }};
}
Groups g_groups = EmptyGroups();
std::future<Groups> g_scan;
s64 g_next_scan_ns = 0;

Groups Scan() {
    Common::SetCurrentThreadName("shadPS4:SlowThreadDiscovery");
    auto groups = EmptyGroups();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return groups;
    }
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (h == nullptr) {
            continue;
        }
        PWSTR desc = nullptr;
        bool kept = false;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
            for (auto& g : groups) {
                const size_t n = std::wcslen(g.name);
                if (g.prefix ? std::wcsncmp(desc, g.name, n) == 0 : std::wcscmp(desc, g.name) == 0) {
                    g.handles.emplace_back(h);
                    g.last.push_back(0);
                    kept = true;
                    break;
                }
            }
            LocalFree(desc);
        }
        if (!kept) {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return groups;
}

} // namespace

std::string ThreadTimes(bool report) {
    const s64 now = NowNs();
    if (g_scan.valid() && g_scan.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        g_groups = g_scan.get(); // old handles close here, outside the rendering locks
    }
    if (!g_scan.valid() && now >= g_next_scan_ns) {
        g_next_scan_ns = now + 60'000'000'000;
        g_scan = std::async(std::launch::async, Scan);
    }
    std::string out;
    for (auto& g : g_groups) {
        double ms = 0.0;
        bool any = false;
        for (size_t i = 0; i < g.handles.size(); ++i) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetThreadTimes(g.handles[i].get(), &created, &exited, &kernel, &user)) {
                continue;
            }
            const auto ticks = [](FILETIME value) {
                return (static_cast<u64>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
            };
            const u64 cpu = ticks(kernel) + ticks(user); // 100 ns units; no clock calibration sleep
            if (g.last[i] != 0 && cpu >= g.last[i]) {
                ms += static_cast<double>(cpu - g.last[i]) / 10'000.0;
                any = true;
            }
            g.last[i] = cpu;
        }
        if (report && any) {
            out += fmt::format(" | {} {:.1f}", g.label, ms);
        }
    }
    return out;
}

// ---- The stall sampler (VR_STALL_SAMPLER=1, logs-on builds) ---------------------------------
// About 1000 times a second the sampler thread suspends fast:StageB and the GPU command thread in
// turn, records the program counter and up to 13 callers, and resumes it. While a thread is
// suspended nothing here allocates, locks or logs (the suspended thread may hold any lock): the
// stack is unwound with RtlVirtualUnwind over the modules' own .pdata, looked up in tables read
// once at the start. Each long frame (RequestSamples, from FrameBoundary) gets its window of
// samples appended to stall_samples.txt in the working directory; analysis tools name the
// addresses with the build's .map.
namespace {

struct UnwindModule {
    u64 base{}, end{};
    const RUNTIME_FUNCTION* table{};
    u32 count{};
    std::array<char, 64> name{};
};
std::vector<UnwindModule> g_unwind_modules;

void LoadUnwindModules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W me{sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        const auto base = reinterpret_cast<u64>(me.modBaseAddr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            continue;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (dir.VirtualAddress == 0 || dir.Size < sizeof(RUNTIME_FUNCTION)) {
            continue;
        }
        UnwindModule m{base, base + me.modBaseSize,
                       reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress),
                       static_cast<u32>(dir.Size / sizeof(RUNTIME_FUNCTION))};
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, m.name.data(),
                            static_cast<int>(m.name.size()) - 1, nullptr, nullptr);
        g_unwind_modules.push_back(m);
    }
    CloseHandle(snap);
}

const RUNTIME_FUNCTION* FindFunction(u64 pc, u64& image_base) {
    for (const auto& m : g_unwind_modules) {
        if (pc < m.base || pc >= m.end) {
            continue;
        }
        image_base = m.base;
        const u32 rva = static_cast<u32>(pc - m.base);
        u32 lo = 0, hi = m.count;
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            const auto& f = m.table[mid];
            if (rva < f.BeginAddress) {
                hi = mid;
            } else if (rva >= f.EndAddress) {
                lo = mid + 1;
            } else {
                return &f;
            }
        }
        return nullptr;
    }
    return nullptr;
}

constexpr u32 MaxFrames = 48; // the GPU driver alone can be 20+ frames deep
struct Sample {
    s64 ns{};
    u8 thread{};
    u8 count{};
    std::array<u64, MaxFrames> pcs{};
};
constexpr u32 RingSamples = 8192; // ~4 s of both threads at 1 kHz (~3 MB)
std::array<Sample, RingSamples> g_ring{};
std::atomic<u64> g_written{0};

// One sample of `thread` (suspended by the caller); no allocation or lock. A bad stack ends the
// sample, never the process (UnwindGuarded).
void UnwindImpl(HANDLE thread, u8 index, Sample& out);
void Unwind(HANDLE thread, u8 index, Sample& out) {
    __try {
        UnwindImpl(thread, index, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // keep what was unwound so far
    }
}
void UnwindImpl(HANDLE thread, u8 index, Sample& out) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    out.count = 0;
    if (!GetThreadContext(thread, &ctx)) {
        return;
    }
    out.thread = index;
    for (u32 i = 0; i < MaxFrames; ++i) {
        if (ctx.Rip == 0) {
            break;
        }
        out.pcs[out.count++] = ctx.Rip;
        u64 image_base = 0;
        const RUNTIME_FUNCTION* f = FindFunction(ctx.Rip, image_base);
        if (f == nullptr) {
            if (i != 0 || ctx.Rsp == 0) {
                break; // not ours to unwind (guest code, a module without .pdata)
            }
            ctx.Rip = *reinterpret_cast<const u64*>(ctx.Rsp); // a leaf: the return address is on top
            ctx.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, const_cast<RUNTIME_FUNCTION*>(f),
                         &ctx, &handler_data, &establisher, nullptr);
    }
}

std::atomic<s64> g_request_start{0}, g_request_end{0};
std::atomic<u32> g_request_ms10{0};
std::atomic<u64> g_request_seq{0};

void SamplerThread() {
    SetCurrentThreadName("hang:Sampler");
    LoadUnwindModules();
    const wchar_t* names[2] = {L"fast:StageB", L"shadPS4:GpuCommandProcessor"};
    std::array<HANDLE, 2> handles{};
    const auto find = [&] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return;
        }
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) {
                continue;
            }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (h == nullptr) {
                continue;
            }
            PWSTR desc = nullptr;
            bool kept = false;
            if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
                for (u32 i = 0; i < 2; ++i) {
                    if (handles[i] == nullptr && std::wcscmp(desc, names[i]) == 0) {
                        handles[i] = h;
                        kept = true;
                    }
                }
                LocalFree(desc);
            }
            if (!kept) {
                CloseHandle(h);
            }
        }
        CloseHandle(snap);
    };
    std::FILE* file = std::fopen("stall_samples.txt", "w");
    if (file != nullptr) {
        for (const auto& m : g_unwind_modules) {
            std::fprintf(file, "MODULE %s %llx %llx\n", m.name.data(),
                         static_cast<unsigned long long>(m.base),
                         static_cast<unsigned long long>(m.end - m.base));
        }
    }
    u64 done_seq = 0;
    s64 next_find = 0;
    LOG_INFO(Debug, "Stall sampler: {} modules with unwind data; writing stall_samples.txt",
             g_unwind_modules.size());
    for (;;) {
        const s64 now = NowNs();
        if ((handles[0] == nullptr || handles[1] == nullptr) && now >= next_find) {
            next_find = now + 2'000'000'000;
            find();
        }
        for (u8 i = 0; i < 2; ++i) {
            if (handles[i] == nullptr) {
                continue;
            }
            Sample& s = g_ring[g_written.load(std::memory_order_relaxed) % RingSamples];
            if (SuspendThread(handles[i]) == static_cast<DWORD>(-1)) {
                continue;
            }
            Unwind(handles[i], i, s);
            ResumeThread(handles[i]);
            s.ns = NowNs();
            g_written.fetch_add(1, std::memory_order_release);
        }
        const u64 seq = g_request_seq.load(std::memory_order_acquire);
        if (seq != done_seq && file != nullptr) {
            done_seq = seq;
            const s64 start = g_request_start.load(std::memory_order_relaxed);
            const s64 end = g_request_end.load(std::memory_order_relaxed);
            std::fprintf(file, "FRAME %.1f\n", g_request_ms10.load() / 10.0);
            const u64 written = g_written.load(std::memory_order_acquire);
            const u64 first = written > RingSamples ? written - RingSamples : 0;
            for (u64 k = first; k < written; ++k) {
                const Sample& s = g_ring[k % RingSamples];
                if (s.ns < start || s.ns > end || s.count == 0) {
                    continue;
                }
                std::fprintf(file, "S %u", s.thread);
                for (u32 f = 0; f < s.count; ++f) {
                    std::fprintf(file, " %llx", static_cast<unsigned long long>(s.pcs[f]));
                }
                std::fputc('\n', file);
            }
            std::fflush(file);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(900));
    }
}

bool SamplerOn() {
    static const bool on = [] {
        const char* env = std::getenv("VR_STALL_SAMPLER");
        if (env == nullptr || env[0] != '1') {
            return false;
        }
        std::thread(SamplerThread).detach();
        return true;
    }();
    return on;
}

} // namespace

void RequestSamples(double ms) {
    if (!SamplerOn()) {
        return;
    }
    const s64 end = NowNs();
    g_request_start.store(end - static_cast<s64>(ms * 1e6) - 2'000'000, std::memory_order_relaxed);
    g_request_end.store(end, std::memory_order_relaxed);
    g_request_ms10.store(static_cast<u32>(ms * 10.0), std::memory_order_relaxed);
    g_request_seq.fetch_add(1, std::memory_order_release);
}

void StartSampler() {
    (void)SamplerOn();
}

} // namespace Common::Slow

#else

namespace Common::Slow {
std::string ThreadTimes(bool) {
    return {};
}
void RequestSamples(double) {}
void StartSampler() {}
} // namespace Common::Slow

#endif
