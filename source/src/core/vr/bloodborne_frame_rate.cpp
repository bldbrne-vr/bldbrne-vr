// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-FileCopyrightText: bbport contributors (deadinside28/bloodborne_pc)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The game's frame rate set free by hooks in its code, ported from bbport
// (gpu/shim/bbport_frame_rate.cpp, the same 1.09 executable, every site and value as there).
//
// The game's main loop (0x2018d20) runs its frame pacer (0x2034770: it waits for the frame time
// its mode asks, 1/30 s, and measures the frame at [pacer + 0x264]) and then steps the game by a
// fixed 1/30 s (0x2018e36). The hooks keep the pacer's mode at one vblank with our frame time as
// its target, and step the game by the measured frame time (1/MaxRate to 1/30 s: below 30 FPS
// the game slows down, as it always did).
//
// What the game ties to a 30 FPS frame besides (bbport found them by the frame time each function
// is called with, and by what its code does with the constant): subsystems stepped by a fixed
// 1/30 s, per-frame steps and approaches, per-frame damping. Their instructions read the constant
// from a page next to the image instead (only the instruction's displacement changes), where
// FrameStep writes the value for this frame's time: a step of 1/30 s becomes the frame's time, a
// per-frame factor f becomes f^(frames) for the frame's length in 30 FPS frames. Durations the
// game converts from 30 FPS frames into seconds stay as they are: what counts them down uses the
// real time (the community patches shortened those 2-8 times).
//
// The community's 60 FPS++ and our limiter pin edit the same code, so memory_patcher leaves them
// out when the hooks are requested (FrameHooksRequested); every site here is checked byte for
// byte first, and without the three core hooks the game keeps its own 30 FPS.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <xbyak/xbyak.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "core/file_sys/file_override.h"
#include "core/vr/bloodborne_frame_rate.h"
#include "core/vr/cutscene_frame_limit.h"
#include "core/vr/game_area.h"
#include "core/vr/hook_util.h"
#include "core/vr/near_code.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {
namespace {

constexpr float Frame30 = 1.0f / 30.0f;
// bbport: above this the game breaks (on loading screens at 880-1000 FPS its main loop outran
// its worker threads and a pool allocator popped a freed node); at 480 every load went through.
constexpr int MaxRate = 480;
constexpr int MinRate = 20;

// What the redirected instructions read, rewritten by FrameStep for each frame (near the image:
// RIP-relative operands reach it).
struct alignas(64) Live {
    float dt = Frame30;              // the frame's time: the game steps by it
    std::atomic<float> shortest{1.0f / 60.0f}; // aligned, lock-free load in the guest pacer stub
    float pad[2] = {};
    float decay95[4] = {0.95f, 0.95f, 0.95f, 0.95f}; // 0.95 per 30 FPS frame (read as four)
    float approach30 = Frame30;      // a 1/30 approach per 30 FPS frame
    float approach10 = 0.1f;         // a 0.1 approach per 30 FPS frame
    float step_down = -0.1f;         // -0.1 per 30 FPS frame
    float step_up = 0.1f;            // +0.1 per 30 FPS frame
    float decay08 = 0.8f;            // 0.8 per 30 FPS frame
    float cloth = 1.0f;              // cloth constraints' stiffness
};
static_assert(std::atomic<float>::is_always_lock_free && sizeof(std::atomic<float>) == sizeof(float));
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
static_assert(offsetof(Live, shortest) == sizeof(float));

u8* g_base = nullptr;
u64 g_size = 0;
Live* g_live = nullptr;
std::atomic<bool> g_active{false};
std::atomic<int> g_rate{60};
// The presenter publishes a request without ever locking or reading guest memory.
// Guest callbacks apply it; the latest request survives a temporary cutscene cap.
std::atomic<int> g_requested_rate{60};
std::mutex g_rate_mutex;
CutsceneFrameLimit::Hold g_cutscene_hold; // protected by g_rate_mutex
std::atomic<bool> g_cutscene_held{false};
// The eboot when its event flags' layout matched (FlagLayoutMatches at FrameRateEbootLoaded, frame
// hooks or not), else 0: what StrangeMoonTick reads the strange moon's flag from.
std::atomic<VAddr> g_flag_image{0};
std::unique_ptr<Xbyak::CodeGenerator> g_code;
std::atomic<VAddr> g_trace_base{0}; // the eboot, for FrameTraceTick (hooks or not)
std::atomic<u64> g_frames{0};       // game frames stepped by FrameStep
std::atomic<double> g_game_time{0}; // their summed steps (s)
// The measured frame time smoothed (tau 0.5 s; 1/480 .. 1/15 s): VR::GameFrameSeconds with hooks.
std::atomic<float> g_step_seconds{1.0f / 60.0f};
// The cloth by its constraints' stiffness factor (HookClothStiffness, VR_CLOTH_FIX=0: not).
std::atomic<bool> g_cloth_hooked{false};
float g_cloth_exponent = -1.725f; // Havok's stiffness adaptation exponent (0x48d7bbc), read at load
std::atomic<u64> g_cloth_calls{0}, g_cloth_modes{0}, g_cloth_types{0};

s64 CutsceneNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<bool> EventFlagAt(u64 image, u32 flag) {
#ifdef _WIN32
    return CutsceneFrameLimit::ReadFlag(image, flag,
        [](u64 at, void* out, size_t size) {
            SIZE_T copied{};
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at),
                                     out, size, &copied) && copied == size;
        });
