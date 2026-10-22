// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>

// Simon's Bowblade as a physical bow: carry the game's native arrow by its nock,
// squeeze at the bow to latch, pull to draw, and release to shoot. Charge is local:
// no game attack is held while drawing, including the animation fallback. Tracking
// loss preserves the nocked arrow; only releasing the trigger may loose it. Letting
// the string down below min_draw cancels the charge and puts the arrow back.
// The native held arrow is positioned by bow_pose.h; the game fires its projectile.
//
// Pure (no emulator dependencies; tests/test_bow.cpp). OpenXR LOCAL space (+x right, +y up, +z
// back, metres).
namespace VR::Bow {

struct Settings {
    bool enabled{true};
    float draw_m{0.60f};    // the hands this far apart: full draw
    float rest_m{0.12f};    // ... this close: no draw (the string at rest)
    // the string hand this close to the arrow rest (bow_pose.h ArrowRestOfGrip) as it squeezes:
    // nocked. A braced string sits about 20 cm off the grip, so 27 cm leaves room round it.
    float nock_m{0.27f};
    float min_draw{0.15f};  // a release below this draw puts the arrow back (no shot)
    float full_draw{0.85f}; // begin charging while at or above this draw
    float charge_s{0.6f};   // ... held this long at it: the charged shot
    // bow_arrow_pitch_deg: the arrow carried in the string hand points this far below the grip's
    // blade axis (a sword's line, 45 degrees above where a Touch controller points): forward.
    float arrow_pitch_deg{45.0f};
    bool quiver{true};      // bow_quiver: arrows from over the shoulder (else at the bow)
    bool haptics{true};
    // The arrow leaves at the release, fired by the game's update with no shot clip
    // (core/vr/bloodborne_gun.cpp BladeShotTick), with the bow's own release sound (`twang`,
    // learned from the game's own shot clips, or the release whoosh until known);
    // 0: the game's R1 / R2 shot clips (the arrow at their fire event).
    bool direct{true};
    std::int32_t twang{-1}; // bow_twang: the release sound's id (-1: not learned yet)
};

inline Settings& GetBowSettings() {
    static Settings settings;
    return settings;
}

inline bool ParseBowSetting(const std::string& key, const std::string& value) {
    auto& s = GetBowSettings();
    const auto on = [&value] { return value == "1" || value == "true"; };
    if (key == "bow") {
        s.enabled = on();
    } else if (key == "bow_draw_cm") {
        s.draw_m = std::clamp(std::stof(value), 30.0f, 90.0f) * 0.01f;
    } else if (key == "bow_nock_cm") {
        s.nock_m = std::clamp(std::stof(value), 5.0f, 40.0f) * 0.01f;
    } else if (key == "bow_min_draw") {
        s.min_draw = std::clamp(std::stof(value), 0.0f, 0.8f);
    } else if (key == "bow_full_draw") {
        s.full_draw = std::clamp(std::stof(value), 0.5f, 1.0f);
    } else if (key == "bow_arrow_pitch_deg") {
        s.arrow_pitch_deg = std::clamp(std::stof(value), 0.0f, 80.0f);
    } else if (key == "bow_charge_ms") {
        s.charge_s = std::clamp(std::stof(value), 0.0f, 3000.0f) * 0.001f;
    } else if (key == "bow_quiver") {
        s.quiver = on();
    } else if (key == "bow_haptics") {
        s.haptics = on();
    } else if (key == "bow_direct") {
        s.direct = on();
    } else if (key == "bow_twang") {
        s.twang = std::stoi(value);
    } else {
        return false;
    }
    return true;
}

inline void WriteBowDefaults(std::ostream& out) {
    const Settings d{};
    out << "# ---- Simon's Bowblade's bow (its bow form) ----\n"
        << "# 1 = a real bow: squeeze the string hand's trigger at the bow to nock, pull the hands "
           "apart to draw,\n# let go to shoot (a full draw: the charged shot); 0 = the game's R1 / "
           "R2 shots as before\n"
        << "bow=" << (d.enabled ? 1 : 0) << "\n"
        << "# the hands this far apart (cm): full draw\n"
        << "bow_draw_cm=" << d.draw_m * 100.0f << "\n"
        << "# the string hand this close to the arrow rest on the bow hand (cm) as it squeezes: "
           "nocked\n"
        << "bow_nock_cm=" << d.nock_m * 100.0f << "\n"
        << "# a release below this draw (0..1) puts the arrow back; full draw begins charging\n"
        << "bow_min_draw=" << d.min_draw << "\n"
        << "bow_full_draw=" << d.full_draw << "\n"
        << "# the full draw held this long (ms) charges the shot (the triple buzz)\n"
        << "bow_charge_ms=" << d.charge_s * 1000.0f << "\n"
        << "# the arrow in the string hand points this far (deg) below the grip's sword line "
           "(forward)\n"
        << "bow_arrow_pitch_deg=" << d.arrow_pitch_deg << "\n"
        << "# 1 = the bow comes out with an arrow in the string hand; the next ones from over "
           "your shoulder\n# (squeeze the string hand's trigger there), then bring it to the "
           "bow; 0 = squeeze the\n# trigger at the bow\n"
        << "bow_quiver=" << (d.quiver ? 1 : 0) << "\n"
        << "# the nock click, the string's tension, the ticks of the draw, the release's snap\n"
        << "bow_haptics=" << (d.haptics ? 1 : 0) << "\n"
        << "# 1 = the arrow leaves the moment you let go (no shot animation); 0 = the game's own "
           "shots\n"
        << "bow_direct=" << (d.direct ? 1 : 0) << "\n"
        << "# the bow's release sound, learned from game shots (-1 = the release whoosh "
           "until known)\n"
        << "bow_twang=" << d.twang << "\n";
}

// The quiver (bow_quiver): the string hand over a shoulder, from just behind the ears (head-
// relative, m: `back` behind the eyes along the body's forward, `up` above them, `side` to the
// right), so a cheek anchor at full draw (in front of the ears) never takes the next arrow. Made
// roomier and nearer for an easy reach (2026-10-13: from 7 cm behind the eyes, was 12; down to
// 40 cm below them, was 30; 50 cm to either side, was 45; 2026-10-14, the tester's "+2 cm": 42 cm
// below and 52 to either side; not nearer than 7 cm behind the eyes, the cheek anchor's 5).
constexpr bool AtQuiver(float back, float up, float side) {
    return back <= -0.07f && up >= -0.42f && side >= -0.52f && side <= 0.52f;
}

struct V3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
};

