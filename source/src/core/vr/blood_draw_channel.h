// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The right hand's empty syringe (core/vr/blood_draw.h) across threads: the presenter's state to
// the render-camera hook (the syringe drawn in the right fist) and the pose hook (the right weapon
// put away while it is there, core/vr/bloodborne_held.cpp); the hook's syringe tip back to the
// presenter's stab gate (VialContact::LeftArmGate), as the Blood Vial's (Quick::VialContact).
#include <atomic>
#include <cstdint>
#include "core/vr/blood_draw.h"
#include "core/vr/seqlock.h"
#include "core/vr/vial_contact.h"

namespace VR::BloodDraw {

struct Scene {
    std::int64_t steady_ns{0};
    bool shown{false};  // the syringe in the right fist
    float fill{0.0f};   // the blood in it, 0..1
    Phase phase{Phase::Idle};
};
inline constexpr std::int64_t StaleNs = 250'000'000;

inline SeqLock<Scene>& SceneChannel() {
    static SeqLock<Scene> channel;
    return channel;
}
inline void PublishScene(const Scene& s) {
    SceneChannel().Store(s);
}
// The syringe shown and fresh at `now_ns` (the steady clock): draw it, hide the right weapon.
inline bool LatestActive(std::int64_t now_ns, Scene& out) {
    return SceneChannel().Load(out) && out.shown && now_ns - out.steady_ns >= -StaleNs &&
           now_ns - out.steady_ns < StaleNs;
}

// The syringe's tip on the right grip (grip_offset in the right grip's frame), each drawn frame.
inline SeqLock<VialContact::Sample>& ContactChannel() {
    static SeqLock<VialContact::Sample> channel;
    return channel;
}

// The stab's ask for the game's blood bullets, the presenter to the main player's update (site A,
// core/vr/bloodborne_vitals.cpp BloodBulletsSiteA), and its answer back. An ask older than
// AskTimeoutNs when site A first sees it is dropped unanswered (a load, a pause).
inline constexpr std::int64_t AskTimeoutNs = 500'000'000;
struct Asks {
    std::atomic<std::uint32_t> serial{0};
    std::atomic<std::int64_t> steady_ns{0};
    std::atomic<std::uint64_t> answer{0}; // (serial << 8) | Answer
    std::atomic<std::uint32_t> cancelled{0}; // asks up to this serial are never taken
    // The game's D-pad up instead (Answer::Unavailable): site A's Blood Bullets count while the
    // presenter waits for them to rise, whatever the HUD reads ((serial << 32) | count).
    std::atomic<std::uint64_t> bullets{0};
};
inline Asks& BulletsAsks() {
    static Asks asks;
    return asks;
}
// The presenter: a new ask at `now_ns`; its serial.
inline std::uint32_t AskBullets(std::int64_t now_ns) {
    auto& a = BulletsAsks();
    a.steady_ns.store(now_ns, std::memory_order_relaxed);
    return a.serial.fetch_add(1, std::memory_order_release) + 1;
}
// The presenter: the answer to the ask `serial` (None while it is unanswered).
inline Answer AnswerFor(std::uint32_t serial) {
    const std::uint64_t v = BulletsAsks().answer.load(std::memory_order_acquire);
    return serial != 0 && static_cast<std::uint32_t>(v >> 8) == serial
               ? static_cast<Answer>(v & 0xFF)
               : Answer::None;
}
// The presenter: the ask `serial` is over (refused, timed out, a context switch): if site A has
// not taken it yet it never will (no HP spent behind a refusal the player was shown).
inline void CancelAsk(std::uint32_t serial) {
    auto& a = BulletsAsks();
    std::uint32_t was = a.cancelled.load(std::memory_order_relaxed);
    while (was < serial &&
           !a.cancelled.compare_exchange_weak(was, serial, std::memory_order_release)) {
    }
}
// Site A: the newest ask, if it is not `done` (site A's last one), not cancelled and fresh at
// `now_ns`; else 0.
inline std::uint32_t TakeAsk(std::uint32_t done, std::int64_t now_ns) {
    auto& a = BulletsAsks();
    const std::uint32_t s = a.serial.load(std::memory_order_acquire);
    if (s == 0 || s == done || s <= a.cancelled.load(std::memory_order_acquire)) return 0;
    const std::int64_t at = a.steady_ns.load(std::memory_order_relaxed);
    return now_ns - at >= -AskTimeoutNs && now_ns - at < AskTimeoutNs ? s : 0;
}
inline void PublishAnswer(std::uint32_t serial, Answer answer) {
    BulletsAsks().answer.store(
        (static_cast<std::uint64_t>(serial) << 8) | static_cast<std::uint8_t>(answer),
        std::memory_order_release);
}
// Site A, while the ask `serial` went to the game's D-pad up: the Blood Bullets now.
inline void PublishBullets(std::uint32_t serial, std::int32_t count) {
    BulletsAsks().bullets.store((static_cast<std::uint64_t>(serial) << 32) |
                                    static_cast<std::uint32_t>(count),
                                std::memory_order_release);
}
// The presenter: site A's count for the ask `serial` (false: none for it).
inline bool BulletsFor(std::uint32_t serial, std::int32_t& count) {
    const std::uint64_t v = BulletsAsks().bullets.load(std::memory_order_acquire);
    if (serial == 0 || static_cast<std::uint32_t>(v >> 32) != serial) return false;
    count = static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
    return true;
}

} // namespace VR::BloodDraw