#else
    return std::nullopt; // no unsafe raw-memory fallback on unsupported hosts
#endif
}

std::optional<bool> CutsceneFlag() {
    return EventFlagAt(reinterpret_cast<u64>(g_base), CutsceneFrameLimit::PlayingFlag);
}

// The flag getter and the game's reference to its manager say the event flags are laid out as
// CutsceneFrameLimit::ReadFlag walks them. Nothing is patched at either site.
bool FlagLayoutMatches(const u8* base, u64 size) {
    constexpr u64 Getter = 0x13CFB60, ManagerRef = 0x1CD93AE;
    constexpr u8 GetterBytes[] = {0x44, 0x8b, 0x4f, 0x1c, 0x31, 0xd2, 0x89, 0xf0,
        0x41, 0xf7, 0xf1, 0x89, 0xc1, 0x41, 0x89, 0xc8, 0x45, 0x0f, 0xaf, 0xc1,
        0x4c, 0x8b, 0x57, 0x38};
    constexpr u8 RefBytes[] = {0x48, 0x8d, 0x05, 0x4b, 0x1d, 0x86, 0x03, 0x4c,
        0x8b, 0x00, 0x4d, 0x85, 0xc0, 0x75, 0x31};
    return base != nullptr && size >= CutsceneFrameLimit::FlagManagerRva + sizeof(u64) &&
           std::memcmp(base + Getter, GetterBytes, sizeof(GetterBytes)) == 0 &&
           std::memcmp(base + ManagerRef, RefBytes, sizeof(RefBytes)) == 0;
}

// g_rate_mutex held by all writers. The emitted x86 pacer loads this aligned
// 32-bit atomic; no code rewriting or changes to the OpenXR frame loop.
void ApplyRequestedRate() {
    const int rate = g_cutscene_hold.Rate(g_requested_rate.load(std::memory_order_relaxed));
    if (g_rate.exchange(rate, std::memory_order_relaxed) != rate && g_live) {
        g_live->shortest.store(1.0f / static_cast<float>(rate), std::memory_order_relaxed);
        LOG_INFO(Core, "Frame rate: at most {} fps (live)", rate);
    }
}

void PublishCutsceneHold(u32 previous, const char* reason) {
    g_cutscene_held.store(g_cutscene_hold.scene != 0, std::memory_order_release);
    ApplyRequestedRate();
    if (previous != g_cutscene_hold.scene) {
        if (g_cutscene_hold.scene) {
            LOG_INFO(Core, "Cutscene FPS: scene {} limited to {} fps; headset timing unchanged",
                     g_cutscene_hold.scene, g_rate.load(std::memory_order_relaxed));
        } else {
            LOG_INFO(Core, "Cutscene FPS: scene {} released ({}); restored requested {} fps",
                     previous, reason, g_rate.load(std::memory_order_relaxed));
        }
    }
}

void CutsceneFileOpened(std::string_view path) {
    const u32 id = CutsceneFrameLimit::SceneOf(path);
    if (!id) return; // unrelated preloads must not release a playing scene's cap
    std::lock_guard lock(g_rate_mutex);
    const u32 previous = g_cutscene_hold.scene;
    // Both vanilla events set 9180 before requesting the scene. A preload with
    // the flag off does not cap combat.
    const auto playing = CutsceneFlag();
    if (!playing) {
        LOG_WARNING(Core, "Cutscene FPS: scene {} requested but event state unavailable; "
                          "leaving requested FPS unchanged", id);
    }
    g_cutscene_hold.Open(id, playing.value_or(false), CutsceneNowNs());
    PublishCutsceneHold(previous, "inactive or unavailable event");
}

void CutsceneFrameTick() {
    if (!g_cutscene_held.load(std::memory_order_acquire) &&
        g_rate.load(std::memory_order_relaxed) ==
            g_requested_rate.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_rate_mutex);
    if (!g_cutscene_hold.scene) {
        ApplyRequestedRate();
        return;
    }
    const u32 previous = g_cutscene_hold.scene;
    const auto playing = CutsceneFlag();
    const s64 now = CutsceneNowNs();
    g_cutscene_hold.Tick(playing, now);
    PublishCutsceneHold(previous, !playing ? "event state unavailable" :
        !*playing ? "event ended/skipped" : "safety timeout");
}

