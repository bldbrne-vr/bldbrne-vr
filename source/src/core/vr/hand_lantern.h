// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// The Hand Lantern carried (the tester's 2026-10-30 requests): the lantern on the hunter's hip is a
// light while it hangs there, a portable one while it is in your hand.
//
// The game: using the Hand Lantern (goods 1412) toggles SpEffect 2010 on the hunter; while it is
// on, his effect controller keeps effect 21052 (the lit lantern: model s10021 and its light) at
// the hip in its slot 102 (measured in the mock, 2026-10-30: +2010 / -2010 / +2010 over three
// uses, the slot-102 node with them). The use is the game's UseGoods at once (no animation).
//
// Carry: taken into the hand lit, the hip's lantern is put out (one use) and the light you carry
// follows the hand; the trigger or a shake flips the light you carry and the lantern goes back;
// put back (any way), the game's lantern is set to the light you carried (one use if it differs).
//
// The lantern hangs from its handle in the fist (Pendulum): its body always below the hand, it
// swings when the hand moves or the hunter turns, and settles. Drawn HeldScale bigger in the hand.
//
// Pure (no emulator dependencies; tests/test_hand_lantern.cpp).
namespace VR::HandLantern {

inline constexpr std::int32_t Goods = 1412;     // the Hand Lantern
inline constexpr std::int32_t LitEffect = 2010; // SpEffect while it is lit
// Put out: 2011 (0.5 s, 2010's category 100: it replaces 2010 and runs out). The item's own
// UseGoods only ever lights it; its animation's second use is what put it out.
inline constexpr std::int32_t OffEffect = 2011;
inline constexpr std::int32_t HipFfx = 21052;   // the lit lantern at the hip (its model and light)
inline constexpr float HeldScale = 2.0f;        // its size in the hand, over its tuned look
// The light in the hand: the game's own lit lantern (its light, soft and warm: the mock, 2026-10-30,
// +15 of 255 round the hand where the torch's flame 21051 gave +38 and 100021052 and its kin none)
// on the left palm's dummy (3, where the held item sits). Its lantern model is not drawn while it
// is carried (the renderer skips that one face set near the eye: ModelIndexCount); ours, hanging
// and swinging, is.
inline constexpr std::int32_t HandLightFfx = HipFfx;
inline constexpr std::int32_t HandLightDummy = 3;
inline constexpr std::uint32_t ModelIndexCount = 1266; // s10021.flver: one mesh, one face set
// ... drawn by the effects' model shader (the draw inspector, 2026-10-30: the frame's only
// 1266-index draw, alpha-blended, the lantern's 256x256 textures). Its transform is camera-
// relative, so the match is by shader, not by distance.
inline constexpr std::uint64_t ModelVsHash = 0x1eb3a53d308;
// ... and its glow card (a 256x256 billboard, 8 vertices, non-indexed): without the lantern in
// front of it, a bright orb at the fist. Its shaders are the effects' shared ones (the game's
// shader archive: GXFfxtexture.vpo with GXFfxtextureGlowD.ppo), so other effects' glows may
// match it: hidden right after the model (4,007 of its 4,018 draws in the mock trace,
// 2026-10-30), or alone within GlowAloneNs after the light in the hand went out (the effect's
// last frame, 28 to 30 ms after, draws the card without the model; the other 3 were the lit
// lantern coming into view, the model culled first) and only within GlowNearM of where the
// lantern's own card (after its model) was drawn less than GlowAloneNs before, by its vertices'
// centre (the game's world coordinates, metres), so another effect's glow is never taken for it.
// The mock trace, 2026-10-30: all 11 switch-off cards at their lantern card's very place, 8 to
// 10 ms after it; GlowNearM leaves room for a sprint and a swing over a slow game frame.
inline constexpr std::uint64_t GlowVsHash = 0x4d8ebd0b8eb;
inline constexpr std::uint64_t GlowFsHash = 0xfe15fbb64eb;
inline constexpr std::uint32_t GlowVertexCount = 8;
inline constexpr std::int64_t GlowAloneNs = 150'000'000;
inline constexpr float GlowNearM = 0.5f;

// The light you carry, and the game uses that keep the hip's lantern right. Each call returns
// whether one use of the Hand Lantern (a toggle) has to go to the game now.
struct Carry {
    bool active{false}; // the lantern is in the hand
    bool lit{false};    // ... and carries a light

