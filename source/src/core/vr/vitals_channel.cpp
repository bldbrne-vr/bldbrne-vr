// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/inframe.h"
#include "core/vr/seqlock.h"
#include "core/vr/vitals_channel.h"

namespace VR::Vitals {

namespace {

SeqLock<Status> g_status;
SeqLock<GaugeState> g_gauge;
SeqLock<Beat> g_beat;

constexpr std::int64_t StaleNs = 1'000'000'000;

} // Anonymous namespace

void PublishStatus(const Status& status) {
    g_status.Store(status);
}

bool LatestStatus(Status& out) {
    return g_status.Load(out);
}

void PublishGauge(const GaugeState& state) {
    g_gauge.Store(state);
}

bool LatestGauge(GaugeState& out) {
    return g_gauge.Load(out);
}

void PublishBeat(const Beat& beat) {
    g_beat.Store(beat);
}

bool LatestBeat(Beat& out) {
    return g_beat.Load(out);
}

bool WristWorks(std::int64_t now_ns, const char** why) {
    const auto fail = [&](const char* reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    Status s{};
    if (!LatestStatus(s)) {
        return fail("no HP read yet");
    }
    if (!s.ok) {
        return fail("the HP / stamina / bullets read failed");
    }
    if (now_ns - s.steady_ns > StaleNs) {
        return fail("the HP read is stale (no player update for 1 s)");
    }
    if (!VR::InFrame::Available()) {
        return fail("the in-frame renderer cannot draw into the game's frame");
    }
    GaugeState g{};
    if (LatestGauge(g) && now_ns - g.tick_ns <= StaleNs && g.tick_ns - g.anchor_ns > StaleNs) {
        return fail("the hunter's left forearm (dummies 3, 153, 344) is not found");
    }
    if (why != nullptr) {
        *why = nullptr;
    }
    return true;
}

} // namespace VR::Vitals