void InstallCutsceneFrameLimit() {
#ifdef _WIN32
    // The layout is validated before we ever traverse it (FlagLayoutMatches).
    if (!FlagLayoutMatches(g_base, g_size)) {
        LOG_WARNING(Core, "Cutscene FPS: event flag signatures differ; fallback not installed");
        return;
    }
    Core::FileSys::SetCutsceneOpenObserver(&CutsceneFileOpened);
    LOG_INFO(Core, "Cutscene FPS: targeted 30 fps fallback ready (Rom 32000000, Ludwig 34000030)");
#endif
}

// The pacer at the main loop's step (rdi: it) and the step's frame (rsi: rbp): this frame's
// values, and the step itself into [rbp - 0x38].
void PS4_SYSV_ABI FrameStep(const u8* pacer, u8* frame) {
    CutsceneFrameTick();
    float measured{};
    std::memcpy(&measured, pacer + 0x264, sizeof(measured));
    const float dt =
        std::clamp(std::isfinite(measured) ? measured : Frame30, 1.0f / MaxRate, Frame30);
    const float frames = dt * 30.0f; // the frame's length in 30 FPS frames
    Live& live = *g_live;
    live.dt = dt;
    const float decay95 = std::pow(0.95f, frames);
    for (float& d : live.decay95) {
        d = decay95;
    }
    live.approach30 = 1.0f - std::pow(1.0f - Frame30, frames);
    live.approach10 = 1.0f - std::pow(0.9f, frames);
    live.step_down = -0.1f * frames;
    live.step_up = 0.1f * frames;
    live.decay08 = std::pow(0.8f, frames);
    // The cloth setups' range checks of Stiffness, Rigid Factor and Allowed Compression (bbport
    // 0.7: 0x80d800 only warns, so the community's 0.5 there changed nothing). With the stiffness
    // hook the cloth follows the frame rate by its constraints' factor (ClothStiffnessFactor) and
    // these keep the game's 1.0; without it, the old 0.5 from 60 FPS up.
    live.cloth = g_cloth_hooked.load(std::memory_order_relaxed) ? 1.0f
                                                                : std::clamp(frames, 0.5f, 1.0f);
    std::memcpy(frame - 0x38, &dt, sizeof(dt));
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (std::isfinite(measured) && measured > 0.0f) {
        const float m = std::clamp(measured, 1.0f / MaxRate, 1.0f / 15.0f);
        const float previous = g_step_seconds.load(std::memory_order_relaxed);
        g_step_seconds.store(previous + (m - previous) * (1.0f - std::exp(-m / 0.5f)),
                             std::memory_order_relaxed);
    }
    g_game_time.store(g_game_time.load(std::memory_order_relaxed) + dt, std::memory_order_relaxed);
}

// Havok's stiffness factor of a constraint set for one substep (0x1167a80, replaced; bbport 0.7's
// ClothStiffnessFactor). `mode` 2 adapts the stiffness to how often the set is solved,
// (substeps x a x b)^exponent (a, b the instance's scales), for a set of type 8 the iteration's
// share instead and for types 5 and 10 only the last substep. The substeps are per frame: at 90 FPS
// the cloth is solved 3 times as often as at 30 FPS with the same factor, and is that much stiffer
// (it flutters). Here the game's factor times the frame's length in 30 FPS frames: a constraint
// with stiffness k pulls ~k x frames a frame, as far per second as at 30 FPS. At 30 FPS: the game's.
float PS4_SYSV_ABI ClothStiffnessFactor(const u8* set, int mode, int iteration, int substeps,
                                        float a, float b) {
    u32 type{};
    std::memcpy(&type, set + 0x18, sizeof(type));
    const u64 n = g_cloth_calls.fetch_add(1, std::memory_order_relaxed);
    g_cloth_modes.fetch_or(u64{1} << (static_cast<u32>(mode) & 63), std::memory_order_relaxed);
    g_cloth_types.fetch_or(u64{1} << (type & 63), std::memory_order_relaxed);
    const float frames = g_live->dt * 30.0f;
    if (n == 20000) {
        LOG_INFO(Core, "Frame rate: cloth stiffness factor: modes {:#x}, constraint types {:#x}, "
                       "{:.0f} fps: x{:.3f}",
                 g_cloth_modes.load(), g_cloth_types.load(), 30.0f / frames, frames);
    }
    const bool adapts = mode == 2 && !(substeps == 1 && a == 1.0f && b == 1.0f);
    if (adapts && type == 8) {
        return static_cast<float>(iteration + 1) / static_cast<float>(substeps);
    }
    if (adapts && (type == 5 || type == 10)) {
        return substeps - 1 == iteration ? 1.0f : 0.0f;
    }
    const float game =
        adapts ? std::pow(static_cast<float>(substeps) * a * b, g_cloth_exponent) : 1.0f;
    if (type == 5 || type == 8 || type == 10) {
        return game;
    }
    return game * frames;
}

