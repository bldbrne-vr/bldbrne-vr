// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// The riposte's burst (vr.ini riposte_style, 2026-10-25; the tester: "a red and black outline shader
// thing ... think of like the red/black of persona 5", "not heavy"): the eyes turn into black /
// blood red / white with black ink lines (host_shaders/vr_riposte.glsl), twice in a visceral (a
// parry riposte or a backstab: the game's throws do not tell them apart, and the user is fine
// with both), where the user put it after trying the first version ("the effect comes up while
// the camera zoom plays and stops right after the second camera shake. And then it shows up again
// when the enemy is thrown back ... with a small delay as it popped up too early originally"):
//  - the lunge: on the game's visceral camera clock (its effect time, read by the camera's effect
//    source detour, bloodborne_camera.cpp): in onto the first kick (the hand going in), held
//    until your hand starts pulling back out (or the rip), at most to a little before the
//    second kick (VisceralStage's late kick, 1.9 - 2.4 s), so the plain world shows before;
//  - the throw: RipDelayS after the rip (the damage detour, bloodborne_visceral_pull.cpp
//    RiposteOnDamage), past the rip's slow beat as the enemy is thrown back.
// Drawn by the passes that already finish the eyes after DLSS / FSR: the native eyes' feedback
// pass (post_process.frag) and the DIBR late warp's last pass (vr_eye_upscale.comp), both eyes
// cut at one brightness (renderer_vulkan/vr_riposte_meter.h). Header-only: the game's threads
// note the camera clock and the rip in atomics, the renderer reads the strength.
namespace VR::Riposte {

// The throw's burst, from RipDelayS after the rip: in over InS, full until HoldS, gone by OutS
// (seconds from its start).
// The user, after the headset tries: 0.24, 0.26, 0.46, 0.80, then 0.82 s ("fire right before the enemy is
// thrown back, now it feels like it ends too early - but I do not want to make it last longer").
// The user, 2026-10-28: 50 ms later in and 30 ms sooner over (was 0.82 s, gone by 1.69 s; now
// 0.87 s to 1.66 s), the hold and the fade a little shorter for it.
constexpr float RipDelayS = 0.87f;
constexpr float InS = 0.07f;
constexpr float HoldS = 0.41f;
constexpr float OutS = 0.79f; // the user: 0.84, 0.95, 80 ms shorter, then 0.79 (above)
// A rip within this long of the last one is the same visceral (a second damage of it, or the
// interactive pull's and the plain hook's both seeing it): no second burst.
constexpr std::int64_t SameNs = 2'000'000'000;

// The lunge's burst, on the visceral camera's effect time (s): in from CamInS to CamFullS, onto
// the first kick (the hand going in, ~0.7 s; at the zoom's start it "comes on too fast", the user
// after the headset try), then held until your pull or the rip cuts it (below), out from CamOutS
// to CamEndS at the longest (gone just before the second kick rises). A clock not noted for
// CamStaleNs (the effect ended, the visceral was cut short) fades out over CamFadeNs.
// The user, 2026-10-27: 150 ms later in, 100 ms earlier out (was 0.50 / 0.70 / 2.40 / 2.60).
// 2026-11-01: "last until we pull back OR until a little before the second camera shake" (was
// 2.30 / 2.50, past the second kick).
constexpr float CamInS = 0.65f;
constexpr float CamFullS = 0.85f;
constexpr float CamOutS = 1.65f;
constexpr float CamEndS = 1.85f;
constexpr std::int64_t CamStaleNs = 100'000'000;
constexpr std::int64_t CamFadeNs = 150'000'000;
// Your pull cuts the lunge's burst: once the hand has pulled back PullCutProgress of the pull
// (core/vr/visceral_pull.h progress), it goes out over PullFadeNs, so the plain world shows
// between it and the throw's burst (2026-10-28: "stop the first burst ... so that we see
// a bit of the real world, before the second burst plays"). A visceral without a pull is cut the
// same way at its rip, RipDelayS before the throw's burst. Half the pull (6 of 12 cm): at 15% (1.8
// cm, until 2026-11-01) the hand settling after the catch cut it 0.16 s after it, while it was
// still coming in (the tester: "the first burst went away immediately").
constexpr float PullCutProgress = 0.50f;
constexpr std::int64_t PullFadeNs = 100'000'000;
// The camera clock's start: a clock not noted for this long, or gone this far back, is a new
// visceral (as the camera hook's log).
constexpr std::int64_t CamNewNs = 500'000'000;
// The lunge's burst at most this strong (the look blended this far over the view); the throw's
// goes to 1 (the tester: "lower the intensity of the first burst a bit (and keep the second
// intensity high)"; 0.70 until 2026-10-28, then "a tiny bit" less: 0.63, then 0.66; 2026-11-01
// "increase the intensity ... for both first and second burst": 0.80, the look itself stronger
// too, vr_riposte.glsl).
constexpr float CamPeak = 0.80f;

inline float Smooth(float e0, float e1, float x) {
    const float u = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

inline std::atomic<std::int64_t>& LastRip() {
    static std::atomic<std::int64_t> ns{0};
    return ns;
}

struct CameraClock {
    std::atomic<float> elapsed{0.0f};
    std::atomic<std::int64_t> ns{0};
    std::atomic<std::int64_t> start_ns{0}; // this visceral's first noted time
};
static_assert(std::atomic<float>::is_always_lock_free &&
              std::atomic<std::int64_t>::is_always_lock_free);

inline CameraClock& Camera() {
    static CameraClock clock;
    return clock;
}

// The damage hook: a visceral ripped at `now_ns` (VR::Body::SteadyNowNs). True when that starts a
// burst.
inline bool Note(std::int64_t now_ns) {
    std::atomic<std::int64_t>& last = LastRip();
    std::int64_t was = last.load(std::memory_order_relaxed);
    if (was != 0 && now_ns - was < SameNs) {
        return false;
    }
    return last.compare_exchange_strong(was, now_ns, std::memory_order_relaxed);
}

// The camera clock going back this far (s) is a new visceral's.
constexpr float CamJumpS = 0.25f;

enum class CameraNote : std::uint8_t { Ignored, Same, New };

// The camera's effect source detour: the visceral camera curve at `elapsed` s of its effect time,
// at `now_ns` (VR::Body::SteadyNowNs). New: this visceral's clock starts here (no clock for
// CamNewNs, or it went back). Ignored: a curve far ahead of the one followed, more than the time
// since its last note: an older visceral's effect still running (it lasts its whole duration,
// 27 s) beside a new one. Both noted every frame, the clock jumped between them, every frame a
// "new visceral": the lunge's burst flickered and the pull never cut it (the 2026-11-02 headset
// log, two viscerals back to back). A hitch only moves the clock on by its own time.
inline CameraNote NoteCamera(float elapsed, std::int64_t now_ns) {
    CameraClock& clock = Camera();
    const std::int64_t last = clock.ns.load(std::memory_order_relaxed);
    const float was = clock.elapsed.load(std::memory_order_relaxed);
    const bool fresh = last != 0 && now_ns - last <= CamNewNs;
    if (fresh &&
        elapsed - was > static_cast<float>(std::max<std::int64_t>(now_ns - last, 0)) * 1e-9f +
                            CamJumpS) {
        return CameraNote::Ignored;
    }
    const bool is_new = !fresh || elapsed + CamJumpS < was;
    if (is_new) {
        clock.start_ns.store(now_ns, std::memory_order_relaxed);
    }
    clock.elapsed.store(elapsed, std::memory_order_relaxed);
    clock.ns.store(now_ns, std::memory_order_release);
    return is_new ? CameraNote::New : CameraNote::Same;
}

inline std::atomic<std::int64_t>& PullCut() {
    static std::atomic<std::int64_t> ns{0};
    return ns;
}

// The visceral pull (bloodborne_visceral_pull.cpp, every update of a throw): the hand's pull so
// far. The first time it passes PullCutProgress in this visceral (since the camera clock's start)
// the lunge's burst starts going out. True when that cut a burst still showing (this visceral's
// camera clock running, before CamEndS): the caller logs it.
inline bool NotePull(float progress, std::int64_t now_ns) {
    if (!(progress >= PullCutProgress)) {
        return false;
    }
    std::atomic<std::int64_t>& cut = PullCut();
    const CameraClock& clock = Camera();
    if (cut.load(std::memory_order_relaxed) > clock.start_ns.load(std::memory_order_relaxed)) {
        return false;
    }
    cut.store(now_ns, std::memory_order_relaxed);
    const std::int64_t ns = clock.ns.load(std::memory_order_relaxed);
    return ns != 0 && now_ns - ns <= CamNewNs &&
           clock.elapsed.load(std::memory_order_relaxed) < CamEndS;
}

// 0 .. 1 at `now_ns` for a rip at `rip_ns` (0: none): the throw's burst.
inline float Envelope(std::int64_t now_ns, std::int64_t rip_ns) {
    if (rip_ns == 0) {
        return 0.0f;
    }
    const float t = static_cast<float>(now_ns - rip_ns) * 1e-9f - RipDelayS;
    if (!(t >= 0.0f) || t >= OutS) {
        return 0.0f;
    }
    return t < InS ? Smooth(0.0f, InS, t) : t < HoldS ? 1.0f : 1.0f - Smooth(HoldS, OutS, t);
}

// 0 .. 1 at `elapsed` s of the visceral camera's effect time: the lunge's burst.
inline float CameraEnvelope(float elapsed) {
    if (!(elapsed >= CamInS) || elapsed >= CamEndS) {
        return 0.0f;
    }
    return elapsed < CamFullS  ? Smooth(CamInS, CamFullS, elapsed)
           : elapsed < CamOutS ? 1.0f
                               : 1.0f - Smooth(CamOutS, CamEndS, elapsed);
}

// The lunge's burst at `now_ns`: the last noted effect time, faded once it stops coming.
inline float CameraStrength(std::int64_t now_ns) {
    const CameraClock& clock = Camera();
    const std::int64_t ns = clock.ns.load(std::memory_order_acquire);
    if (ns == 0) {
        return 0.0f;
    }
    const std::int64_t age = std::max<std::int64_t>(now_ns - ns, 0);
    if (age >= CamStaleNs + CamFadeNs) {
        return 0.0f;
    }
    float k = CamPeak * CameraEnvelope(clock.elapsed.load(std::memory_order_relaxed));
    if (age > CamStaleNs) {
        k *= 1.0f - Smooth(0.0f, static_cast<float>(CamFadeNs),
                           static_cast<float>(age - CamStaleNs));
    }
    // Cut by your pull or, sooner, by the rip (a visceral without a pull), of this visceral.
    const std::int64_t start = clock.start_ns.load(std::memory_order_relaxed);
    std::int64_t cut = PullCut().load(std::memory_order_relaxed);
    const std::int64_t rip = LastRip().load(std::memory_order_relaxed);
    if (rip > start && (cut <= start || rip < cut)) {
        cut = rip;
    }
    if (cut > start && now_ns >= cut) {
        k *= 1.0f - Smooth(0.0f, static_cast<float>(PullFadeNs), static_cast<float>(now_ns - cut));
    }
    return k;
}

// The burst's strength at `now_ns` (VR::Body::SteadyNowNs). VR_RIPOSTE_TEST (tests), without a
// visceral: =<seconds> a throw's burst every that many seconds as well; =-<seconds> the look held
// on and off in turns of that many seconds from the first call (off first: screenshots and its
// cost).
inline float Strength(std::int64_t now_ns) {
    static const std::int64_t test_ns = [] {
        const char* v = std::getenv("VR_RIPOSTE_TEST");
        const float s = v != nullptr ? std::strtof(v, nullptr) : 0.0f;
        return std::abs(s) >= 1.0f ? static_cast<std::int64_t>(s * 1e9f) : std::int64_t{0};
    }();
    float k = std::max(CameraStrength(now_ns),
                       Envelope(now_ns, LastRip().load(std::memory_order_relaxed)));
    if (test_ns > 0 && now_ns > 0) {
        k = std::max(k, Envelope(now_ns, now_ns - now_ns % test_ns));
    } else if (test_ns < 0) {
        static const std::int64_t start = now_ns;
        if (((now_ns - start) / -test_ns) % 2 == 1) {
            k = 1.0f;
        }
    }
    return k;
}

} // namespace VR::Riposte
