// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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

void ParseCutterLab(const std::string& value, CutterLab& out) {
    // "0" / "1": the built-in dials (every vr.ini written before 2026-11-08 has whip_cutter_lab=0:
    // it must not turn the lash off - whip_cutter_lash does); else the lab's names, from the
    // built-in dials (names this chain has no use for are left out).
    CutterLab lab{};
    bool guided = false; // the lab's model=guided keeps its guide vector; any other, none
    std::size_t at = 0;
    const bool pairs = !(value == "0" || value == "1" || value == "off" || value.empty());
    while (pairs && at < value.size()) {
        const std::size_t end = std::min(value.find_first_of(", ", at), value.size());
        const std::string kv = value.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq);
        if (k == "model") {
            guided = kv.substr(eq + 1) == "guided";
            lab.path_model = kv.substr(eq + 1) != "guided" && kv.substr(eq + 1) != "free";
            continue;
        }
        float v = 0.0f;
        try {
            v = std::stof(kv.substr(eq + 1));
        } catch (const std::exception&) {
            continue; // the lab's other words (compare=, motion=, view=)
        }
        if (k == "air") lab.air = std::clamp(v, 0.0f, 10.0f);
        else if (k == "inner") lab.inner = std::clamp(v, 0.0f, 200.0f);
        else if (k == "tipmass") lab.tipmass = std::clamp(v, 0.05f, 10.0f);
        else if (k == "drive") lab.drive = std::clamp(v, 0.0f, 100.0f);
        else if (k == "dspan") lab.dspan = std::clamp(v, 0.1f, 5.0f);
        else if (k == "daccel") lab.daccel = std::clamp(v, 0.0f, 3000.0f);
        else if (k == "bend") lab.bend = std::clamp(v, 5.0f, 120.0f);
        else if (k == "iters") lab.iters = std::clamp(static_cast<int>(v), 4, 256);
        else if (k == "pinned") lab.pinned = std::clamp(static_cast<int>(v), 0, 8);
        else if (k == "tout") lab.tout = std::clamp(v, 0.02f, 2.0f);
        else if (k == "tin") lab.tin = std::clamp(v, 0.02f, 2.0f);
        else if (k == "mompay") lab.mompay = v != 0.0f;
        else if (k == "mout") lab.mout = std::clamp(v, 0.5f, 40.0f);
        else if (k == "min") lab.min = std::clamp(v, 0.0f, 30.0f);
        else if (k == "mhold") lab.mhold = std::clamp(v, 0.02f, 3.0f);
        else if (k == "vact") lab.vact = std::clamp(v, 0.0f, 20.0f);
        else if (k == "hithold") lab.hithold = std::clamp(v, 0.0f, 2.0f);
        else if (k == "guide") lab.guide = std::clamp(v, 0.0f, 60.0f);
        else if (k == "gzeta") lab.gzeta = std::clamp(v, 0.0f, 3.0f);
        else if (k == "gtaper") lab.gtaper = std::clamp(v, 0.0f, 1.0f);
        else if (k == "w0") lab.w0 = std::clamp(v, 0.0f, 200.0f);
        else if (k == "zeta") lab.zeta = std::clamp(v, 0.0f, 3.0f);
        else if (k == "rdrag") lab.rdrag = std::clamp(v, 0.0f, 10.0f);
        else if (k == "aim") lab.aim = std::clamp(v, 0.0f, 1.0f);
        else if (k == "path") lab.path = std::clamp(v, 0.0f, 120.0f);
        else if (k == "ptaper") lab.ptaper = std::clamp(v, 0.0f, 1.0f);
        else if (k == "pzeta") lab.pzeta = std::clamp(v, 0.0f, 3.0f);
        else if (k == "pdelay") lab.pdelay = std::clamp(v, 0.0f, 0.3f);
        else if (k == "pcurl") lab.pcurl = std::clamp(v, 0.0f, 90.0f);
        else if (k == "pkeep") lab.pkeep = std::clamp(v, 0.0f, 1.0f);
        else if (k == "pout") lab.pout = std::clamp(v, 0.5f, 20.0f);
        else if (k == "pin") lab.pin = std::clamp(v, 0.0f, 10.0f);
        else if (k == "pmin") lab.pmin = std::clamp(v, 0.0f, 1.0f);
        else if (k == "pturn") lab.pturn = std::clamp(v, 0.0f, 0.5f);
        else if (k == "pslow") lab.pslow = std::clamp(v, 0.0f, 3.0f);
        else if (k == "paccel") lab.paccel = std::clamp(v, 0.0f, 3.0f);
        else if (k == "pfollow") lab.pfollow = std::clamp(v, 0.0f, 200.0f);
        else if (k == "prate") lab.prate = std::clamp(v, 0.0f, 5000.0f);
        else if (k == "pground") lab.pground = std::clamp(v, 0.0f, 1.0f);
        else if (k == "pspin") lab.pspin = std::clamp(v, 0.0f, 1.0f);
        else if (k == "pspinacc") lab.pspinacc = std::clamp(v, 1.0f, 500.0f);
        else if (k == "pgrav") lab.pgrav = std::clamp(v, 0.0f, 3.0f);
    }
    if (!guided) lab.guide = 0.0f;
    lab.on = out.on;
    out = lab;
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
    } else if (key == "whip_cutter_start") {
        s.cutter_start = std::clamp(std::stof(value), 0.2f, 1.2f);
    } else if (key == "whip_cutter_keep") {
        s.cutter_keep = std::clamp(std::stof(value), 0.1f, 1.0f);
    } else if (key == "whip_cutter_hold_s") {
        const float v = std::stof(value);
        s.cutter_hold_s = v <= 0.0f ? 0.0f : std::clamp(v, 0.1f, 3.0f);
    } else if (key == "whip_cutter_throw") {
        s.cutter_throw = value == "1" || value == "true";
    } else if (key == "whip_cutter_follow") {
        s.cutter_follow = std::clamp(std::stof(value), 0.0f, 1.0f);
    } else if (key == "whip_cutter_turn") {
        s.cutter_turn = value == "1" || value == "true";
    } else if (key == "whip_cutter_tip_keep") {
        s.cutter_tip_keep = std::clamp(std::stof(value), 0.0f, 30.0f);
    } else if (key == "whip_cutter_spine") {
        s.cutter_spine = std::clamp(std::stof(value), 0.0f, 0.5f);
    } else if (key == "whip_cutter_unfold") {
        s.cutter_unfold = std::clamp(std::stof(value), 0.0f, 0.95f);
    } else if (key == "whip_cutter_lash") {
        s.cutter_lab.on = value == "1" || value == "true" || value == "on";
    } else if (key == "whip_cutter_lab") {
        ParseCutterLab(value, s.cutter_lab);
    } else if (key == "whip_cutter_reel_s") {
        const float v = std::stof(value);
        s.cutter_reel_s = v <= 0.0f ? 0.0f : std::clamp(v, 0.08f, 1.0f);
    } else if (key == "whip_cutter_full") {
        const float v = std::stof(value);
        s.cutter_full = v <= 0.0f ? 0.0f : std::clamp(v, 3.0f, 25.0f);
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
        << "whip_payout_speed=" << d.payout_speed << "\n"
        << "# Beast Cutter dials (thrown more like the game's chain; every default as before):\n"
        << "# the share of whip_payout_speed that starts a payout\n"
        << "whip_cutter_start=" << d.cutter_start << "\n"
        << "# slower than this share of it, the pause before it comes back starts\n"
        << "whip_cutter_keep=" << d.cutter_keep << "\n"
        << "# the pause (s; 0 = whip_hold_s x 1.5 + 0.5)\n"
        << "whip_cutter_hold_s=" << d.cutter_hold_s << "\n"
        << "# 1 = a payout starts only while the weapon points ahead (no wind-up payout)\n"
        << "whip_cutter_throw=" << (d.cutter_throw ? 1 : 0) << "\n"
        << "# a throw's front follows the hand: 0 as before .. 1 (very stiff)\n"
        << "whip_cutter_follow=" << d.cutter_follow << "\n"
        << "# 1 = a stroke turning back brings the chain in, the next throws it out again\n"
        << "whip_cutter_turn=" << (d.cutter_turn ? 1 : 0) << "\n"
        << "# m/s: out, its tip faster than this keeps it out (0 = only your hand's speed)\n"
        << "whip_cutter_tip_keep=" << d.cutter_tip_keep << "\n"
        << "# out, each link pulled toward the line from your hand to its tip (0 off)\n"
        << "whip_cutter_spine=" << d.cutter_spine << "\n"
        << "# out, no link nearer your hand than this share of its length along the chain (0 off)\n"
        << "whip_cutter_unfold=" << d.cutter_unfold << "\n"
        << "# m/s: the swing that pays it fully out, a weaker one less (0 = whip_payout_speed)\n"
        << "whip_cutter_full=" << d.cutter_full << "\n"
        << "# the Beast Cutter's chain flies along your swing: out with its momentum, shorter as it "
           "fades,\n# back in once it is spent (the trigger and a flick back bring it in at once); "
           "0: the original\n"
        << "whip_cutter_lash=1\n"
        << "# its dials (0: the built-in ones; else name=value,... pairs, as the Cutter Chain Lab "
           "copies them)\n"
        << "whip_cutter_lab=0\n"
        << "# s: the reel-in once it starts (0 = 0.40)\n"
        << "whip_cutter_reel_s=" << d.cutter_reel_s << "\n";
}