// ---- Redirected constants: instruction (its RIP-relative operand), the constant, the value ----
struct Redirect {
    u64 site;
    u64 constant;
    std::size_t value; // offsetof(Live, ..)
};
#define LIVE(field) offsetof(Live, field)
constexpr Redirect Redirects[] = {
    // A fixed 1/30 s step or "one frame" (the patches' lists, checked one by one by bbport):
    {0xab9f0d, 0x47efdcc, LIVE(dt)},  // a time step, one frame when unset
    {0xdca3ed, 0x4888688, LIVE(dt)},  // a per-frame budget
    {0xee596a, 0x48ad65c, LIVE(dt)},  // effects: a fade of at least one frame (three releases)
    {0xee5a8a, 0x48ad660, LIVE(dt)},
    {0xee5baa, 0x48ad664, LIVE(dt)},
    {0xef06f1, 0x48adc08, LIVE(dt)},  // at least one frame
    {0xef6992, 0x48add28, LIVE(dt)},
    {0x1044943, 0x48cc3f4, LIVE(dt)}, // an update by one frame
    {0x12a4416, 0x492577c, LIVE(dt)}, // a timer advanced by one frame each frame
    {0x15dc1f7, 0x4926c9c, LIVE(dt)},
    {0x173456e, 0x49270e4, LIVE(dt)}, // an update by one frame
    {0x17f9d26, 0x4927554, LIVE(dt)},
    {0x180f647, 0x49275f4, LIVE(dt)}, // a timer advanced by one frame each frame
    {0x18bc4f7, 0x4927c4c, LIVE(dt)}, // an update by one frame (twice)
    {0x18bc50d, 0x4927c4c, LIVE(dt)},
    {0x1dbc308, 0x4928e98, LIVE(dt)}, // frames counted at one frame's time
    {0x1f77d4a, 0x4929170, LIVE(dt)}, // an update by one frame (0 after a reset)
    {0x227784b, 0x4929b80, LIVE(dt)}, // timers advanced by one frame (0xebbda0)
    {0x2277892, 0x4929b80, LIVE(dt)},
    {0x2313dad, 0x4929e70, LIVE(dt)}, // a simulation step: 1/60 in the game's 60 Hz mode,
    {0x2313db7, 0x4929e74, LIVE(dt)}, // 1/30 otherwise
    {0x2315d71, 0x4929e74, LIVE(dt)},
    // Body parts the physics moves (0x1515ec0, each part every frame): their timers advanced by
    // the step they are given (0x180df90 at +0xf0, 0x1811650 at +0xd0), 1/30 s; not in the
    // patches.
    {0x1515fab, 0x4926364, LIVE(dt)},
    {0x1516030, 0x4926364, LIVE(dt)},
    // Steps and approaches per frame:
    {0x19084f5, 0x4927fd4, LIVE(step_down)}, // a value ramped by 0.1 a frame
    {0x1908522, 0x4927fd4, LIVE(step_down)},
    {0x1908518, 0x4927fe0, LIVE(step_up)},
    {0x190854c, 0x4927fe0, LIVE(step_up)},
    {0x143e2b2, 0x4925e94, LIVE(approach30)}, // the follow camera's approaches
    {0x143af8c, 0x4925e68, LIVE(approach10)},
    // Damping per frame:
    {0x1a4abca, 0x490b5b0, LIVE(decay95)}, // a character's motion (0x1a4a730, x0.95 a frame)
    {0x1514c4d, 0x4926334, LIVE(decay08)}, // the run's wall check (x0.8 a frame while blocked)
    // Cloth constraints' stiffness:
    {0x47c6dc, 0x47693f0, LIVE(cloth)},
    {0x7e45be, 0x47b63e4, LIVE(cloth)},
    {0x7e4642, 0x47b63e4, LIVE(cloth)},
    {0x7e6239, 0x47b63e4, LIVE(cloth)},
    {0x7f706d, 0x47b8474, LIVE(cloth)},
    {0x7f70ad, 0x47b8474, LIVE(cloth)},
    {0x7f79e5, 0x47b8474, LIVE(cloth)},
    {0x7f7be8, 0x47b8474, LIVE(cloth)},
    {0x7f7dbd, 0x47b8474, LIVE(cloth)},
    {0x805898, 0x47b91c0, LIVE(cloth)},
    {0x805d18, 0x47b91c0, LIVE(cloth)},
    {0x806269, 0x47b91c0, LIVE(cloth)},
    {0x8062f0, 0x47b91c0, LIVE(cloth)},
    {0x809d62, 0x47b98b8, LIVE(cloth)},
    {0x80a4c9, 0x47b98b8, LIVE(cloth)},
    {0x80a876, 0x47b98b8, LIVE(cloth)},
};
#undef LIVE

