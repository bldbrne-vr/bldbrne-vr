// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/vr/lever_prompt.h"
#include "core/vr/physical_lever.h"
#include "core/vr/seqlock.h"
#include <atomic>
#include <mutex>

namespace VR::Lever {
// The game publishes only a verified, permitted offer. Geometry is in game units;
// the presenter converts it to metres without ever receiving a guest pointer.
struct ProbeHands {
    std::int64_t ns{};
    std::array<Vec3, 2> world{};
    std::array<bool, 2> enabled{};
    float radius{};
};
struct Offer {
    std::int64_t ns{};
    Candidate candidate{};
    std::int64_t request_ns{}; // identifies the squeeze that requested this resolution
};
// The hand-pullable lever the game selected last (its rig bound at rest; no permission check):
// where its handle is, so the XR side can arm it while near (lever_prompt.h ArmMargin), tick a
// hand that comes within reach and log how close the hands get. Never an offer: a grab still goes
// through a request and the game's checks. `armed` false: still in place but not grabbable now
// (used or running, animating, physical levers off, or its normal prompt is back for a while
// after B or a refused pull); the XR side keeps asking while near.
struct Nearby {
    std::int64_t ns{};
    std::uint64_t id{}; // object handle
    Rig rig{};          // game units
    bool armed{};
    float angle{}; // where its handle stands: 0 at rest; a parked capstan's angle (radians)
};
// Where the player is, for the game's proximity scan (lever_prompt.h ScanNs): a lever arms by
// proximity alone, without the game ever selecting it (its prompt zone stands the hunter about
// 1.9 m in front of the hinge, out of reach of the handle). Game units, as the rigs are.
struct ScanRequest {
    std::int64_t ns{};
    float scale{}; // game units per metre
    Vec3 head{};
    std::array<Vec3, 2> hands{};
    std::array<bool, 2> tracked{};
};
enum class CommandKind : std::uint8_t { Hold, Release, Complete, Settle };
struct Command {
    std::uint64_t serial{};
    std::int64_t ns{};
    std::uint64_t id{}, generation{};
    float angle{};
    int hand{-1};
    CommandKind kind{CommandKind::Hold};
};
struct HandPresentation {
    std::int64_t ns{};
    std::uint32_t space{};
    int hand{-1};
};
struct Applied {
    std::int64_t ns{};
    std::uint64_t id{}, generation{};
    float angle{};
};
struct Completion {
    std::int64_t ns{};
    std::uint64_t serial{}, id{}, generation{};
    int hand{-1};
    bool accepted{};
};
inline bool Fresh(std::int64_t stamp, std::int64_t now) {
    return stamp > 0 && now >= stamp && now - stamp <= 150'000'000;
}

// One XR command writer and one game-thread consumer. The small locked command slot
// also serializes wake/retire, so a release cannot be lost between reading and sleeping.
// The entry hook reads work directly and avoids entering C++ while it is zero.
class Transport {
  public:
    std::atomic<bool> motion_test{false};
    std::atomic<bool> installed{false}; // enabled only after game hooks/resolver are ready
    std::atomic<std::uint8_t> work{0};
    // Native selection hook: cheap hint only, with no pose/permission reads. Advanced only
    // while the selected action is a lever instance whose rig binds (`verdict.bound`). The XR
    // reader timestamps changes, so an abandoned interaction prompt ages out.
    std::atomic<std::uint64_t> selected_hint{0};
    std::atomic<std::int64_t> acquire{0}; // one request per squeeze; zero means asleep
    // XR writer while near the armed lever (twice a second): the game re-checks it is still in
    // place, selectable and at rest, from the scheduler hook, and republishes Nearby. Zero: idle.
    std::atomic<std::int64_t> nearby_check{0};
    void RequestNearbyCheck(std::int64_t ns) { nearby_check.store(ns, std::memory_order_release); }
    // XR writer a few times a second while a hand can pull a lever and no armed one is near: the
    // game looks for a bound lever near the head or a hand and arms it (no selection needed),
    // from the scheduler hook. Zero: idle (the native gate skips the host).
    std::atomic<std::int64_t> scan{0};
    // The steady time a parked capstan's hand pull stopped being possible, so its own scheduler
    // puts its bar back at rest once (the game still holds it unturned): written by the XR side
    // (physical levers switched off, or no display mode a hand pulls in) and by the selection
    // classifier (its prompt given back, by B or a refused pull). The first request stands until
    // the scheduler serves it. Zero: idle (the native gate skips the host).
    std::atomic<std::int64_t> unpark{0};
    void RequestUnpark(std::int64_t ns) {
        std::int64_t idle = 0;
        unpark.compare_exchange_strong(idle, ns, std::memory_order_acq_rel);
    }
    void RequestScan(const ScanRequest &where) {
        scan_where_.Store(where);
        scan.store(where.ns, std::memory_order_release);
    }
    bool RequestedScan(ScanRequest &where) const { return scan_where_.Load(where, 4); }
    // XR writer: B at a hand-pulled lever (hint fresh, nothing held) asks for its normal prompt
    // and button back (lever_prompt.h FallbackNs, FallbackPress). The native gate compares it
    // with verdict.fallback, so a change reaches the host on the next selection pass.
    std::atomic<std::uint64_t> prompt_fallback{0};
    // Any thread: a lever's prompt may have changed (a refused pull demoted it, a hold began, the
    // XR side's readiness changed). The native gate compares it with verdict.serial, so the host
    // reclassifies on the next selection pass; nothing else writes the verdict off that thread.
    std::atomic<std::uint64_t> prompt_serial{0};
    void PromptChanged() { prompt_serial.fetch_add(1, std::memory_order_acq_rel); }
    // XR writer: the steady time a hand could last pull a lever (immersive first person, the body
    // driven by fresh tracking, a hand tracked); 0 once the display mode cannot. A hand-pulled
    // lever hides its prompt only while this is fresh (lever_prompt.h ReadyNs).
    std::atomic<std::int64_t> hand_pull_ready{0};
    // Game thread only (native selection gate and its host classifier): which selected lever
    // instance has its prompt replaced. Read by generated code at fixed offsets.
    PromptVerdict verdict{};
    // Game thread writer: the map object a physical hold or settle owns right now (0: none).
    // The script-prompt gate hides an EMEVD action-button prompt only on this object.
    std::atomic<std::uint64_t> held_object{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
                  sizeof(std::atomic<std::uint64_t>) == 8);
    void RequestOffer(std::int64_t ns) { acquire.store(ns, std::memory_order_release); }
    void RequestMotionOffer(const ProbeHands &hands) {
        probe_hands_.Store(hands);
        RequestOffer(hands.ns);
    }
    bool RequestedHands(ProbeHands &hands) const { return probe_hands_.Load(hands, 4); }
    void CancelOffer(std::int64_t ns) {
        acquire.compare_exchange_strong(ns, 0, std::memory_order_acq_rel);
    }
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free &&
                  sizeof(std::atomic<std::uint8_t>) == 1);

