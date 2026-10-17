// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "common/logging/log.h"
#include "core/vr/body_channel.h"
#include "core/vr/body_presets.h"
#include "core/vr/seqlock.h"
#include "core/vr/vr_session.h"

namespace VR::Body {

namespace {

namespace P = VR::BodyPresets;

SeqLock<HandsSample> g_hands;
SeqLock<Anchor> g_anchor;
SeqLock<PairLock> g_pair_lock;
std::atomic<u32> g_marks{0};
SeqLock<BodyStatus> g_status;

// Body events (pose hook -> presenter): a small ring; rare, so a mutex is fine.
struct EventQueue {
    std::mutex mutex;
    std::array<BodyEvent, 64> ring{};
    u32 head{0}; // the oldest
    u32 count{0};
};

EventQueue& Events() {
    static EventQueue queue;
    return queue;
}

// VR_BODY_SIM_HANDS=<presets> (--vr-sim): the timeline, parsed once.
const std::vector<const P::Preset*>& SimTimeline() {
    static const std::vector<const P::Preset*> timeline = [] {
        const char* spec = std::getenv("VR_BODY_SIM_HANDS");
        if (spec == nullptr || spec[0] == '\0') {
            return std::vector<const P::Preset*>{};
        }
        std::string unknown;
        auto parsed = P::ParseTimeline(spec, &unknown);
        LOG_INFO(Core, "VR body: --vr-sim scripted hands '{}' ({} segments of {:.0f} s){}", spec,
                 parsed.size(), P::SegmentSeconds,
                 unknown.empty() ? std::string{} : " unknown: " + unknown);
        return parsed;
    }();
    return timeline;
}

// The scripted hands of VR_BODY_SIM_HANDS, relative to the synthetic head: the presets are given
// for a head at (0, 1.6, 0), the simulated head sits at the tracking origin.
bool SimulatedHands(HandsSample& out) {
    const auto& timeline = SimTimeline();
    if (timeline.empty()) {
        return false;
    }
    static const s64 start = SteadyNowNs();
    static std::atomic<u64> sequence{0};
    static std::atomic<u64> logged_segment{~u64{0}};
    const s64 now = SteadyNowNs();
    const P::TimelinePoint point = P::Locate(timeline, static_cast<double>(now - start) * 1e-9);
    if (logged_segment.exchange(point.segment) != point.segment) {
        LOG_INFO(Core, "VR body: sim hands seg {} ({})", point.preset->name, point.preset->expect);
    }
    const VR::HeadState head = VR::GetHead();
    out = {};
    out.seq = sequence.fetch_add(1) + 1;
    out.xr_time = now;
    out.steady_ns = now;
    out.head_valid = head.valid;
    out.simulated = true;
    out.head_p = {head.head.position.x, head.head.position.y, head.head.position.z};
    out.head_q = {head.head.orientation.x, head.head.orientation.y, head.head.orientation.z,
                  head.head.orientation.w};
    for (int side = 0; side < 2; ++side) {
        const P::HandPose pose = P::Evaluate(*point.preset, side, point.t);
        TrackedHand& hand = out.hand[static_cast<size_t>(side)];
        hand.position_valid = true;
        hand.position_tracked = pose.tracked;
        hand.orientation_valid = true;
        hand.orientation_tracked = true;
        hand.p = Vec3{pose.p[0], pose.p[1] - P::HeadHeight, pose.p[2]} + out.head_p;
        hand.q = {pose.q[0], pose.q[1], pose.q[2], pose.q[3]};
        hand.grip = side == 1 && point.preset->grip_right ? 1.0f : 0.0f;
    }
    return true;
}

} // Anonymous namespace

s64 SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void PublishHands(const HandsSample& sample) {
    g_hands.Store(sample);
}

bool LatestHands(HandsSample& out) {
    if (VR::IsSimulatedHead() && SimulatedHands(out)) {
        return true;
    }
    return g_hands.Load(out);
}

void PublishAnchor(const Anchor& anchor) {
    g_anchor.Store(anchor);
}

bool LatestAnchor(Anchor& out) {
    return g_anchor.Load(out);
}

void PublishPairLock(const PairLock& lock) {
    g_pair_lock.Store(lock);
}

bool LatestPairLock(PairLock& out) {
    return g_pair_lock.Load(out);
}

void NoteMark() {
    g_marks.fetch_add(1, std::memory_order_relaxed);
}

u32 MarkCount() {
    return g_marks.load(std::memory_order_relaxed);
}

void PublishBodyStatus(const BodyStatus& status) {
    g_status.Store(status);
}

bool LatestBodyStatus(BodyStatus& out) {
    return g_status.Load(out);
}

void PushBodyEvent(const BodyEvent& event) {
    auto& q = Events();
    std::scoped_lock lock{q.mutex};
    if (q.count == q.ring.size()) {
        return;
    }
    q.ring[(q.head + q.count) % q.ring.size()] = event;
    ++q.count;
}

u32 PollBodyEvents(BodyEvent* out, u32 max) {
    auto& q = Events();
    std::scoped_lock lock{q.mutex};
    u32 n = 0;
    while (n < max && q.count > 0) {
        out[n++] = q.ring[q.head];
        q.head = (q.head + 1) % static_cast<u32>(q.ring.size());
        --q.count;
    }
    return n;
}

} // namespace VR::Body