// The instruction at `code` reads `r.constant` RIP-relative (a VEX instruction, no immediate
// after the operand): it reads the live value instead.
bool RedirectAt(u8* const code, const Redirect& r) {
    std::size_t disp_at = 0, length = 0;
    if (code[0] == 0xc5 && (code[3] & 0xc7) == 0x05) {
        disp_at = 4, length = 8;
    } else if (code[0] == 0xc4 && (code[4] & 0xc7) == 0x05) {
        disp_at = 5, length = 9;
    } else {
        return false;
    }
    s32 disp{};
    std::memcpy(&disp, code + disp_at, sizeof(disp));
    const u64 next = reinterpret_cast<u64>(code) + length;
    if (next + static_cast<s64>(disp) != reinterpret_cast<u64>(g_base + r.constant)) {
        return false;
    }
    s32 to{};
    if (!Hook::Rel32(next, reinterpret_cast<u64>(g_live) + r.value, to) ||
        !Hook::MakeWritable(code + disp_at, sizeof(to))) {
        return false;
    }
    std::memcpy(code + disp_at, &to, sizeof(to));
    Hook::FlushCode(code + disp_at, sizeof(to));
    return true;
}

// `r.site` in place, or (Windows) where the loader's red-zone patching moved it: a jump to its
// trampoline (module.cpp, after the image), which runs the same instruction re-encoded for its new
// address. Found there by its operand still reading `r.constant` (0xdca3ed in 1.09: the moved
// instruction is within the trampoline's first bytes); any other code is left as it is.
bool RedirectOperand(const Redirect& r) {
    u8* const code = g_base + r.site;
    if (RedirectAt(code, r)) {
        return true;
    }
    if (code[0] != 0xe9) {
        return false;
    }
    s32 rel{};
    std::memcpy(&rel, code + 1, sizeof(rel));
    u8* const trampoline = code + 5 + rel;
    constexpr std::size_t Scan = 64;
    for (std::size_t i = 0; i + 9 <= Scan; ++i) {
        if (RedirectAt(trampoline + i, r)) {
            LOG_INFO(Core, "Frame rate: {:#x} was moved by the loader; its copy at +{} of the "
                           "trampoline follows the frame's time",
                     r.site, i);
            return true;
        }
    }
    return false;
}

// ---- Detours: the instructions at a site replaced by a rel32 jump to a stub near the image ----
// A stub's bytes, built in place (bbport's BbGuestCode::Emitter).
struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    bool overflow = false;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            if (size < code.size()) {
                code[size] = b;
            }
            ++size;
        }
        overflow = overflow || size > code.size();
    }
    void U32(u32 v) {
        u8 b[4];
        std::memcpy(b, &v, sizeof(b));
        Bytes({b[0], b[1], b[2], b[3]});
    }
    void U64(u64 v) {
        U32(static_cast<u32>(v));
        U32(static_cast<u32>(v >> 32));
    }
    // jmp qword [rip+0]: to `target` anywhere.
    void JmpAbs(u64 target) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(target);
    }
    // The guest's red zone kept (the stubs push below rsp).
    void SkipRedZone() {
        Bytes({0x48, 0x8d, 0x64, 0x24, 0x80}); // lea rsp, [rsp - 128]
    }
    void RestoreRedZone() {
        Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
    }
    // The 32-bit value at `address` into the stack frame's [rbp + offset] (rax kept).
    void StoreToFrame(const void* address, s8 offset) {
        SkipRedZone();
        Bytes({0x50, 0x48, 0xb8}); // push rax; movabs rax, address
        U64(reinterpret_cast<u64>(address));
        Bytes({0x8b, 0x00});                                  // mov eax, [rax]
        Bytes({0x89, 0x45, static_cast<u8>(offset), 0x58});   // mov [rbp + offset], eax; pop rax
        RestoreRedZone();
    }
};