    // Into the hand: the light comes with it; a lit hip lantern goes out.
    [[nodiscard]] bool Take(bool game_lit) {
        active = true;
        lit = game_lit;
        return game_lit;
    }
    // The trigger or a shake (a use asked while carried): the light you carry flips, and the game
    // is not touched (its lantern is out while you carry it).
    void Flip() {
        if (active) {
            lit = !lit;
        }
    }
    // Back on the hip: the game's lantern set to the light you carried.
    [[nodiscard]] bool PutBack(bool game_lit) {
        if (!active) {
            return false;
        }
        active = false;
        return lit != game_lit;
    }
    [[nodiscard]] bool Light() const {
        return active && lit;
    }
};

// A use owed to the game (Carry's Take / PutBack answers): the hip's lantern to be lit or put
// out, sent once the game may use an item. A refused send stays owed and is asked again RetryNs
// later (it was dropped: the hip's lantern left wrong, review of 2026-10-30); seen already so,
// it is no longer owed.
struct Owed {
    static constexpr std::int64_t RetryNs = 250'000'000;
    bool owed{false};
    bool target{false};         // the hip's lantern lit (or out)
    std::int64_t retry_ns{0};   // refused: not asked again before this
    std::uint32_t refused{0};   // refusals in a row

    void Owe(bool now_owed, bool lit) {
        owed = now_owed;
        target = lit;
        retry_ns = 0;
        refused = 0;
    }
    // Whether to send it now (`game_lit`: the hip's lantern as it is).
    [[nodiscard]] bool Due(bool game_lit, std::int64_t now_ns) {
        if (!owed) {
            return false;
        }
        if (game_lit == target) {
            owed = false;
            return false;
        }
        return now_ns >= retry_ns;
    }
    // The send's outcome: no longer owed, or asked again RetryNs later.
    void Sent(bool ran, std::int64_t now_ns) {
        if (ran) {
            owed = false;
            refused = 0;
            return;
        }
        retry_ns = now_ns + RetryNs;
        ++refused;
    }
};

// The lantern hanging from the fist: a point mass on a rigid handle (length_m) below the pivot,
// integrated (Verlet) in the pivot's own frame: gravity, the swing's damping (against the pivot,
// not the world: a steady walk or run trails nothing) and the pivot's acceleration, pulling it
// the other way. The pivot's motion is the hand's own (within your play space: felt in full)
// and the body's (the play space carried through the world: the hunter running, the stick's
// turns). The body's velocity is smoothed over body_smooth_s, and what the lantern feels of it
// follows that at most body_accel: the game's instant starts, stops and turns sway it a little
// (2026-10-30: "when I run it swings way too much"), its uneven steps between updates
// cancel out, and a jump of the body (a warp, a snap turn) is carried unfelt. Step returns the
// up direction: from the lantern's body to its handle (world up at rest), unit.
struct Pendulum {
    struct Config {
        float length_m{0.13f};       // the handle's pivot to the lantern's centre
        float damping{2.2f};         // 1/s: a swing fades over about a second
        float max_tilt_deg{80.0f};   // never swings up past this from hanging straight down
        float body_accel{2.0f};      // m/s^2: the most of the body's acceleration it feels
        float body_smooth_s{0.1f};   // the body's velocity, smoothed over this
        float max_body_speed{25.0f}; // m/s: a faster step of the body is a jump, carried unfelt
        float warm_s{0.3f};          // after a restart the body's motion is learnt, unfelt
        float max_dt_s{0.05f};       // a longer gap starts it hanging again ...
        float max_jump_m{0.4f};      // ... and so does a jump of the hand within your play space
    };
    using V3 = std::array<float, 3>;
    V3 rel{};        // the lantern's centre from the pivot
    V3 rel_prev{};   // ... a step ago
    V3 last_pivot{};
    V3 hand_v{};     // the hand's own velocity, last step
    V3 body_v{};     // the body's velocity, smoothed
    V3 body_felt{};  // ... as the lantern feels it (following body_v at most body_accel)
    float warm{0.0f}; // seconds since the restart
    bool valid{false};
    bool learnt{false}; // the velocities are known (a step after the restart)
    double t{0.0};