    // Game thread only. Clearing is also required when a hold is rejected or retired.
    void PublishOffer(const Offer &offer) {
        offers_.Store(offer);
        offered_.store(offer.ns, std::memory_order_release);
    }
    bool LatestOffer(Offer &out) const {
        return offered_.load(std::memory_order_acquire) && offers_.Load(out, 4);
    }
    // XR reader: stop polling an expired offer. A newer game publication wins the CAS.
    void DismissOffer(const Offer &expired) {
        auto expected = expired.ns;
        offered_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    // Game thread (the selection classifier) writes; the XR reader reads while a lever is near.
    void PublishNearby(const Nearby &lever) { nearby_.Store(lever); }
    bool LatestNearby(Nearby &out) const { return nearby_.Load(out, 4) && out.ns > 0; }
    // XR writer; body and held-item readers. Inactive readers only test one atomic.
    void PresentHand(const HandPresentation &hand) {
        if (hand.ns <= 0 || hand.hand < 0 || hand.hand > 1) {
            presenting_.store(false, std::memory_order_release);
            return;
        }
        presentation_.Store(hand);
        presenting_.store(true, std::memory_order_release);
    }
    bool PresentedHand(HandPresentation &hand) const {
        return presenting_.load(std::memory_order_acquire) && presentation_.Load(hand, 4);
    }
    void RecordCompletion(const Completion &completion) { completion_.Store(completion); }
    bool LatestCompletion(Completion &out) const { return completion_.Load(out, 4); }
    void RecordApplied(const Applied &applied) { applied_.Store(applied); }
    bool LatestApplied(Applied &out) const { return applied_.Load(out, 4); }

    // A terminal request stays latched until the game has finished cleanup/activation.
    // Returning false means the caller must cancel its local hold, retaining grip ownership.
    bool Publish(Command next) {
        if (next.ns <= 0 || next.id == 0 || next.hand < 0 || next.hand > 1 ||
            !std::isfinite(next.angle)) {
            return false;
        }
        std::scoped_lock lock{mutex_};
        if (work.load(std::memory_order_relaxed) != 0 &&
            (command_.kind != CommandKind::Hold || command_.id != next.id ||
             command_.generation != next.generation || command_.hand != next.hand ||
             next.ns < command_.ns)) {
            return false;
        }
        next.serial = ++serial_;
        command_ = next;
        work.store(1, std::memory_order_release);
        return true;
    }
    bool Read(Command &out) const {
        if (work.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lock{mutex_};
        if (work.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        out = command_;
        return true;
    }
    // Called only after cleanup or completion. A newer hand sample/release keeps the
    // gate awake; a consumer cannot erase a producer's update with an old acknowledgement.
    bool Retire(std::uint64_t serial) {
        std::scoped_lock lock{mutex_};
        if (work.load(std::memory_order_relaxed) == 0 || command_.serial != serial) {
            return false;
        }
        applied_.Store({}); // a later grab must wait for its own applied pose
        work.store(0, std::memory_order_release);
        return true;
    }
    bool CleanupPending() const {
        Command command{};
        return Read(command) && command.kind != CommandKind::Hold;
    }

  private:
    SeqLock<ProbeHands> probe_hands_;
    SeqLock<ScanRequest> scan_where_;
    SeqLock<HandPresentation> presentation_;
    std::atomic<bool> presenting_{false};
    SeqLock<Offer> offers_;
    SeqLock<Nearby> nearby_;
    SeqLock<Applied> applied_;
    SeqLock<Completion> completion_;
    std::atomic<std::int64_t> offered_{0};
    mutable std::mutex mutex_;
    Command command_{};
    std::uint64_t serial_{};
};
inline Transport &Channel() {
    static Transport channel;
    return channel;
}
} // namespace VR::Lever