// `e` into near memory; a jump to it at `site` over `length` bytes (the rest int3) if the site
// holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* const code = g_base + site;
    if (length < 5 || std::memcmp(code, expected, length) != 0 || e.overflow) {
        return false;
    }
    u8* const stub = Hook::NearAlloc(reinterpret_cast<u64>(g_base), g_size, e.size, 16);
    s32 rel{};
    if (stub == nullptr || !Hook::Rel32(reinterpret_cast<u64>(code) + 5,
                                        reinterpret_cast<u64>(stub), rel) ||
        !Hook::MakeWritable(code, length)) {
        return false;
    }
    std::memcpy(stub, e.code.data(), e.size);
    Hook::FlushCode(stub, e.size);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel, sizeof(rel));
    for (std::size_t i = 5; i < length; ++i) {
        code[i] = 0xcc;
    }
    Hook::FlushCode(code, length);
    return true;
}

// The pacer's mode, after the game chose it (0x203493d, `lea r15, [r12 + 0x268]`; r12 the
// pacer): one vblank (+0x10), no flip skipping (+0x14), our frame time as the frame time target
// (+0x18), no vblank counts to wait for (+0x268, +0x26c).
bool HookPacerMode() {
    constexpr u8 code[] = {0x4d, 0x8d, 0xbc, 0x24, 0x68, 0x02, 0x00, 0x00};
    Emitter e;
    e.Bytes({0x41, 0xc7, 0x44, 0x24, 0x10, 0x01, 0x00, 0x00, 0x00}); // mov dword [r12 + 0x10], 1
    e.Bytes({0x41, 0xc6, 0x44, 0x24, 0x14, 0x00});                   // mov byte [r12 + 0x14], 0
    e.SkipRedZone();
    e.Bytes({0x50, 0x48, 0xb8});                       // push rax; movabs rax,
    e.U64(reinterpret_cast<u64>(&g_live->shortest));   //   &shortest
    e.Bytes({0x8b, 0x00});                             // mov eax, [rax]
    e.Bytes({0x41, 0x89, 0x44, 0x24, 0x18, 0x58});     // mov [r12 + 0x18], eax; pop rax
    e.RestoreRedZone();
    e.Bytes({0x49, 0xc7, 0x84, 0x24, 0x68, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00}); // mov qword [r12 + 0x268], 0
    for (const u8 b : code) {
        e.Bytes({b});
    }
    e.JmpAbs(reinterpret_cast<u64>(g_base) + 0x203493d + sizeof(code));
    return Detour(0x203493d, code, sizeof(code), e);
}

// The pacer keeps its one vblank when a frame came late (0x2034d96: `test dl, dl; je; mov eax,
// [r12 + 0x10]`, then eax is stored back: 0 when late).
bool HookPacerLate() {
    constexpr u8 code[] = {0x84, 0xd2, 0x74, 0x05, 0x41, 0x8b, 0x44, 0x24, 0x10};
    Emitter e;
    e.Bytes({0x84, 0xd2});                   // test dl, dl
    e.Bytes({0x41, 0x8b, 0x44, 0x24, 0x10}); // mov eax, [r12 + 0x10]
    e.JmpAbs(reinterpret_cast<u64>(g_base) + 0x2034d9f);
    return Detour(0x2034d96, code, sizeof(code), e);
}

// The main loop's step (0x2018e36: `mov dword [rbp - 0x38], 1/30`, its time; rbx the pacer):
// FrameStep writes this frame's values and the step. The host call goes through our usual stub
// (every volatile register and ymm kept); the site's rel32 jump lands on a near jump to it.
bool HookStep() {
    constexpr u64 site = 0x2018e36;
    constexpr u8 code[] = {0xc7, 0x45, 0xc8, 0x89, 0x88, 0x08, 0x3d};
    if (std::memcmp(g_base + site, code, sizeof(code)) != 0) {
        return false;
    }
    using namespace Xbyak::util;
    void* const host = Hook::EmitHostCallStubWith(
        *g_code, reinterpret_cast<void*>(&FrameStep), nullptr, 0,
        reinterpret_cast<u64>(g_base) + site + sizeof(code), [](Xbyak::CodeGenerator& c) {
            c.mov(rdi, rbx);
            c.mov(rsi, rbp);
        });
    Emitter e;
    e.JmpAbs(reinterpret_cast<u64>(host));
    return Detour(site, code, sizeof(code), e);
}

// A subsystem stepped by a fixed 1/30 s (`mov dword [rbp + offset], 1/30` at `site`): by the
// frame's time.
bool HookFixedStep(u64 site, s8 offset) {
    const u8 code[] = {0xc7, 0x45, static_cast<u8>(offset), 0x89, 0x88, 0x08, 0x3d};
    Emitter e;
    e.StoreToFrame(&g_live->dt, offset);
    e.JmpAbs(reinterpret_cast<u64>(g_base) + site + sizeof(code));
    return Detour(site, code, sizeof(code), e);
}