inline V3 Sub(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float Length(V3 a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

struct Input {
    float dt{0.0f};
    bool valid{false}; // both hands tracked
    V3 bow{};          // the bow hand's grip
    V3 string{};       // the string hand's grip
    bool pinch{false}; // the string hand's trigger
    bool at_quiver{false}; // the string hand over a shoulder (the presenter's: the quiver)
    bool pinch_valid{true}; // an inactive action is not a trigger release
};

// Holding: an arrow in the string hand (bow_quiver), not on the string yet: the one ready as the
// bow comes out, or one from the quiver. It stays there until it is shot.
enum class Phase : std::uint8_t { Idle, Holding, Nocked };

enum class Event : std::uint8_t {
    None,
    Take,    // an arrow from the quiver (bow_quiver)
    Empty,   // a squeeze at the bow with no arrow in hand (bow_quiver)
    Nock,    // an arrow on the string
    Full,    // the draw is charged locally; nothing fires until release
    Quick,   // released: the quick shot (R1)
    Charged, // trigger released after the full draw was held: the charged shot
    PutBack, // released without enough draw: no shot
};

inline const char* EventName(Event e) {
    switch (e) {
    case Event::None:
        return "none";
    case Event::Take:
        return "an arrow from the quiver";
    case Event::Empty:
        return "no arrow in hand";
    case Event::Nock:
        return "nock";
    case Event::Full:
        return "full draw";
    case Event::Quick:
        return "quick shot";
    case Event::Charged:
        return "charged shot";
    case Event::PutBack:
        return "put back";
    }
    return "?";
}

struct Output {
    Phase phase{Phase::Idle};
    Event event{Event::None};
    float draw{0.0f};      // 0 .. 1
    bool charged{false};   // local charge only; never a held game attack
    bool detent{false};    // the draw passed a new third this frame
    bool wall{false};      // ... reached full draw this frame (the string can go no further)
    V3 aim{0.0f, 0.0f, -1.0f}; // a shot's line (string -> bow, unit), before the release
    float aim_draw{0.0f};      // ... and its draw
};

class Bow {
public:
    Phase State() const {
        return phase;
    }

    static constexpr float NockGraceS = 0.3f; // a squeeze this recent nocks as the hand arrives

    // Back to the start, the string hand's trigger as it is now: a trigger already held is no
    // squeeze (the bow coming up with it held, a menu closed over it). `arrow`: one in the string
    // hand, ready (the bow coming out, bow_quiver).
    void Arm(bool pinch_now, bool arrow = false) {
        Reset();
        if (arrow) {
            phase = Phase::Holding;
        }
        prev_pinch = pinch_now;
        squeeze_s = NoSqueezeS;
    }

    // The string at rest (m): where the arrow was nocked, or rest_m (NockPoint's, the draw's).
    float RestM(const Settings& s) const {
        return std::max(s.rest_m, nock_rest);
    }

    void Reset() {
        phase = Phase::Idle;
        nock_rest = 0.0f;
        charged = false;
        last_draw = 0.0f;
        full_s = 0.0f;
        third = 0;
        head = 0;
        count = 0;
    }

    Output Update(const Input& in, const Settings& s) {
        Output out{};
        if (!in.pinch_valid) {
            out.phase = phase;
            out.draw = last_draw;
            out.charged = charged;
            return out;
        }
        // How long the trigger has been squeezed (none: let go, or held from before Arm).
        squeeze_s = !in.pinch                ? NoSqueezeS
                    : !prev_pinch            ? 0.0f
                    : squeeze_s < NoSqueezeS ? squeeze_s + in.dt
                                             : squeeze_s;
        prev_pinch = in.pinch;
        const bool at_bow = in.valid && Length(Sub(in.bow, in.string)) <= s.nock_m;
        if (phase == Phase::Idle) {
            const bool squeeze = in.pinch && squeeze_s <= NockGraceS;
            if (squeeze && s.quiver && in.at_quiver) {
                phase = Phase::Holding;
                out.event = Event::Take;
            } else if (squeeze && at_bow && squeeze_s == 0.0f && s.quiver) {
                out.event = Event::Empty;
            } else if (squeeze && at_bow && !s.quiver) {
                Reset();
                phase = Phase::Nocked;
                nock_rest = Length(Sub(in.bow, in.string));
                out.event = Event::Nock;
            }
            out.phase = phase;
            return out;
        }
        if (phase == Phase::Holding) {
            // In the fingers until it is shot: nocked at the bow with the trigger squeezed (still
            // held from the quiver, or squeezed there; one held since before Arm is none).
            if (at_bow && in.pinch && squeeze_s < NoSqueezeS) {
                Reset();
                phase = Phase::Nocked;
                nock_rest = Length(Sub(in.bow, in.string));
                out.event = Event::Nock;
            }
            out.phase = phase;
            return out;
        }
        if (!in.valid) {
            // A missing hand never fires by timeout. A deliberate trigger release
            // uses the last tracked draw and direction.
            if (!in.pinch) {
                const Sample shot = Newest();
                out.aim = shot.dir;
                out.aim_draw = shot.draw;
                out.event = charged                       ? Event::Charged
                            : shot.draw >= s.min_draw ? Event::Quick
                                                      : Event::PutBack;
                Reset();
                KeepPutBack(out, s);
            } else {
                out.draw = last_draw;
                out.charged = charged;
            }
            out.phase = phase;
            return out;
        }
        const V3 d = Sub(in.bow, in.string);
        const float len = Length(d);
        // The string at rest where it was nocked (an arrow nocked at bow_nock_cm, let go there,
        // goes back), or rest_m.
        const float rest = std::max(s.rest_m, nock_rest);
        const float span = std::max(s.draw_m - rest, 0.05f);
        const float draw = std::clamp((len - rest) / span, 0.0f, 1.0f);
        last_draw = draw;
        if (in.pinch && len > 0.03f) {
            recent[head] = {{d.x / len, d.y / len, d.z / len}, draw};
            head = (head + 1) % recent.size();
            count = std::min<std::size_t>(count + 1, recent.size());
        }
        out.draw = draw;
        const int now_third = static_cast<int>(draw * 3.0f + 1e-4f);
        if (now_third > third) {
            out.detent = true;
            out.wall = now_third >= 3;
            third = now_third;
        }
        // Charge is ours even when the fire hook is unavailable. The fallback
        // presses its attack only after release, never at full draw.
        const bool was_charged = charged;
        if (in.pinch && draw < s.min_draw) {
            // Discard older high-energy samples when deliberately letting down.
            count = 0;
            head = 0;
            charged = false;
            full_s = 0.0f;
        }
        if (in.pinch && !charged) {
            const float threshold = s.full_draw - (full_s > 0.0f ? 0.04f : 0.0f);
            full_s = draw >= threshold ? full_s + in.dt : 0.0f;
            if (full_s >= s.charge_s && draw >= threshold) {
                charged = true;
                out.event = Event::Full;
            }
        }
        out.charged = charged;
        if (!in.pinch) {
            // The release: the line and the draw a few frames back (the release's flinch moves
            // the string hand; at 90 Hz the oldest of four is 44 ms before this frame).
            const Sample shot = count > 0 ? recent[count < recent.size() ? 0 : head]
                                          : Sample{{0.0f, 0.0f, -1.0f}, 0.0f};
            out.aim = shot.dir;
            out.aim_draw = shot.draw;
            out.event = out.aim_draw < s.min_draw ? Event::PutBack
                        : was_charged ? Event::Charged : Event::Quick;
            out.charged = false;
            Reset();
            KeepPutBack(out, s);
        }
        out.phase = phase;
        return out;
    }

private:
    // An arrow put back stays in the fingers (bow_quiver; without it, it came from nowhere).
    void KeepPutBack(Output& out, const Settings& s) {
        if (out.event == Event::PutBack && s.quiver) {
            phase = Phase::Holding;
        }
    }

    struct Sample {
        V3 dir{0.0f, 0.0f, -1.0f};
        float draw{0.0f};
    };
    Sample Newest() const {
        return count > 0 ? recent[(head + recent.size() - 1) % recent.size()]
                         : Sample{{0.0f, 0.0f, -1.0f}, last_draw};
    }
    static constexpr float NoSqueezeS = 1e6f;
    Phase phase{Phase::Idle};
    bool prev_pinch{false};
    float squeeze_s{NoSqueezeS};
    bool charged{false};
    float full_s{0.0f};
    float last_draw{0.0f};
    float nock_rest{0.0f}; // the hands' distance at the nock (m)
    int third{0};
    std::array<Sample, 4> recent{};
    std::size_t head{0};
    std::size_t count{0};
};

// The hand on the string while an arrow is nocked: on the line from the bow hand back toward the
// string hand, as far from the bow as the draw lets it (from `rest_m` to full draw, where
// Bow::Update's draw reaches 1): the controller may go past full draw or in toward the bow, the
// string (and the hand on it) cannot.
inline V3 NockPoint(V3 bow, V3 string, float rest_m, float draw_m) {
    const V3 d = Sub(string, bow);
    const float len = Length(d);
    if (!(len > 1e-4f)) {
        return string;
    }
    const float full = rest_m + std::max(draw_m - rest_m, 0.05f); // (Update's span)
    const float k = std::clamp(len, rest_m, full) / len;
    return {bow.x + d.x * k, bow.y + d.y * k, bow.z + d.z * k};
}

} // namespace VR::Bow
