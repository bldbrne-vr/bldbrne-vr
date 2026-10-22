// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>

// Physical gestures (vr.ini pose_gestures): a pose of your own
// arms and head is recognised and the game gets the gesture's marker SpEffect directly. No gesture
// is requested and no animation plays: the hunter's body stays yours.
//
// What the game checks (v1.09; every event, talk and AI script scanned): each gesture clip's
// TimeAct puts a 0.1 s marker SpEffect on the hunter, and only two places react to the markers:
//  - the fallen Brain of Mensis (m26_00, event 12600028): flag 12600027 ON, the player inside region
//    2602022 (a cylinder of radius 10 m, height 5 m at (95.48, 943.53, -31.81)) and SpEffect 164,
//    which only Make Contact's "arms shifted" clip applies (with 161), after holding it ~48-75 s;
//  - the Doll (m21_00, event 12100117): her observe flag 12100118 ON (her AI sets it while you stand
//    close in front of her), you are the host, and SpEffect 161, 162 or 163 (a gesture's category).
// So the recognisers run only there (Zone); everywhere else the game side makes one cheap check a
// second (bloodborne_pose.cpp).
//
// Header-only and pure (tests/test_pose_gestures.cpp): the settings, the game's facts, the learned
// list's decoding, the pose classifier in the head's frame, the recogniser (settle, stillness, a
// grace for tracking blips, the hold time), the game side's gates and its marker decision (the
// Brain's hold timer, the Doll's cooldown).
namespace VR::PoseGesture {

// ---- Settings (vr.ini) ----

struct PoseSettings {
    bool enabled{true};          // pose_gestures (NOT `gestures`: that is the right hand's swings)
    bool all{false};             // pose_gestures_all: every gesture counts, learned or not
    float contact_hold_s{10.0f}; // pose_contact_hold_s: Make Contact held this long at the Brain
};

inline PoseSettings& GetPoseSettings() {
    static PoseSettings settings;
    return settings;
}

// VR::LoadSettings: true if `key` is one of ours (and it was applied); throws on a bad number.
inline bool ParsePoseSetting(PoseSettings& s, const std::string& key, const std::string& value) {
    const auto flag = [&value] { return value == "1" || value == "true"; };
    if (key == "pose_gestures") {
        s.enabled = flag();
    } else if (key == "pose_gestures_all") {
        s.all = flag();
    } else if (key == "pose_contact_hold_s") {
        const float v = std::stof(value);
        if (!std::isfinite(v)) {
            throw std::invalid_argument{"a number expected"};
        }
        s.contact_hold_s = std::clamp(v, 1.0f, 120.0f);
    } else {
        return false;
    }
    return true;
}

inline bool ParsePoseSetting(const std::string& key, const std::string& value) {
    return ParsePoseSetting(GetPoseSettings(), key, value);
}

// The defaults block written into a new vr.ini.
inline void WritePoseDefaults(std::ostream& out) {
    const PoseSettings d{};
    out << "# ---- Physical gestures (your pose fires the gesture's event; no animation) ----\n"
        << "# Make Contact at the fallen Brain of Mensis, and the Doll's reactions in the Hunter's "
           "Dream (0 = off)\n"
        << "pose_gestures=" << (d.enabled ? 1 : 0) << "\n"
        << "# 0 = only gestures you have learned count (as with the pad); 1 = every gesture\n"
        << "pose_gestures_all=" << (d.all ? 1 : 0) << "\n"
        << "# seconds of Make Contact (both hands up over your head) before the Brain answers "
           "(the game's own: ~50)\n"
        << "pose_contact_hold_s=" << d.contact_hold_s << "\n";
}

// ---- The game's facts ----

constexpr std::int32_t MarkerRally = 161;   // Hunter's Rally, Approval, Beg for Life, Make Contact
constexpr std::int32_t MarkerPoint = 162;   // Point Forward, Wave, Conviction, Joy, ...
constexpr std::int32_t MarkerBow = 163;     // Hunter's Salutation, the bows, Respect, ...
constexpr std::int32_t MarkerContact = 164; // Make Contact's arms-shifted phase (the Brain)

constexpr std::uint32_t FlagBrainFallen = 12600027; // event 12600027 enabled the fallen Brain
constexpr std::uint32_t FlagDollObserve = 12100118; // the Doll's AI: you stand in front of her

enum class Zone : std::uint8_t { None, Brain, Doll };

inline const char* ZoneLabel(Zone z) {
    switch (z) {
    case Zone::Brain:
        return "the Brain of Mensis";
    case Zone::Doll:
        return "the Doll";
    default:
        return "none";
    }
}

enum class Kind : std::uint8_t { None, MakeContact, Rally, Point, Bow };
constexpr int KindCount = 5;

inline const char* KindName(Kind k) {
    switch (k) {
    case Kind::MakeContact:
        return "Make Contact";
    case Kind::Rally:
        return "Rally";
    case Kind::Point:
        return "Point / Wave";
    case Kind::Bow:
        return "Bow";
    default:
        return "none";
    }
}

constexpr std::uint32_t KindBit(Kind k) {
    return 1u << static_cast<int>(k);
}

// Gesture index = goods id - 799 (1..23, the game's table; the menu's order).
constexpr std::uint32_t G(int idx) {
    return 1u << idx;
}

// The game's gestures a pose stands for: it counts when any of them is learned.
constexpr std::uint32_t GesturesOf(Kind k) {
    switch (k) {
    case Kind::MakeContact:
        return G(21); // Make Contact
    case Kind::Rally:
        return G(13) | G(15); // Hunter's Rally, Approval
    case Kind::Point:
        return G(1) | G(9); // Point Forward, Wave
    case Kind::Bow:
        // Hunter's Salutation, Church Bow (Male), Curtsy, Church Bow (Female), Respect, Deep Respect
        return G(2) | G(6) | G(7) | G(8) | G(19) | G(20);
    default:
        return 0;
    }
}

// The Doll's marker for a pose (the category of the gestures it stands for).
constexpr std::int32_t MarkerOf(Kind k) {
    switch (k) {
    case Kind::MakeContact:
    case Kind::Rally:
        return MarkerRally;
    case Kind::Point:
        return MarkerPoint;
    case Kind::Bow:
        return MarkerBow;
    default:
        return -1;
    }
}

// The learned list (GestureGameData, [PlayerGameData + 0x5B8]: 30 u32 at +0x10): entry k =
// (k << 16) | (idx << 1) | learned. Unused entries are 0xFFFFxxxx or have idx bits 0x7FFF. Returns
// the learned gestures as G(idx) bits.
inline std::uint32_t LearnedMask(const std::uint32_t* entries, int count) {
    std::uint32_t mask = 0;
    for (int k = 0; k < count; ++k) {
        const std::uint32_t v = entries[k];
        if (v > 0xFFFEFFFFu || (v & 0xFFFEu) == 0xFFFEu) {
            continue;
        }
        const int idx = static_cast<std::int16_t>(v & 0xFFFFu) >> 1;
        if (idx >= 1 && idx <= 23 && (v & 1u) != 0) {
            mask |= G(idx);
        }
    }
    return mask;
}

// Does a pose count (its gesture learned, or pose_gestures_all)?
inline bool Available(Kind k, std::uint32_t learned, bool all) {
    return k != Kind::None && (all || (GesturesOf(k) & learned) != 0);
}

// The poses that count, as KindBit bits.
inline std::uint32_t AvailableKinds(std::uint32_t learned, bool all) {
    std::uint32_t bits = 0;
    for (int i = 1; i < KindCount; ++i) {
        if (Available(static_cast<Kind>(i), learned, all)) {
            bits |= KindBit(static_cast<Kind>(i));
        }
    }
    return bits;
}

struct V3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
};