// The run's wall check (0x1514c24: the distance moved this frame x 30 = a speed at 30 FPS): the
// distance divided by the frame's time ([rbp - 0x74], the character update's), a speed at any
// frame rate. Above ~90 FPS the per-frame distance fell under the check's threshold and the run
// dropped to half speed. 1: done by the hook, 2: a Sprint Fix patch already did it, 0: other code.
int HookRunSpeed() {
    constexpr u64 site = 0x1514c24;
    const u8* const code = g_base + site;
    constexpr u8 patched[] = {0xc5, 0xfa, 0x5e, 0x45, 0x8c, 0x0f, 0x1f, 0x00}; // vdivss; nop
    if (std::memcmp(code, patched, sizeof(patched)) == 0) {
        return 2;
    }
    s32 disp{};
    std::memcpy(&disp, code + 4, sizeof(disp));
    if (code[0] != 0xc5 || code[1] != 0xfa || code[2] != 0x59 || code[3] != 0x05 ||
        reinterpret_cast<u64>(code) + 8 + static_cast<s64>(disp) !=
            reinterpret_cast<u64>(g_base + 0x492632c)) {
        return 0;
    }
    u8 expected[8];
    std::memcpy(expected, code, sizeof(expected));
    Emitter e;
    e.Bytes({0xc5, 0xfa, 0x5e, 0x45, 0x8c}); // vdivss xmm0, xmm0, [rbp - 0x74]
    e.JmpAbs(reinterpret_cast<u64>(g_base) + site + 8);
    return Detour(site, expected, sizeof(expected), e) ? 1 : 0;
}