std::string WhipSettingsText() {
    const auto& s = GetWhipSettings();
    return fmt::format("whip={} whip_reach_scale={:.2f} whip_hold_s={:.2f} whip_payout_speed={:.1f} "
                       "whip_flick_recall={} whip_pace={} whip_cutter_start={:.2f} whip_cutter_keep={:.2f} "
                       "whip_cutter_hold_s={:.2f} whip_cutter_throw={} whip_cutter_follow={:.2f} "
                       "whip_cutter_turn={} whip_cutter_tip_keep={:.1f} whip_cutter_spine={:.2f} "
                       "whip_cutter_unfold={:.2f} whip_cutter_full={:.1f} whip_cutter_reel_s={:.2f} "
                       "whip_cutter_lash={} whip_cutter_lab={}",
                       s.physical ? "physical" : "canned", s.reach_scale, s.hold_s,
                       s.payout_speed, s.flick_recall ? (s.flick_any ? "any" : "1") : "0",
                       s.pace ? 1 : 0, s.cutter_start, s.cutter_keep, s.cutter_hold_s,
                       s.cutter_throw ? 1 : 0, s.cutter_follow, s.cutter_turn ? 1 : 0,
                       s.cutter_tip_keep, s.cutter_spine, s.cutter_unfold, s.cutter_full, s.cutter_reel_s,
                       s.cutter_lab.on ? 1 : 0,
                       s.cutter_lab.on ? fmt::format("air={} inner={} tipmass={} drive={} dspan={} "
                                                     "daccel={} bend={} iters={} pinned={} mompay={} "
                                                     "mout={} min={} mhold={} vact={} hithold={} "
                                                     "guide={} gzeta={} gtaper={} w0={} zeta={} "
                                                     "rdrag={} aim={}{}",
                                                     s.cutter_lab.air, s.cutter_lab.inner,
                                                     s.cutter_lab.tipmass, s.cutter_lab.drive,
                                                     s.cutter_lab.dspan, s.cutter_lab.daccel,
                                                     s.cutter_lab.bend, s.cutter_lab.iters,
                                                     s.cutter_lab.pinned, s.cutter_lab.mompay ? 1 : 0,
                                                     s.cutter_lab.mout, s.cutter_lab.min,
                                                     s.cutter_lab.mhold, s.cutter_lab.vact,
                                                     s.cutter_lab.hithold, s.cutter_lab.guide,
                                                     s.cutter_lab.gzeta, s.cutter_lab.gtaper,
                                                     s.cutter_lab.w0, s.cutter_lab.zeta,
                                                     s.cutter_lab.rdrag, s.cutter_lab.aim,
                                                     s.cutter_lab.path_model
                                                         ? fmt::format(
                                                               " model=path path={} ptaper={} "
                                                               "pzeta={} pdelay={} pcurl={} "
                                                               "pkeep={} pout={} pin={} pfollow={} "
                                                               "prate={} pground={} pspin={} "
                                                               "pspinacc={} pgrav={} pmin={} "
                                                               "pturn={} pslow={} paccel={}",
                                                               s.cutter_lab.path, s.cutter_lab.ptaper,
                                                               s.cutter_lab.pzeta, s.cutter_lab.pdelay,
                                                               s.cutter_lab.pcurl, s.cutter_lab.pkeep,
                                                               s.cutter_lab.pout, s.cutter_lab.pin,
                                                               s.cutter_lab.pfollow, s.cutter_lab.prate,
                                                               s.cutter_lab.pground, s.cutter_lab.pspin,
                                                               s.cutter_lab.pspinacc, s.cutter_lab.pgrav,
                                                               s.cutter_lab.pmin, s.cutter_lab.pturn,
                                                               s.cutter_lab.pslow, s.cutter_lab.paccel)
                                                         : std::string{})
                                         : std::string{"0"});
}

namespace {
SeqLock<SwingInput> g_swing;
// Contact lifetime needs one coherent sample: never mix another weapon's speed with
// this weapon's identity, or a recall's lifting flag with an older asked-out flag.
SeqLock<WhipFeel> g_feel;
} // namespace

void PublishSwingInput(const SwingInput& input) {
    g_swing.Store(input);
}

SwingInput LatestSwingInput(std::int64_t now_ns, std::int64_t max_age_ns) {
    SwingInput input{};
    return g_swing.Load(input) ? FreshSwingInput(input, now_ns, max_age_ns) : SwingInput{};
}

void PublishWhipFeel(const WhipFeel& f) {
    g_feel.Store(f);
}

WhipFeel LatestWhipFeel() {
    // A reader overlapping a publication keeps its last coherent sample. Contact callers
    // still enforce its age, so contention cannot cancel a live attack or keep stale motion.
    thread_local WhipFeel f{};
    g_feel.Load(f);
    return f;
}

} // namespace VR::Whip
