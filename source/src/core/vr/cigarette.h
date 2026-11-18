// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace VR::Cigarette {
// Native goods row installed by the cigarette data patch. Never invoke UseGoods for it.
inline constexpr std::int32_t Goods = 9000;
// Its icon in the game's menus: the game's own icon 343, a sheet of yellowed paper (no goods,
// weapon or attire uses it). The inventory draws only some of the icons the game ships, as they
// ship: new icon files (9000, 4980; on disk or served from memory) and the unused 184 stayed blank,
// 31 and 343 showed, a served copy of 343 showed the original (the mock, 2026-11-04). Our own
// packet art is the VR card's and the held model's. The old patch scripts gave the row icon 9000.
inline constexpr std::int32_t Icon = 343, LegacyIcon = 9000;
// Our own packet art for our VR card: icon file 9000 served from memory at boot (only our code reads
// it; the game's menus never show it).
inline constexpr std::int32_t CardIcon = 9000;
// A puff: DwellNs at the lips, its smoke then (450 ms until the headset run 14, 2026-11-05: none
// would come; 380 until the tester, 2026-11-05: "the delay should be 0.52s after we put it in the
// mouth for the smoke to fire").
inline constexpr std::int64_t DwellNs = 520'000'000, SmokeNs = 1'800'000'000;
// How long the "received" card stays up (its icon and name before you), turning with the view over
// NoticeTurnS (left in the room, the headset run 14 walked through it pushing the chair: unseen).
inline constexpr std::int64_t NoticeNs = 3'500'000'000;
inline constexpr float NoticeTurnS = .3f;
// The mouth zone: the hand within ZoneM of the mouth point, a ball; the next drag once it went
// RearmM further from the mouth than the nearest it came since the last (2026-11-05:
// "put it away from the mouth by 5cm or more and then bring it back to smoke again"; 17 cm out
// before). (10 cm round until the tester, 2026-11-04: "make the mouth area a little bigger": 13 by
// 16; 18 by 22 cm after the headset run 14 had no puff, for other reasons, until the user,
// 2026-11-05: "we made the mouth be too far out from the head"; then "much more near the
// original", "it's ok if you make it a circle like before".)
inline constexpr float ZoneM = .105f, RearmM = .05f;
struct Point { float x{}, y{}, z{}; };
struct Puff { std::int64_t ns{}; Point origin{}, forward{}; std::uint32_t space{}; };
// A cigarette thrown away (2026-11-04: "throw it away when done (like a throwable but it
// shouldn't do damage or anything)"): where and how fast it left the hand, in the hand's space. It is
// only drawn: it flies, lands on the floor, lies there and is gone after TossNs; nothing is used or
// hit, and the packet gives another.
struct Toss { std::int64_t ns{}; Point origin{}, velocity{}; std::uint32_t space{}; };
inline constexpr std::int64_t TossNs = 4'000'000'000;
// The trigger let go throws it only once smoked, or held TossAfterNs: let go while still bringing
// it up from the fan is no throw (the headset run 14: drawn and swung up to the mouth, it was
// "thrown" at once). The hand opening (the grip that holds it let go) throws it at once.
inline constexpr std::int64_t TossAfterNs = 2'000'000'000;
// Kos Lights reached the inventory (bloodborne_givekit.cpp, the game thread): grants this run.
// The quick items show a new value (a thump on both hands, the card before you).
inline std::atomic<std::uint32_t> gift_serial{0};
// One puff at a time: a cigarette just taken is ready at once, the next drag after it was taken
// RearmM away from the mouth (from the nearest it came) and brought back within RearmM of there.
// Invalid tracking (or the game holding the arm) never completes a drag: it starts the dwell over,
// and does not unready it (the headset run 14: a moment of the game's arm left it needing a pull
// away first).
class Drag {
public:
    bool Update(std::int64_t now, bool valid, float distance) {
        if (held_ == 0 && now > 0) held_ = now;
        if (!valid || !std::isfinite(distance) || now <= 0 || now < last_) {
            since_ = 0; last_ = now; return false;
        }
        if (last_ > 0 && now - last_ > 150'000'000) since_ = 0;
        last_ = now;
        if (!armed_) {
            nearest_ = std::min(nearest_, distance);
            if (distance >= nearest_ + RearmM) armed_ = true;
        }
        // Back within RearmM of where it was (any of the zone for a cigarette just taken).
        if (distance > ZoneM || distance >= nearest_ + RearmM || !armed_) {
            since_ = 0;
            return false;
        }
        if (!since_) since_ = now;
        if (now - since_ < DwellNs || now - puff_ < SmokeNs) return false;
        puff_ = now; since_ = 0; armed_ = false; nearest_ = distance; ++puffs_;
        return true;
    }
    // A flick as the trigger lets go may throw it away: smoked once, or held TossAfterNs.
    bool Spent(std::int64_t now) const {
        return puffs_ > 0 || (held_ > 0 && now - held_ >= TossAfterNs);
    }
    void Reset() { *this = {}; }
private:
    std::int64_t since_{}, last_{}, puff_{}, held_{};
    float nearest_{std::numeric_limits<float>::infinity()}; // since the last drag: the nearest
    int puffs_{};
    bool armed_{true};
};
} // namespace VR::Cigarette