inline V3 Sub(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float Len(V3 a) {
    return std::sqrt(Dot(a, a));
}

// The Brain's region 2602022 (m26_00 coordinates: the player's position there is in the same
// frame). FromSoft cylinders stand on their position; the vertical band also covers a centred one.
constexpr V3 BrainCentre{95.48f, 943.53f, -31.81f};
constexpr float BrainRadius = 10.0f;
constexpr float BrainHeight = 5.0f;

inline float BrainHorizontal(V3 p) {
    return std::hypot(p.x - BrainCentre.x, p.z - BrainCentre.z);
}

// Inside the game's region (a little inside its rim: the game's own test decides at the marker).
inline bool InBrainRegion(V3 p) {
    return BrainHorizontal(p) <= BrainRadius - 0.25f && p.y >= BrainCentre.y - 0.5f * BrainHeight &&
           p.y <= BrainCentre.y + BrainHeight;
}

// Near enough to run the recogniser (a margin around the region, so the pose can start outside).
inline bool NearBrain(V3 p) {
    return BrainHorizontal(p) <= BrainRadius + 2.0f && p.y >= BrainCentre.y - 4.0f &&
           p.y <= BrainCentre.y + BrainHeight + 2.0f;
}

// ---- The pose classifier (LOCAL tracking space, metres, y up) ----

struct Frame {
    bool head_ok{false};
    V3 head{};               // the eyes' centre
    V3 forward{0, 0, -1};    // the HMD's forward (unit)
    bool left_ok{false};     // tracked position
    bool right_ok{false};
    V3 left{}, right{};      // the grips
    V3 left_v{}, right_v{};  // their linear velocities (m/s)
};

struct Config {
    float above_m{0.06f};    // a hand this far over the eyes is raised over the head
    float apart_m{0.40f};    // Make Contact: the raised hands at least this far apart
    float low_m{0.35f};      // a hand this far under the eyes is low (under the chest)
    float shoulder_m{0.35f}; // Point: the hand no lower than this under the eyes ...
    float forward_m{0.30f};  // ... and this far in front of them
    float bow_deg{30.0f};    // Bow: the head this far below level
    float bow_max_deg{80.0f};// ... and no further (looking at your feet is no bow)
    float still_mps{0.35f};  // the still poses (all but Point / Wave) settle with the hands this slow
    float settle_s{0.4f};    // a pose is recognised after this long
    float grace_s{0.4f};     // a recognised pose survives this long of not matching (tracking blips)
};

// A hand in the head's frame: up over the eyes, forward along the head's yaw, and sideways.
struct Local {
    float up{0.0f};
    float fwd{0.0f};
    float side{0.0f};
};

inline Local ToHead(const Frame& f, V3 hand) {
    float fx = f.forward.x;
    float fz = f.forward.z;
    const float n = std::hypot(fx, fz);
    if (n < 1e-4f) {
        fx = 0.0f; // straight up or down: no yaw to read
        fz = -1.0f;
    } else {
        fx /= n;
        fz /= n;
    }
    const V3 d = Sub(hand, f.head);
    // right = forward x up = (-fz, 0, fx)
    return {d.y, d.x * fx + d.z * fz, -d.x * fz + d.z * fx};
}

// The head's pitch in degrees (+ up, - down).
inline float HeadPitchDeg(const Frame& f) {
    const float n = Len(f.forward);
    if (n < 1e-6f) {
        return 0.0f;
    }
    return std::asin(std::clamp(f.forward.y / n, -1.0f, 1.0f)) * 57.29578f;
}

// The pose the frame shows now (no timing), most specific first. In the Brain's zone only Make
// Contact counts.
inline Kind Classify(const Frame& f, const Config& c, Zone zone) {
    if (zone == Zone::None || !f.head_ok || !f.left_ok || !f.right_ok) {
        return Kind::None;
    }
    const Local l = ToHead(f, f.left);
    const Local r = ToHead(f, f.right);
    const bool l_up = l.up > c.above_m;
    const bool r_up = r.up > c.above_m;
    if (l_up && r_up && Len(Sub(f.left, f.right)) > c.apart_m) {
        return Kind::MakeContact;
    }
    if (zone == Zone::Brain) {
        return Kind::None;
    }
    const bool l_low = l.up < -c.low_m;
    const bool r_low = r.up < -c.low_m;
    if ((l_up && r_low) || (r_up && l_low)) {
        return Kind::Rally;
    }
    const auto pointing = [&c](const Local& h) {
        return h.up >= -c.shoulder_m && h.up <= c.above_m && h.fwd > c.forward_m;
    };
    if ((pointing(r) && l_low) || (pointing(l) && r_low)) {
        return Kind::Point;
    }
    const float pitch = HeadPitchDeg(f);
    if (l_low && r_low && pitch < -c.bow_deg && pitch > -c.bow_max_deg) {
        return Kind::Bow;
    }
    return Kind::None;
}

inline bool NeedsStill(Kind k) {
    return k != Kind::Point; // a wave moves
}

inline bool HandsStill(const Frame& f, const Config& c) {
    return Len(f.left_v) < c.still_mps && Len(f.right_v) < c.still_mps;
}

// ---- The recogniser (the presenter's frame thread) ----

struct Output {
    Kind pose{Kind::None}; // the recognised pose (None: none)
    float held_s{0.0f};    // how long it has been held (from its first matching frame)
    std::uint32_t serial{0}; // +1 for every recognition (a new hold)
    bool recognized_now{false}; // recognised this frame (the tick)
    Kind released{Kind::None};  // released this frame ...
    float released_held_s{0.0f}; // ... after this long
};

class Recognizer {
public:
    void Reset() {
        const std::uint32_t serial = out_.serial;
        *this = {};
        out_.serial = serial;
    }

    // `available`: the poses that count (AvailableKinds); others are as no pose.
    const Output& Update(const Frame& f, const Config& c, Zone zone, std::uint32_t available,
                         float dt) {
        dt = std::clamp(dt, 0.0f, 0.1f);
        out_.recognized_now = false;
        out_.released = Kind::None;
        out_.released_held_s = 0.0f;
        Kind k = Classify(f, c, zone);
        if (k != Kind::None && (available & KindBit(k)) == 0) {
            k = Kind::None;
        }
        if (out_.pose != Kind::None) {
            out_.held_s += dt;
            if (k == out_.pose) {
                miss_s_ = 0.0f;
                return out_;
            }
            miss_s_ += dt;
            if (miss_s_ <= c.grace_s && zone != Zone::None) {
                return out_;
            }
            out_.released = out_.pose;
            out_.released_held_s = out_.held_s;
            out_.pose = Kind::None;
            out_.held_s = 0.0f;
            miss_s_ = 0.0f;
            candidate_ = Kind::None;
        }
        if (k == Kind::None || k != candidate_) {
            candidate_ = k;
            candidate_s_ = 0.0f;
            still_s_ = 0.0f;
            return out_;
        }
        candidate_s_ += dt;
        still_s_ = NeedsStill(k) && !HandsStill(f, c) ? 0.0f : still_s_ + dt;
        if (candidate_s_ >= c.settle_s && still_s_ >= c.settle_s) {
            out_.pose = k;
            out_.held_s = candidate_s_;
            out_.recognized_now = true;
            ++out_.serial;
            candidate_ = Kind::None;
        }
        return out_;
    }

    const Output& Last() const {
        return out_;
    }

private:
    Output out_{};
    Kind candidate_{Kind::None};
    float candidate_s_{0.0f};
    float still_s_{0.0f};
    float miss_s_{0.0f};
};

// ---- The game side (the main player's update) ----

// What may stop a marker (nullptr: nothing). Never in a menu, a cinematic, on a ladder (an event
// animation), dead, attacking, rolling or as a guest; the hunter idle or walking.
struct GateInput {
    bool host{false};       // chrType 0 (no phantom: the Doll needs the host)
    bool alive{false};      // HP > 0
    bool free{false};       // the newest animation is idle or locomotion
    bool menu{false};       // an in-game menu is open
    bool cinematic{false};  // a cinematic / scripted animation is shown
};

inline const char* GateBlock(const GateInput& g) {
    if (!g.host) {
        return "not the host";
    }
    if (!g.alive) {
        return "dead";
    }
    if (g.menu) {
        return "a menu is open";
    }
    if (g.cinematic) {
        return "a cinematic";
    }
    if (!g.free) {
        return "busy (attacking, rolling, a ladder, an item, ...)";
    }
    return nullptr;
}

constexpr float DollHoldS = 0.5f;     // a pose held this long reaches the Doll
// Then nothing for this long: her answer must be over before the next marker. Her event (12100117)
// waits for her SpEffect 160 (gone while she answers) with the markers' condition groups, and a
// group that passed while she was busy is remembered: the next answer would be that older pose's
// (163 before 162 before 161). Measured on the mock: 7000 (162) and 7026 + 7025 (163) are over
// within 3 s; 7001 (161) takes 5-6 s (a 162 5.0 s after it was taken for the next pose's answer).
constexpr float DollCooldownS = 5.0f;
constexpr float DollCooldownRallyS = 8.0f; // after 161 (her 7001)

struct TriggerInput {
    Zone zone{Zone::None};
    bool in_region{false}; // the Brain: inside the game's region now
    Kind pose{Kind::None}; // the presenter's recognised pose
    float held_s{0.0f};
    std::uint32_t serial{0}; // its hold's serial
    const char* block{nullptr}; // GateBlock
    std::uint32_t learned{0};
    bool all{false};
    float contact_hold_s{10.0f};
    double now_s{0.0};
};

struct TriggerOutput {
    int count{0};
    std::int32_t markers[2]{-1, -1};
    const char* waiting{nullptr}; // why a held pose fires nothing (yet): for the log
};

// Once per hold: the Brain applies 164 + 161 when Make Contact has been held for the hold time
// inside the region; the Doll applies the pose's category after DollHoldS, then a cooldown.
class Trigger {
public:
    TriggerOutput Step(const TriggerInput& in) {
        TriggerOutput out{};
        if (in.zone == Zone::None || in.pose == Kind::None || in.serial == fired_serial_) {
            return out;
        }
        if (!Available(in.pose, in.learned, in.all)) {
            out.waiting = "not learned";
            return out;
        }
        if (in.block != nullptr) {
            out.waiting = in.block;
            return out;
        }
        if (in.zone == Zone::Brain) {
            if (in.pose != Kind::MakeContact) {
                return out;
            }
            if (!in.in_region) {
                out.waiting = "outside the Brain's region";
                return out;
            }
            if (in.held_s < in.contact_hold_s) {
                out.waiting = "holding";
                return out;
            }
            fired_serial_ = in.serial;
            out.count = 2;
            out.markers[0] = MarkerContact;
            out.markers[1] = MarkerRally;
            return out;
        }
        if (in.held_s < DollHoldS) {
            out.waiting = "holding";
            return out;
        }
        const float cooldown = last_doll_marker_ == MarkerRally ? DollCooldownRallyS : DollCooldownS;
        if (in.now_s - last_doll_s_ < cooldown) {
            out.waiting = "cooldown";
            return out;
        }
        fired_serial_ = in.serial;
        last_doll_s_ = in.now_s;
        last_doll_marker_ = MarkerOf(in.pose);
        out.count = 1;
        out.markers[0] = last_doll_marker_;
        return out;
    }

private:
    std::uint32_t fired_serial_{0};
    double last_doll_s_{-1e9};
    std::int32_t last_doll_marker_{-1};
};

} // namespace VR::PoseGesture