// Havok's stiffness factor (0x1167a80: `push rbp; mov rbp, rsp; push r15`) by
// ClothStiffnessFactor, its arguments as they are (a tail call). VR_CLOTH_FIX=0: left alone.
bool HookClothStiffness() {
    if (const char* env = std::getenv("VR_CLOTH_FIX"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 site = 0x1167a80;
    constexpr u8 code[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    float exponent{};
    std::memcpy(&exponent, g_base + 0x48d7bbc, sizeof(exponent));
    if (!(exponent < 0.0f && exponent > -4.0f)) {
        return false;
    }
    g_cloth_exponent = exponent;
    Emitter e;
    e.JmpAbs(reinterpret_cast<u64>(&ClothStiffnessFactor));
    return Detour(site, code, sizeof(code), e);
}

// VR_FRAME_TRACE=<ms> (tests: the same walk at every frame rate, the elevator): the main player's
// physics position every <ms> of real time, read on the caller's thread.
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;
constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetPhysicsModule = 0x68;
constexpr u32 OffsetModuleOwner = 0x08;
constexpr u32 OffsetPhysicsPosition = 0x1E0;

bool PlayerPosition(const u8* base, float (&out)[3]) {
    const u8* const world = Hook::Read<const u8*>(base + WorldChrManRva);
    const u8* const player = Hook::Plausible(world) ? Hook::Read<const u8*>(world + OffsetMainPlayer)
                                                    : nullptr;
    const u8* const mods =
        Hook::Plausible(player) ? Hook::Read<const u8*>(player + OffsetChrModules) : nullptr;
    const u8* const physics =
        Hook::Plausible(mods) ? Hook::Read<const u8*>(mods + OffsetPhysicsModule) : nullptr;
    if (!Hook::Plausible(physics) || Hook::Read<const u8*>(physics + OffsetModuleOwner) != player) {
        return false;
    }
    std::memcpy(out, physics + OffsetPhysicsPosition, sizeof(out));
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

} // Anonymous namespace

void FrameTraceTick() {
    static const int every_ms = [] {
        const char* env = std::getenv("VR_FRAME_TRACE");
        return env && *env ? std::max(std::atoi(env), 1) : 0;
    }();
    const VAddr base = g_trace_base.load(std::memory_order_relaxed);
    if (every_ms == 0 || base == 0) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{};
    const auto now = Clock::now();
    if (now - last < std::chrono::milliseconds(every_ms)) {
        return;
    }
    last = now;
    float pos[3]{};
    if (!PlayerPosition(reinterpret_cast<const u8*>(base), pos)) {
        return;
    }
    LOG_INFO(Core, "Frame trace: pos {:.4f} {:.4f} {:.4f} | game frames {} game time {:.3f} s | {}",
             pos[0], pos[1], pos[2], g_frames.load(std::memory_order_relaxed),
             g_game_time.load(std::memory_order_relaxed),
             FrameHooksActive() ? "hooks" : "patches");
}

bool FrameHooksRequested() {
    if (const char* env = std::getenv("VR_FRAME_HOOKS"); env && (env[0] == '0' || env[0] == '1')) {
        return env[0] == '1';
    }
    return VR::GetSettings().frame_hooks;
}

// The strange moon's flag for the looks that follow it (game_area.h BloodMoon): read only in the
// maps whose sky it can change, at once on coming into one and every StrangeMoonEvery-th call
// after (GameArea::StrangeMoonPoll), and only once the flags' layout has matched; not known counts
// as not up.
// Called by the body's render tick (bloodborne_body.cpp, the game's loop thread) right after the
// map he stands in is published (both go into GameArea::BloodMoon), so it runs with frame_hooks=0
// too (until 2026-11-17 it sat in the frame hooks' own step: without them the flag was never read
// and the air kept the probe's colour under the blood moon; a review's point).
void StrangeMoonTick() {
    static GameArea::StrangeMoonPoll poll; // (one thread: the game's loop)
    if (!poll.Due(GameArea::Slot().load(std::memory_order_relaxed))) {
        return;
    }
    const VAddr image = g_flag_image.load(std::memory_order_relaxed);
    if (image == 0) {
        return;
    }
    GameArea::PublishStrangeMoon(EventFlagAt(image, GameArea::StrangeMoonFlag).value_or(false));
}

void FrameRateEbootLoaded(VAddr base, u64 size) {
    g_trace_base = base;
    // (The flags' layout, for StrangeMoonTick: whatever the frame hooks' own state.)
    if (g_flag_image.load(std::memory_order_relaxed) == 0 &&
        FlagLayoutMatches(reinterpret_cast<const u8*>(base), size)) {
        g_flag_image.store(base, std::memory_order_relaxed);
    }
    if (!FrameHooksRequested() || g_base != nullptr) {
        return;
    }
    if (size <= 0x492a000) {
        LOG_WARNING(Core, "Frame rate: the eboot is not the expected 1.09 image: 30 FPS");
        return;
    }
    g_base = reinterpret_cast<u8*>(base);
    g_size = size;
    u8* const page = Hook::NearAlloc(base, size, sizeof(Live), 64);
    if (page == nullptr) {
        LOG_ERROR(Core, "Frame rate: no memory next to the game's code: 30 FPS");
        return;
    }
    g_live = new (page) Live{};
    g_rate.store(g_requested_rate.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_live->shortest.store(1.0f / static_cast<float>(std::clamp(g_rate.load(), MinRate, MaxRate)),
                           std::memory_order_relaxed);
    g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
    // The core first: without all three the game keeps its own frame rate.
    bool core = false;
    try {
        core = HookPacerMode() && HookPacerLate() && HookStep();
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "Frame rate: the step's stub was not emitted: {}", e.what());
    }
    if (!core) {
        LOG_ERROR(Core, "Frame rate: the game's frame pacer is not the expected code (pacer {}, "
                        "step {}): 30 FPS",
                  Hook::HexBytes(g_base + 0x203493d, 8), Hook::HexBytes(g_base + 0x2018e36, 7));
        return;
    }
    g_active = true;
    InstallCutsceneFrameLimit();
    const int steps = HookFixedStep(0x17f9b71, -0x48) + HookFixedStep(0x1572a7d, -0x30);
    const int run = HookRunSpeed();
    const bool cloth = HookClothStiffness();
    g_cloth_hooked = cloth;
    int redirected = 0;
    for (const Redirect& r : Redirects) {
        if (RedirectOperand(r)) {
            ++redirected;
        } else {
            LOG_WARNING(Core, "Frame rate: {:#x} is not the expected code, left as it is ({})",
                        r.site, Hook::HexBytes(g_base + r.site, 9));
        }
    }
    LOG_BENCHMARK_INFO(
        Core,
        "Frame rate: hooks (no frame rate patches): the game steps by each frame's time "
        "(1/{}..1/30 s), at most {} fps; {} of 2 fixed steps, {} of {} per-frame values follow the "
        "frame's time, the run's wall check {}, cloth stiffness {}",
        MaxRate, g_rate.load(), steps, redirected, std::size(Redirects),
        run == 1 ? "by speed" : run == 2 ? "by speed (a Sprint Fix patch did it already)"
                                         : "unchanged (not found)",
        cloth ? "by the frame's time" : "the game's (VR_CLOTH_FIX=0 or not found)");
}

bool FrameHooksActive() {
    return g_active.load(std::memory_order_relaxed);
}

void SetGameFrameRate(int fps) {
    const int rate = std::clamp(fps, MinRate, MaxRate);
    if (g_requested_rate.load(std::memory_order_relaxed) == rate) return;
    g_requested_rate.store(rate, std::memory_order_relaxed);
    // Consumed by the game's next FrameStep. Never block the OpenXR thread
    // behind cutscene file opens, fault-safe reads, or transition logging.
}

float GameStepSeconds() {
    return g_step_seconds.load(std::memory_order_relaxed);
}

int GameFrameRate() {
    return g_rate.load(std::memory_order_relaxed);
}

} // namespace VR::Bloodborne
