// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <ostream>
#include <stdexcept>
#include <fmt/format.h>

#include "core/vr/whip_settings.h"
#include "core/vr/seqlock.h"

namespace VR::Whip {

WhipSettings& GetWhipSettings() {
    static WhipSettings settings;
    return settings;
}

bool ParseWhipSetting(const std::string& key, const std::string& value) {
    auto& s = GetWhipSettings();
    if (key == "whip") {
        if (value != "physical" && value != "canned") {
            throw std::invalid_argument{"physical or canned expected"};
        }
        s.physical = value == "physical";
    } else if (key == "whip_reach_scale") {
        s.reach_scale = std::clamp(std::stof(value), 0.3f, 1.5f);
    } else if (key == "whip_hold_s") {
        s.hold_s = std::clamp(std::stof(value), 0.1f, 2.0f);
    } else if (key == "whip_payout_speed") {
        s.payout_speed = std::clamp(std::stof(value), 3.0f, 15.0f);
    } else if (key == "whip_flick_recall") {
        s.flick_any = value == "any";
        s.flick_recall = s.flick_any || value == "1" || value == "true";
    } else if (key == "whip_pace") {
        s.pace = value == "1" || value == "true";
    } else {
        return false;
    }
    return true;
}

void WriteWhipDefaults(std::ostream& out) {
    const WhipSettings d{};
    out << "# ---- The whip forms (the Beast Cutter's and the Threaded Cane's form B) ----\n"
        << "# physical = the whip is a chain on your hand: swung fast it pays out toward its "
           "reach cap, stays\n# out while you keep it moving (spin it overhead: it circles like a "
           "helicopter blade) and reels\n# back in when you slow down; it hits where you see it "
           "(no attack animation; each revolution of\n# a spin counts as a swing); canned = a "
           "swing presses R1 / R2 (the game's attack animations)\n"
        << "# Beast Cutter form B: hold the right trigger to recall and keep it compact; "
           "release to allow extension.\n"
        << "whip=" << (d.physical ? "physical" : "canned") << "\n"
        << "# reach scale (1 = the default tuned caps, measured from the grip):\n"
        << "# Beast Cutter light/heavy/charged: 4.7 / 6.6 / 7.4 m; "
           "Threaded Cane: about 4.15 / 3.17 / 3.60 m.\n"
        << "# Actual tip distance depends on the swing and the chain's shape.\n"
        << "whip_reach_scale=" << d.reach_scale << "\n"
        << "# slow this long (s), the whip reels back in\n"
        << "whip_hold_s=" << d.hold_s << "\n"
        << "# the swing's speed (m/s, the weapon's far end) that pays the whip fully out\n"
        << "whip_payout_speed=" << d.payout_speed << "\n";
}

std::string WhipSettingsText() {
    const auto& s = GetWhipSettings();
    return fmt::format("whip={} whip_reach_scale={:.2f} whip_hold_s={:.2f} whip_payout_speed={:.1f} "
                       "whip_flick_recall={} whip_pace={}",
                       s.physical ? "physical" : "canned", s.reach_scale, s.hold_s,
                       s.payout_speed, s.flick_recall ? (s.flick_any ? "any" : "1") : "0",
                       s.pace ? 1 : 0);
}

namespace {
SeqLock<SwingInput> g_swing;
std::atomic<std::int64_t> g_feel_ns{0};
std::atomic<float> g_feel_out{0.0f};
std::atomic<float> g_feel_tip{0.0f};
std::atomic<std::uint32_t> g_feel_locks{0};
std::atomic<std::uint32_t> g_feel_reels{0};
std::atomic<bool> g_feel_driving{false};
std::atomic<std::uint32_t> g_feel_lash{0};
std::atomic<bool> g_feel_asked_out{false};
std::atomic<bool> g_feel_lifting{false};
} // namespace

void PublishSwingInput(const SwingInput& input) {
    g_swing.Store(input);
}

SwingInput LatestSwingInput(std::int64_t now_ns, std::int64_t max_age_ns) {
    SwingInput input{};
    return g_swing.Load(input) ? FreshSwingInput(input, now_ns, max_age_ns) : SwingInput{};
}

void PublishWhipFeel(const WhipFeel& f) {
    g_feel_out.store(f.out, std::memory_order_relaxed);
    g_feel_tip.store(f.tip_speed, std::memory_order_relaxed);
    g_feel_locks.store(f.locks, std::memory_order_relaxed);
    g_feel_reels.store(f.reels, std::memory_order_relaxed);
    g_feel_driving.store(f.driving, std::memory_order_relaxed);
    g_feel_lash.store(f.lash, std::memory_order_relaxed);
    g_feel_asked_out.store(f.asked_out, std::memory_order_relaxed);
    // After asked_out (release): a reader that sees this frame's `lifting` sees its asked_out too.
    g_feel_lifting.store(f.lifting, std::memory_order_release);
    g_feel_ns.store(f.ns, std::memory_order_release);
}

WhipFeel LatestWhipFeel() {
    WhipFeel f{};
    f.ns = g_feel_ns.load(std::memory_order_acquire);
    f.out = g_feel_out.load(std::memory_order_relaxed);
    f.tip_speed = g_feel_tip.load(std::memory_order_relaxed);
    f.locks = g_feel_locks.load(std::memory_order_relaxed);
    f.reels = g_feel_reels.load(std::memory_order_relaxed);
    f.driving = g_feel_driving.load(std::memory_order_relaxed);
    f.lash = g_feel_lash.load(std::memory_order_relaxed);
    // `lifting` before asked_out (acquire): never the frame before's asked_out (out, a recall's
    // lift pending) with the next frame's `lifting` (none: the recall fired), a crack's opening.
    f.lifting = g_feel_lifting.load(std::memory_order_acquire);
    f.asked_out = g_feel_asked_out.load(std::memory_order_relaxed);
    return f;
}

} // namespace VR::Whip
