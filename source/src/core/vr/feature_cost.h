// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

// VR_FEATURE_COST=1 (test tooling): the CPU time of the VR features' per-frame work, per site
// (calls, total and longest), logged by the presenter every 5 s as `VR feature cost:` (TakeText).
// Unset: one test of a cached flag per site, nothing timed. VR_HIT_FLASH_TEST=<deg>|plain forces
// the hit flash on (full strength, toward <deg>: 0 ahead, + right; or the plain ring), to time its
// GPU cost in the overlay's `gpu` figure without a blow.
namespace VR::FeatureCost {

enum Slot : std::uint32_t {
    TwoHandUpdate,  // presenter input: UpdateTwoHand (two_hand_grip)
    TwoHandApply,   // presenter frame: ApplyTwoHand
    TwoHandObserve, // pose copy (game side): ObserveTwoHand, the game's own two-handed grip
    ThrustUpdate,   // presenter input: UpdateVisceralThrust (visceral_thrust)
    VisceralOpen,   // game update: VisceralOpenTick, the last target's SpEffect list
    HitVignette,    // presenter frame: UpdateHitVignette + the flash's wedge
    HitHaptics,     // presenter frame: UpdateHitHaptics + the rumble's weighting
    QuickUpdate,    // presenter input: the mouth point + the quick items' whole update
    Count
};

constexpr std::array<const char*, Count> Names = {
    "twohand.update", "twohand.apply", "twohand.observe", "thrust.update",
    "visceral.open",  "hit.vignette",  "hit.haptics",     "quick.update(mouth)"};

inline bool Enabled() {
    static const bool on = std::getenv("VR_FEATURE_COST") != nullptr;
    return on;
}

struct Accumulator {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> ns{0};
    std::atomic<std::uint64_t> max_ns{0};
};

inline std::array<Accumulator, Count>& Accumulators() {
    static std::array<Accumulator, Count> a;
    return a;
}

class Scope {
public:
    explicit Scope(Slot slot) : slot_(slot), on_(Enabled()) {
        if (on_) {
            t0_ = std::chrono::steady_clock::now();
        }
    }
    ~Scope() {
        if (!on_) {
            return;
        }
        const std::uint64_t ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0_)
                .count());
        Accumulator& a = Accumulators()[slot_];
        a.calls.fetch_add(1, std::memory_order_relaxed);
        a.ns.fetch_add(ns, std::memory_order_relaxed);
        std::uint64_t m = a.max_ns.load(std::memory_order_relaxed);
        while (ns > m && !a.max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Slot slot_;
    bool on_;
    std::chrono::steady_clock::time_point t0_{};
};

// Since the last call (`seconds` long): per site calls/s, the average and longest call (us) and
// the time per 60 Hz game frame (us); the sum of all sites last.
inline std::string TakeText(double seconds) {
    std::string text;
    double total_us_per_s = 0.0;
    for (std::uint32_t i = 0; i < Count; ++i) {
        Accumulator& a = Accumulators()[i];
        const std::uint64_t calls = a.calls.exchange(0, std::memory_order_relaxed);
        const std::uint64_t ns = a.ns.exchange(0, std::memory_order_relaxed);
        const std::uint64_t max_ns = a.max_ns.exchange(0, std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        const double us_per_s = static_cast<double>(ns) * 1e-3 / seconds;
        total_us_per_s += us_per_s;
        text += fmt::format("{}{} {:.0f}/s avg {:.2f} max {:.1f} us ({:.2f} us/frame)",
                            text.empty() ? "" : "; ", Names[i],
                            static_cast<double>(calls) / seconds,
                            static_cast<double>(ns) * 1e-3 / static_cast<double>(calls),
                            static_cast<double>(max_ns) * 1e-3, us_per_s / 60.0);
    }
    return fmt::format("{} | all {:.2f} us per 60 Hz frame", text.empty() ? "none" : text,
                       total_us_per_s / 60.0);
}

// VR_HIT_FLASH_TEST: the forced flash's direction (true) or the plain ring (`plain`, false with
// `forced`); not forced when unset.
struct FlashTest {
    bool forced{false};
    bool directional{false};
    float azimuth_rad{0.0f};
};

inline const FlashTest& HitFlashTest() {
    static const FlashTest t = [] {
        FlashTest r{};
        const char* v = std::getenv("VR_HIT_FLASH_TEST");
        if (v == nullptr || *v == '\0') {
            return r;
        }
        r.forced = true;
        if (std::string{v} != "plain") {
            r.directional = true;
            r.azimuth_rad = static_cast<float>(std::atof(v)) * 0.01745329252f;
        }
        return r;
    }();
    return t;
}

} // namespace VR::FeatureCost