    static float Dot(const V3& a, const V3& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    static V3 Sub(const V3& a, const V3& b) {
        return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    }
    static V3 Add(const V3& a, const V3& b) {
        return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
    }
    static V3 Scale(const V3& a, float k) {
        return {a[0] * k, a[1] * k, a[2] * k};
    }
    static V3 Unit(const V3& a, const V3& fallback) {
        const float n = std::sqrt(Dot(a, a));
        return n > 1e-6f && std::isfinite(n) ? Scale(a, 1.0f / n) : fallback;
    }

    void Reset() {
        valid = false;
    }

    // The pivot (the grip, world metres, +Y up) at time `now_s`, `body` of its motion since the
    // last step the body's (where a point of your play space at the pivot's last place went;
    // the rest is the hand's own): the new up direction.
    V3 Step(const Config& c, const V3& pivot, const V3& body, double now_s) {
        const V3 down{0.0f, -1.0f, 0.0f};
        const double dt_d = now_s - t;
        const V3 hand = Sub(Sub(pivot, last_pivot), body);
        last_pivot = pivot;
        t = now_s;
        if (!valid || !(dt_d > 0.0) || dt_d > c.max_dt_s ||
            Dot(hand, hand) > c.max_jump_m * c.max_jump_m) {
            rel = Scale(down, c.length_m);
            rel_prev = rel;
            hand_v = {};
            body_v = {};
            body_felt = {};
            warm = 0.0f;
            valid = true;
            learnt = false;
            return {0.0f, 1.0f, 0.0f};
        }
        const float dt = static_cast<float>(dt_d);
        const V3 hv = Scale(hand, 1.0f / dt);
        const V3 bv = Scale(body, 1.0f / dt);
        // The pivot's velocity change this step as felt (x dt: a displacement): the hand's in
        // full, the body's as body_felt changes (none while it is learnt).
        V3 kick{};
        if (!learnt) {
            body_v = bv;
            body_felt = bv;
            learnt = true;
        } else {
            kick = Scale(Sub(hv, hand_v), dt);
            if (Dot(bv, bv) <= c.max_body_speed * c.max_body_speed) {
                const float follow = 1.0f - std::exp(-dt / std::max(c.body_smooth_s, 1e-3f));
                body_v = Add(body_v, Scale(Sub(bv, body_v), follow));
            }
            V3 dv = Sub(body_v, body_felt);
            if (warm >= c.warm_s) {
                const float n = std::sqrt(Dot(dv, dv));
                const float cap = c.body_accel * dt;
                if (n > cap) {
                    dv = Scale(dv, cap / n);
                }
                kick = Add(kick, Scale(dv, dt));
            }
            body_felt = Add(body_felt, dv);
        }
        hand_v = hv;
        warm += dt;
        // Verlet in the pivot's frame: the swing kept (damped), gravity, the pivot's kick.
        const float keep = std::exp(-c.damping * dt);
        const V3 velocity = Scale(Sub(rel, rel_prev), keep);
        rel_prev = rel;
        rel = Sub(Add(Add(rel, velocity), Scale(down, 9.81f * dt * dt)), kick);
        // The handle is rigid: the bob on its sphere round the pivot, never swung up past
        // max_tilt_deg from hanging (the fist holds the handle).
        V3 hang = Unit(rel, down);
        const float min_cos = std::cos(c.max_tilt_deg * 3.14159265f / 180.0f);
        if (Dot(hang, down) < min_cos) {
            // Back onto the cone's edge, in the same vertical plane; the swing stops there.
            const V3 side = Unit({hang[0], 0.0f, hang[2]}, {1.0f, 0.0f, 0.0f});
            const float s = std::sqrt(std::max(0.0f, 1.0f - min_cos * min_cos));
            hang = Add(Scale(down, min_cos), Scale(side, s));
            rel_prev = Scale(hang, c.length_m);
        }
        rel = Scale(hang, c.length_m);
        return Scale(hang, -1.0f);
    }
};

} // namespace VR::HandLantern
