// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "core/vr/gun_reload.h"
#include "core/vr/gun_reload_audio.h"
#include "core/vr/gun_settings.h"
namespace {
using VR::Body::AxisAngle;
using VR::Gun::WristReload;
constexpr std::int64_t Start = 1'000'000'000;
constexpr std::int64_t Ms = 1'000'000;
// The wrist still at 130, 150 and 170 ms after the shot: one still sample arms the flick.
template <class Reload> void Arm(Reload& r) {
    r.Step(Start + 130 * Ms, {}, true, false);
    r.Step(Start + 150 * Ms, {}, true, false);
    r.Step(Start + 170 * Ms, {}, true, false);
}
TEST(WristReload, StartsLoadedAndFailedRequestsDoNotEmptyIt) {
    WristReload r;
    EXPECT_FALSE(r.Blocked()); r.Request(4, Start); EXPECT_TRUE(r.Blocked());
    r.Denied(3); EXPECT_TRUE(r.Pending()); r.Denied(4);
    EXPECT_FALSE(r.Empty()); EXPECT_FALSE(r.Blocked());
}
TEST(WristReload, UnacknowledgedVanillaRequestExpires) {
    WristReload r; r.Request(4, Start);
    r.Step(Start + 2'100 * Ms, {}, true, false);
    EXPECT_FALSE(r.Blocked());
}
TEST(WristReload, AllFourFlickDirectionsReloadExactlyOnce) {
    for (auto axis : {VR::Body::Vec3{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}}) {
        WristReload r; r.Shot(Start); Arm(r);
        EXPECT_FALSE(r.Step(Start + 190 * Ms, AxisAngle(axis, .25f), true, false));
        EXPECT_TRUE(r.Step(Start + 210 * Ms, AxisAngle(axis, .5f), true, false));
        EXPECT_FALSE(r.Empty());
        EXPECT_FALSE(r.Step(Start + 230 * Ms, AxisAngle(axis, .7f), true, false));
    }
}
// 2026-10-15 (the user): one still sample arms it again, as in 52f50698 (c0a59dad wanted 30, then
// 40 ms).
TEST(WristReload, OneStillSampleArms) {
    WristReload r; r.Shot(Start);
    r.Step(Start + 130 * Ms, {}, true, false);
    r.Step(Start + 150 * Ms, AxisAngle({1,0,0}, .01f), true, false); // still: one sample
    EXPECT_FALSE(r.Step(Start + 170 * Ms, AxisAngle({1,0,0}, .26f), true, false));
    EXPECT_TRUE(r.Step(Start + 190 * Ms, AxisAngle({1,0,0}, .51f), true, false));
    EXPECT_FALSE(r.Empty());
}
// ... and a wrist never still (every sample over 1.5 rad/s) never arms.
TEST(WristReload, AWristNeverStillNeverArms) {
    WristReload r; r.Shot(Start);
    for (int i = 0; i < 40; ++i)
        EXPECT_FALSE(r.Step(Start + 130 * Ms + i * 10 * Ms,
                            AxisAngle({1,0,0}, (i % 8 < 4 ? i % 4 : 4 - i % 4) * .02f), true,
                            false));
    EXPECT_TRUE(r.Empty());
}
// ... and the flick must turn 24 degrees (was 20).
TEST(WristReload, TwentyThreeDegreesIsNotEnoughTwentyFiveIs) {
    for (float turn : {23.f, 25.f}) {
        WristReload r; r.Shot(Start); Arm(r);
        const float radians = turn * VR::Body::Deg;
        r.Step(Start + 190 * Ms, AxisAngle({0,1,0}, radians * .5f), true, false);
        r.Step(Start + 210 * Ms, AxisAngle({0,1,0}, radians), true, false);
        r.Step(Start + 230 * Ms, AxisAngle({0,1,0}, radians), true, false);
        EXPECT_EQ(r.Empty(), turn < 24.f) << turn;
    }
}
// ... at a peak of 4.5 rad/s (was 4): 40 degrees evenly over 160 ms (4.4 rad/s) does not reload.
TEST(WristReload, AFourAndAHalfRadianPerSecondTurnDoesNotReload) {
    WristReload r; r.Shot(Start); Arm(r);
    for (int i = 1; i <= 8; ++i)
        EXPECT_FALSE(r.Step(Start + 170 * Ms + i * 20 * Ms,
                            AxisAngle({1,0,0}, i * 5.f * VR::Body::Deg), true, false));
    EXPECT_TRUE(r.Empty());
}
TEST(WristReload, SlowAimingNeverReloads) {
    WristReload r; r.Shot(Start); Arm(r);
    for (int i = 1; i < 100; ++i)
        EXPECT_FALSE(r.Step(Start + 170 * Ms + i * 20 * Ms,
                            AxisAngle({1,0,0}, i * .025f), true, false));
    EXPECT_TRUE(r.Empty());
}
TEST(WristReload, ShotMotionAndHeldTriggerCannotReload) {
    WristReload r; r.Shot(Start);
    for (int i = 1; i < 20; ++i)
        EXPECT_FALSE(r.Step(Start + i * 20 * Ms, AxisAngle({1,0,0}, i * .2f), true, i > 5));
    EXPECT_TRUE(r.Empty());
}
TEST(WristReload, TrackingLossAndLongPauseRequireFreshQuietWrist) {
    for (bool lost : {false, true}) {
        WristReload r; r.Shot(Start); Arm(r);
        EXPECT_FALSE(r.Step(Start + 190 * Ms, AxisAngle({1,0,0}, .25f), true, false));
        if (lost) r.Step(Start + 200 * Ms, {}, false, false);
        EXPECT_FALSE(r.Step(Start + (lost ? 210 : 900) * Ms, AxisAngle({1,0,0}, .6f), true, false));
        EXPECT_TRUE(r.Empty());
    }
}
TEST(WristReload, QuaternionSignChangeAndTrackingJumpDoNotReload) {
    WristReload r; r.Shot(Start); Arm(r);
    EXPECT_FALSE(r.Step(Start + 190 * Ms, {0,0,0,-1}, true, false));
    EXPECT_FALSE(r.Step(Start + 210 * Ms, AxisAngle({1,0,0}, 2), true, false));
    EXPECT_TRUE(r.Empty());
}
TEST(WristReload, InvalidQuaternionDoesNotReload) {
    WristReload r; r.Shot(Start); Arm(r);
    EXPECT_FALSE(r.Step(Start + 190 * Ms, {0,0,0,0}, true, false));
    EXPECT_FALSE(r.Step(Start + 210 * Ms, {0,0,0,std::numeric_limits<float>::quiet_NaN()},
                        true, false));
    EXPECT_TRUE(r.Empty()); r.Reset(); EXPECT_FALSE(r.Blocked());
}
TEST(WristReload, TinyFastOscillationsDoNotCountAsFullFlick) {
    WristReload r; r.Shot(Start); Arm(r);
    for (int i = 1; i < 9; ++i)
        EXPECT_FALSE(r.Step(Start + 170 * Ms + i * 20 * Ms,
                            AxisAngle({1,0,0}, i % 2 ? .1f : 0), true, false));
    EXPECT_TRUE(r.Empty());
}
TEST(WristReload, ImmediateTriggerAfterReloadIsAccepted) {
    WristReload r;
    r.Shot(Start); Arm(r);
    EXPECT_FALSE(r.Step(Start + 190 * Ms, AxisAngle({1,0,0}, .25f), true, false));
    ASSERT_TRUE(r.Step(Start + 210 * Ms, AxisAngle({1,0,0}, .5f), true, false));
    r.Step(Start + 221 * Ms, AxisAngle({1,0,0}, .5f), true, true);
    EXPECT_FALSE(r.RejectPull());
    EXPECT_FALSE(r.BlocksHeldTrigger());
}
TEST(WristReload, NewShotRequiresANewFlick) {
    WristReload r;
    r.Shot(Start); Arm(r);
    r.Step(Start + 190 * Ms, AxisAngle({1,0,0}, .25f), true, false);
    ASSERT_TRUE(r.Step(Start + 210 * Ms, AxisAngle({1,0,0}, .5f), true, false));
    const std::int64_t next = Start + 1'000 * Ms;
    r.Shot(next);
    EXPECT_TRUE(r.Empty());
    EXPECT_FALSE(r.Step(next + 130 * Ms, AxisAngle({1,0,0}, .5f), true, false));
    EXPECT_TRUE(r.Empty());
}

template <class Reload> void CheckFlickSlowdowns() {
    for (int pause_ms : {10, 30, 60, 200}) {
        Reload r; r.Shot(Start); Arm(r);
        EXPECT_FALSE(r.Step(Start+180*Ms, AxisAngle({1,0,0},.14f),true,false));
        for(int t=190;t<=180+pause_ms;t+=10)
            EXPECT_FALSE(r.Step(Start+t*Ms,AxisAngle({1,0,0},.14f),true,false));
        EXPECT_FALSE(r.Step(Start+(190+pause_ms)*Ms,AxisAngle({1,0,0},.30f),true,false));
        EXPECT_EQ(r.Step(Start+(200+pause_ms)*Ms,AxisAngle({1,0,0},.45f),true,false),pause_ms<180);
        EXPECT_EQ(r.Empty(),pause_ms>=180);
    }
}
TEST(WristReload, BriefSlowdownsPreserveFlickButDoNotExtendDeadline) {
    CheckFlickSlowdowns<WristReload>();
}
// 22 rad/s first (clear of the 25 rad/s tracking-jump limit), then 1.3 rad/s (under the 1.5 rad/s
// quiet speed) to 24.5 degrees at 340 ms, inside the 180 ms window.
TEST(WristReload, FastStartCanFinishBelowQuietSpeed) {
    WristReload r; r.Shot(Start); Arm(r);
    EXPECT_FALSE(r.Step(Start+180*Ms,AxisAngle({1,0,0},.22f),true,false));
    bool loaded=false;
    for(int i=1;i<=16;++i)
        loaded |= r.Step(Start+(180+i*10)*Ms,AxisAngle({1,0,0},.22f+i*.013f),true,false);
    EXPECT_TRUE(loaded);
}
// A twitch that never reaches the flick's 4.5 rad/s (1.3 degrees in one 90 Hz frame, 2 rad/s)
// is dropped once the wrist is still again (one sample): the flick re-arms from there, so a real
// 5 rad/s flick 100 ms later gets its own 180 ms window (it reaches 24 degrees at 370 ms, past
// the 350 ms deadline the twitch used to hold).
TEST(WristReload, TwitchThenStillDoesNotEatTheNextFlick) {
    for (auto axis : {VR::Body::Vec3{1,0,0}, {-1,0,0}, {0,1,0}, {0,0,-1}}) {
        WristReload r; r.Shot(Start); Arm(r); // armed at 170 ms
        const double frame_ms = 1000.0 / 90.0;
        const float twitch = 1.3f * VR::Body::Deg;
        int reloads = 0;
        double reloaded_ms = 0;
        for (int i = 1; 170 + i * frame_ms <= 500; ++i) {
            const double t = 170 + i * frame_ms; // 181.1 ms: the twitch; from 281 ms: the flick
            const float angle = twitch + static_cast<float>(std::max(0.0, (t - 281) * .005));
            const bool loaded = r.Step(Start + static_cast<std::int64_t>(t * Ms),
                                       AxisAngle(axis, angle), true, false);
            if (loaded && !reloads)
                reloaded_ms = t;
            reloads += loaded;
        }
        EXPECT_EQ(reloads, 1);
        EXPECT_GT(reloaded_ms, 350.0);
        EXPECT_LT(reloaded_ms, 380.0);
    }
}
// Minimum-jerk 0..1: a hand's smooth start and stop.
double Smooth(double s) {
    s = std::clamp(s, 0.0, 1.0);
    return s * s * s * (10 - 15 * s + 6 * s * s);
}
// A wind-up ("cock and flick") turns below the quiet speed before the flick reaches 4.5 rad/s:
// with one still sample arming (2026-10-15) the turn-around re-arms the flick there, so it is
// measured from the cocked wrist. At 90 Hz: 8 degrees back over 70 ms (peak 3.7 rad/s), then
// forward to +30 degrees over 110 ms: 24 degrees past the cocked wrist (+16) at 314 ms, before
// the 326 ms it took from the start.
TEST(WristReload, WindUpTurnKeepsTheFlick) {
    for (auto axis : {VR::Body::Vec3{1,0,0}, {-1,0,0}, {0,1,0}, {0,0,-1}}) {
        WristReload r; r.Shot(Start); Arm(r); // armed at 170 ms
        const double frame_ms = 1000.0 / 90.0;
        int reloads = 0;
        double reloaded_ms = 0;
        for (int i = 1; 170 + i * frame_ms <= 500; ++i) {
            const double t = 170 + i * frame_ms;
            const double degrees =
                t < 245 ? -8 * Smooth((t - 175) / 70) : -8 + 38 * Smooth((t - 245) / 110);
            const bool loaded = r.Step(Start + static_cast<std::int64_t>(t * Ms),
                                       AxisAngle(axis, static_cast<float>(degrees) *
                                                           VR::Body::Deg),
                                       true, false);
            if (loaded && !reloads)
                reloaded_ms = t;
            reloads += loaded;
        }
        EXPECT_EQ(reloads, 1);
        EXPECT_GT(reloaded_ms, 305.0);
        EXPECT_LT(reloaded_ms, 320.0);
    }
}
// A quick aim change that does reach 4.5 rad/s, then a still wrist: one still sample arms a second
// flick from the new aim, so a 5 rad/s flick back the other way from 281 ms gets its own 180 ms
// (at 90 Hz it turns 24 degrees at 370 ms, past the aim change's 350 ms deadline). Both a
// one-frame jump (4 degrees, 6.3 rad/s) and a smooth move (8 degrees over 40 ms, peak 6.5 rad/s).
TEST(WristReload, QuickAimChangeThenStillDoesNotEatTheNextFlick) {
    for (bool smooth : {false, true})
        for (auto axis : {VR::Body::Vec3{1,0,0}, {-1,0,0}, {0,1,0}, {0,0,-1}}) {
            WristReload r; r.Shot(Start); Arm(r); // armed at 170 ms
            const double frame_ms = 1000.0 / 90.0;
            int reloads = 0;
            double reloaded_ms = 0;
            for (int i = 1; 170 + i * frame_ms <= 500; ++i) {
                const double t = 170 + i * frame_ms; // 181.1 ms: the first moving sample
                const double aim = smooth ? 8 * Smooth((t - 175) / 40) : 4.0;
                const float angle = static_cast<float>(aim * VR::Body::Deg -
                                                       std::max(0.0, (t - 281) * .005));
                const bool loaded = r.Step(Start + static_cast<std::int64_t>(t * Ms),
                                           AxisAngle(axis, angle), true, false);
                if (loaded && !reloads)
                    reloaded_ms = t;
                reloads += loaded;
            }
            EXPECT_EQ(reloads, 1) << smooth;
            EXPECT_GT(reloaded_ms, 350.0) << smooth;
            EXPECT_LT(reloaded_ms, 380.0) << smooth;
        }
}
// One still sample (QuietNs 0), at any gap between the samples.
TEST(WristReload, OneStillSampleSettlesAtAnyFrameRate) {
    for(int quiet : {8,11,14,39,40}) {
        WristReload r; r.Shot(Start);
        r.Step(Start+130*Ms,{},true,false);
        r.Step(Start+(130+quiet)*Ms,{},true,false);
        EXPECT_FALSE(r.Step(Start+(150+quiet)*Ms,AxisAngle({1,0,0},.25f),true,false));
        EXPECT_TRUE(r.Step(Start+(170+quiet)*Ms,AxisAngle({1,0,0},.45f),true,false)) << quiet;
    }
}


TEST(WristReload, SlowdownFlickWorksAtHeadsetFrameRates) {
    for(float hz : {60.f,72.f,90.f,120.f})
        for(auto axis : {VR::Body::Vec3{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}}) {
            WristReload r; r.Shot(Start);
            int reloads=0;
            for(int i=0;i<int(hz);++i) {
                const float t=i/hz;
                const float angle=t<.38f ? std::max(0.f,(t-.35f)*8.f)
                    : t<.42f ? .24f : std::min(.45f,.24f+(t-.42f)*8.f);
                reloads += r.Step(Start+static_cast<int64_t>(double(i)*1e9/hz),
                                  AxisAngle(axis,angle),true,false);
            }
            EXPECT_EQ(reloads,1) << hz;
        }
}

TEST(WristReload, SettingParsesWritesAndDefaultsOn) {
    auto saved = VR::Gun::GetGunSettings();
    EXPECT_TRUE(VR::Gun::GunSettings{}.wrist_reload);
    EXPECT_TRUE(VR::Gun::ParseGunSetting("gun_wrist_reload", "1"));
    EXPECT_TRUE(VR::Gun::GetGunSettings().wrist_reload);
    std::ostringstream out;
    VR::Gun::WriteGunDefaults(out);
    EXPECT_NE(out.str().find("gun_wrist_reload=1"), std::string::npos);
    EXPECT_TRUE(VR::Gun::ParseGunSetting("gun_wrist_reload", "0"));
    EXPECT_FALSE(VR::Gun::GetGunSettings().wrist_reload);
    VR::Gun::GetGunSettings() = saved;
}
TEST(WristReloadAudio, OneShotDoesNotReplayAndStaleCueIsSilent) {
    const std::array<char, 8> pcm{0, 64, 0, 64, 0, 64, 0, 64};
    VR::Gun::ReloadAudioMixer mixer(pcm, pcm);
    VR::Gun::ReloadAudioFrame state{};
    state.reload = {Start, Start, 1, 0, -.25f, true};
    std::array<float, 16> buffer{};
    mixer.Mix(buffer.data(), 8, 2, 24000, true, state, Start);
    EXPECT_GT(buffer[0], 0);
    EXPECT_GT(buffer[1], 0);
    buffer.fill(0);
    mixer.Mix(buffer.data(), 8, 2, 24000, true, state, Start + 1);
    for (float x : buffer)
        EXPECT_EQ(x, 0);
    state.click = {Start, Start, 1, 0, 0, true};
    mixer.Mix(buffer.data(), 8, 2, 24000, true, state, Start + 200'000'000);
    for (float x : buffer)
        EXPECT_EQ(x, 0);
}
TEST(WristReloadAudio, DisabledCueDoesNotPlayAndMultichannelUsesFrontOnly) {
    const std::array<char, 8> pcm{0, 64, 0, 64, 0, 64, 0, 64};
    VR::Gun::ReloadAudioMixer mixer(pcm, pcm);
    VR::Gun::ReloadAudioFrame state{};
    state.click = {Start, Start, 1, 0, 0, false};
    std::array<std::int16_t, 64> buffer{};
    mixer.Mix(buffer.data(), 8, 8, 48000, false, state, Start);
    for (auto x : buffer)
        EXPECT_EQ(x, 0);
    state.click.enabled = true;
    ++state.click.clink_id;
    mixer.Mix(buffer.data(), 8, 8, 48000, false, state, Start);
    EXPECT_GT(buffer[0], 0);
    EXPECT_GT(buffer[1], 0);
    for (int f = 0; f < 8; ++f)
        for (int c = 2; c < 8; ++c)
            EXPECT_EQ(buffer[f * 8 + c], 0);
}
} // namespace

TEST(WristReload, RejectedPullCannotLeakIntoHeldVanillaL2) {
    VR::Gun::WristReload r;
    r.Request(1, Start);
    EXPECT_FALSE(r.BlocksHeldTrigger()); // initial vanilla press may finish its draw
    EXPECT_TRUE(r.RejectPull());         // another press while that shot is pending
    EXPECT_TRUE(r.BlocksHeldTrigger());
    r.Denied(1);
    EXPECT_TRUE(r.BlocksHeldTrigger()); // denial does not turn a held press into a new shot
    r.Step(Start + 20'000'000, {}, true, false);
    EXPECT_FALSE(r.BlocksHeldTrigger());
}

TEST(WristReload, ConfirmedRequestNeverExpiresAndUsesUpdatedFlick) {
    VR::Gun::ConfirmedWristReload r;r.Request(42,Start);
    r.Step(Start+2'100*Ms,{},true,false);EXPECT_TRUE(r.Pending());
    r.Denied(42);EXPECT_FALSE(r.Pending());r.Shot(Start);Arm(r);
    EXPECT_FALSE(r.Step(Start+190*Ms,AxisAngle({1,0,0},.25f),true,false));
    EXPECT_TRUE(r.Step(Start+210*Ms,AxisAngle({1,0,0},.5f),true,false));
}

TEST(WristReload, ConfirmedFlickToleratesSlowdowns) { CheckFlickSlowdowns<VR::Gun::ConfirmedWristReload>(); }
// ---- Near the body (2026-10-15, the user): a modest speed floor, the same turn ----------------

namespace {
using VR::Gun::NearBody;
using VR::Gun::ReloadAttempt;
using VR::Gun::ReloadEnd;

// A minimum-jerk turn of `degrees` over `seconds` (its peak: 1.875 x the mean speed) starting
// 200 ms after the shot from a still wrist, sampled at `hz`, plus an optional wobble of the hand
// (a fight: `wobble_deg` at 3 Hz about another axis); `near_at(i)` per frame. The reloads; the
// attempts that ended go to `attempts`.
template <class Near>
int MinJerkTurn(float hz, double degrees, double seconds, Near near_at,
                VR::Body::Vec3 axis = {0, 1, 0}, double wobble_deg = 0,
                std::vector<ReloadAttempt>* attempts = nullptr) {
    WristReload r;
    r.Shot(Start);
    int reloads = 0;
    const double frame = 1.0 / hz;
    for (int i = 0; i * frame <= 1.5; ++i) {
        const double t = i * frame; // seconds after the shot
        const float a = static_cast<float>(degrees * Smooth((t - 0.2) / seconds)) * VR::Body::Deg;
        const float w = static_cast<float>(wobble_deg * std::sin(6.2831853 * 3.0 * t)) *
                        VR::Body::Deg;
        const VR::Body::Quat q =
            VR::Body::Normalize(AxisAngle(axis, a) * AxisAngle({1, 0, 0}, w));
        reloads += r.Step(Start + static_cast<std::int64_t>(t * 1e9), q, true, false, near_at(i));
        ReloadAttempt at;
        if (attempts != nullptr && r.TakeAttempt(at))
            attempts->push_back(at);
    }
    return reloads;
}
// The turn whose peak is `peak` rad/s.
double SecondsFor(double degrees, double peak) {
    return 1.875 * degrees * VR::Body::Deg / peak;
}
const auto Always = [](int) { return true; };
const auto Never = [](int) { return false; };
} // namespace

// Slow wrist turns near the body (reading the wrist gauge, reaching for the hip or a pouch, a
// vial at the belt) never reload, however far they turn: their peak stays under 5.5 rad/s.
TEST(WristReloadNear, SlowNearBodyTurnsNeverReload) {
    for (const float hz : {72.f, 90.f, 120.f})
        for (const double degrees : {30.0, 45.0, 60.0, 90.0, 120.0})
            for (const double peak : {2.0, 3.5, 5.0})
                for (const auto axis : {VR::Body::Vec3{0, 1, 0}, {1, 0, 0}, {0, 0, -1}})
                    EXPECT_EQ(MinJerkTurn(hz, degrees, SecondsFor(degrees, peak), Always, axis), 0)
                        << hz << " Hz, " << degrees << " deg at " << peak << " rad/s";
}
// ... while the same 5 rad/s turn with the arm extended reloads, as before (4.5 rad/s, 24 deg).
TEST(WristReloadNear, TheSameTurnExtendedStillReloads) {
    for (const float hz : {72.f, 90.f, 120.f})
        for (const double degrees : {45.0, 60.0})
            EXPECT_EQ(MinJerkTurn(hz, degrees, SecondsFor(degrees, 5.0), Never), 1)
                << hz << " Hz, " << degrees << " deg";
}
// A quick, deliberate flick of ordinary size reloads near the body at every headset rate, in any
// direction, and mid-fight too (the hand wobbling 5 degrees at 3 Hz, never quite still).
TEST(WristReloadNear, AQuickOrdinaryFlickReloadsNearTheBody) {
    const std::array<std::pair<double, double>, 4> flicks{
        {{30.0, 0.12}, {35.0, 0.15}, {28.0, 0.10}, {40.0, 0.16}}};
    for (const float hz : {72.f, 90.f, 120.f})
        for (const auto axis : {VR::Body::Vec3{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, 1}})
            for (const auto& [degrees, seconds] : flicks)
                for (const double wobble : {0.0, 5.0})
                    EXPECT_EQ(MinJerkTurn(hz, degrees, seconds, Always, axis, wobble), 1)
                        << hz << " Hz, " << degrees << " deg in " << seconds * 1000
                        << " ms, wobble " << wobble;
}
// Near once during a flick (the position only inferred for a frame, the hand passing the torso)
// is near for the whole flick: a 5 rad/s turn then does not reload; a quick flick does, however
// often the position drops out.
TEST(WristReloadNear, NearIsKeptForTheWholeFlick) {
    for (const float hz : {72.f, 90.f, 120.f}) {
        const int mid = static_cast<int>(0.33 * hz); // inside the turn
        EXPECT_EQ(MinJerkTurn(hz, 45.0, SecondsFor(45.0, 5.0), [&](int i) { return i == mid; }),
                  0)
            << hz;
        EXPECT_EQ(MinJerkTurn(hz, 30.0, 0.12, [](int i) { return i % 2 == 0; }), 1) << hz;
    }
}
// The attempt log: every flick from a still wrist with the chamber empty ends with its numbers
// and why.
TEST(WristReloadNear, AttemptsSayWhyTheyEnded) {
    std::vector<ReloadAttempt> a;
    EXPECT_EQ(MinJerkTurn(90.f, 45.0, SecondsFor(45.0, 5.0), Always, {0, 1, 0}, 0, &a), 0);
    ASSERT_FALSE(a.empty());
    const auto slow = std::find_if(a.begin(), a.end(),
                                   [](const ReloadAttempt& x) { return x.peak > 4.0f; });
    ASSERT_NE(slow, a.end());
    EXPECT_EQ(slow->end, ReloadEnd::TooSlow);
    EXPECT_TRUE(slow->near_body);
    EXPECT_FLOAT_EQ(slow->need, 5.5f);
    EXPECT_LT(slow->peak, 5.5f);
    EXPECT_GT(slow->turn, 24.0f * VR::Body::Deg);

    a.clear();
    EXPECT_EQ(MinJerkTurn(90.f, 30.0, 0.12, Always, {0, 1, 0}, 0, &a), 1);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a.back().end, ReloadEnd::Reloaded);
    EXPECT_GT(a.back().peak, 5.5f);
    EXPECT_GE(a.back().turn, 24.0f * VR::Body::Deg);
    EXPECT_GT(a.back().ms, 30.0f);
    EXPECT_LT(a.back().ms, 180.0f);

    a.clear(); // fast but small: 10 degrees in 50 ms (6.5 rad/s)
    EXPECT_EQ(MinJerkTurn(90.f, 10.0, 0.05, Never, {0, 1, 0}, 0, &a), 0);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a.front().end, ReloadEnd::TooSmall);
    EXPECT_FALSE(a.front().near_body);

    // The trigger, the orientation, the gun leaving the hand: each ends it with its reason.
    for (const ReloadEnd why :
         {ReloadEnd::Trigger, ReloadEnd::OrientationLost, ReloadEnd::Blocked}) {
        WristReload r;
        r.Shot(Start);
        Arm(r);
        EXPECT_FALSE(r.Step(Start + 190 * Ms, AxisAngle({1, 0, 0}, .15f), true, false));
        r.Step(Start + 200 * Ms, AxisAngle({1, 0, 0}, .2f), why == ReloadEnd::Trigger,
               why == ReloadEnd::Trigger, false, why);
        ReloadAttempt at;
        ASSERT_TRUE(r.TakeAttempt(at));
        EXPECT_EQ(at.end, why);
        EXPECT_FALSE(r.TakeAttempt(at)); // once
    }
}
// The attempt says why it was near (a position inferred for a frame vs the hand close to the
// neck) and the closest tracked distance, not the distance of the frame it ended on.
TEST(WristReloadNear, AttemptsSayWhyTheyWereNear) {
    using VR::Gun::NearSample;
    for (const float hz : {72.f, 90.f, 120.f}) {
        // An extended flick at 5 rad/s whose position drops out for one frame: near (inferred),
        // too slow, the closest tracked distance the reach's 0.55 m.
        const int mid = static_cast<int>(0.33 * hz);
        std::vector<ReloadAttempt> a;
        EXPECT_EQ(MinJerkTurn(hz, 45.0, SecondsFor(45.0, 5.0),
                              [&](int i) {
                                  return i == mid ? NearSample{true, true, -1.0f}
                                                  : NearSample{false, false, 0.55f + 0.001f * i};
                              },
                              {0, 1, 0}, 0, &a),
                  0)
            << hz;
        const auto slow = std::find_if(a.begin(), a.end(),
                                       [](const ReloadAttempt& x) { return x.peak > 4.0f; });
        ASSERT_NE(slow, a.end()) << hz;
        EXPECT_EQ(slow->end, ReloadEnd::TooSlow);
        EXPECT_TRUE(slow->near_body);
        EXPECT_TRUE(slow->near_inferred);
        EXPECT_FALSE(slow->near_close);
        EXPECT_GE(slow->closest, 0.55f);
        EXPECT_LT(slow->closest, 0.60f);
        EXPECT_STREQ(VR::Gun::NearWhy(*slow), "near the body (its position inferred)");
        // Close to the neck the whole time (tracked at 0.30 m): near by distance.
        a.clear();
        EXPECT_EQ(MinJerkTurn(hz, 30.0, 0.12, [](int) { return NearSample{true, false, 0.30f}; },
                              {0, 1, 0}, 0, &a),
                  1)
            << hz;
        ASSERT_FALSE(a.empty());
        EXPECT_EQ(a.back().end, ReloadEnd::Reloaded);
        EXPECT_TRUE(a.back().near_close);
        EXPECT_FALSE(a.back().near_inferred);
        EXPECT_FLOAT_EQ(a.back().closest, 0.30f);
        EXPECT_STREQ(VR::Gun::NearWhy(a.back()), "near the body (close to the neck)");
        // Never tracked: no closest distance.
        a.clear();
        MinJerkTurn(hz, 30.0, 0.12, [](int) { return NearSample{true, true, -1.0f}; }, {0, 1, 0},
                    0, &a);
        ASSERT_FALSE(a.empty());
        EXPECT_LT(a.back().closest, 0.0f);
        // Extended throughout.
        a.clear();
        MinJerkTurn(hz, 45.0, SecondsFor(45.0, 5.0),
                    [](int) { return NearSample{false, false, 0.5f}; }, {0, 1, 0}, 0, &a);
        ASSERT_FALSE(a.empty());
        EXPECT_FALSE(a.back().near_body);
        EXPECT_STREQ(VR::Gun::NearWhy(a.back()), "extended");
    }
}
// NearBody's Sample(): what Step takes, with why it is near.
TEST(WristReloadNear, NearBodySampleSaysWhy) {
    const VR::Body::Vec3 head{0, 1.6f, 0};
    const VR::Body::Quat ahead{0, 0, 0, 1};
    NearBody n;
    n.Update(true, {-0.2f, 1.1f, -0.15f}, true, head, ahead, 1.0f);
    EXPECT_TRUE(n.Sample().near);
    EXPECT_FALSE(n.Sample().inferred);
    EXPECT_NEAR(n.Sample().distance, 0.305f, 0.01f);
    n.Update(false, {-0.2f, 1.4f, -0.55f}, true, head, ahead, 1.0f);
    EXPECT_TRUE(n.Sample().near);
    EXPECT_TRUE(n.Sample().inferred);
    EXPECT_LT(n.Sample().distance, 0.0f);
    n.Update(true, {-0.2f, 1.4f, -0.55f}, true, head, ahead, 1.0f);
    EXPECT_FALSE(n.Sample().near);
    EXPECT_FALSE(n.Sample().inferred);
    // Inferred, then tracked again inside the band (0.40 m: 0.38-0.42): still near, and still
    // because of the inferred frame (never "close to the neck": the hand was never under 0.38 m).
    // The neck is 8 cm behind the eyes (+z), so the hand at z -0.32 is 0.40 m from it.
    n.Update(false, {}, true, head, ahead, 1.0f);
    n.Update(true, {0.0f, 1.2f, -0.32f}, true, head, ahead, 1.0f);
    EXPECT_TRUE(n.Sample().near);
    EXPECT_TRUE(n.Sample().inferred);
    EXPECT_NEAR(n.Sample().distance, 0.40f, 0.005f);
    // Under NearM: near by distance now.
    n.Update(true, {0.0f, 1.2f, -0.25f}, true, head, ahead, 1.0f);
    EXPECT_TRUE(n.Sample().near);
    EXPECT_FALSE(n.Sample().inferred);
    // Back into the band from close: near, by distance.
    n.Update(true, {0.0f, 1.2f, -0.32f}, true, head, ahead, 1.0f);
    EXPECT_TRUE(n.Sample().near);
    EXPECT_FALSE(n.Sample().inferred);
    // Inferred, then tracked over FarM: far, no cause left.
    n.Update(true, {0.0f, 1.2f, -0.32f}, false, head, ahead, 1.0f);
    EXPECT_TRUE(n.Sample().inferred);
    n.Update(true, {0.0f, 1.2f, -0.40f}, true, head, ahead, 1.0f);
    EXPECT_FALSE(n.Sample().near);
    EXPECT_FALSE(n.Sample().inferred);
}
// The attempt's label with the hand tracked again inside the band after an inferred frame that
// came before the still wrist: "its position inferred", never "close to the neck".
TEST(WristReloadNear, InferredThenInBandIsNotClose) {
    const VR::Body::Vec3 head{0, 1.6f, 0};
    const VR::Body::Quat ahead{0, 0, 0, 1};
    NearBody n;
    n.Update(false, {}, true, head, ahead, 1.0f);
    n.Update(true, {0.0f, 1.2f, -0.32f}, true, head, ahead, 1.0f);
    const VR::Gun::NearSample s = n.Sample();
    std::vector<ReloadAttempt> a;
    MinJerkTurn(90.f, 45.0, SecondsFor(45.0, 5.0), [&](int) { return s; }, {0, 1, 0}, 0, &a);
    const auto slow =
        std::find_if(a.begin(), a.end(), [](const ReloadAttempt& x) { return x.peak > 4.0f; });
    ASSERT_NE(slow, a.end());
    EXPECT_EQ(slow->end, ReloadEnd::TooSlow);
    EXPECT_TRUE(slow->near_inferred);
    EXPECT_FALSE(slow->near_close);
    EXPECT_STREQ(VR::Gun::NearWhy(*slow), "near the body (its position inferred)");
}
// The log: every reload, outside the cap and never counted; failed attempts filtered (no try)
// and capped at 4 a second, the rest counted for the next line.
TEST(WristReloadNear, EveryReloadIsLogged) {
    using VR::Gun::ReloadLogGate;
    ReloadLogGate g;
    const ReloadAttempt failed{ReloadEnd::TooSlow, true, 4.0f, 0.5f, 100.0f, 5.5f};
    const ReloadAttempt tiny{ReloadEnd::TooSlow, false, 1.0f, 0.05f, 100.0f, 4.5f};
    ReloadAttempt reloaded = failed;
    reloaded.end = ReloadEnd::Reloaded;
    const std::int64_t t0 = 5'000'000'000;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(g.Admit(failed, t0 + i * 10 * Ms)) << i;
    }
    EXPECT_FALSE(g.Admit(failed, t0 + 50 * Ms)); // the fifth this second
    EXPECT_FALSE(g.Admit(tiny, t0 + 60 * Ms));   // no try: neither logged nor counted
    EXPECT_TRUE(g.Admit(reloaded, t0 + 70 * Ms)); // the reload: always
    EXPECT_EQ(g.TakeDropped(), 1u);
    EXPECT_EQ(g.TakeDropped(), 0u);
    EXPECT_FALSE(g.Admit(failed, t0 + 80 * Ms)); // the reload took no line of the four
    EXPECT_TRUE(g.Admit(reloaded, t0 + 90 * Ms));
    EXPECT_TRUE(g.Admit(failed, t0 + 1'010 * Ms)); // the next second
    // Many reloads in one second: every one.
    ReloadLogGate h;
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(h.Admit(reloaded, t0 + i * Ms));
    }
    EXPECT_TRUE(h.Admit(failed, t0 + 20 * Ms));
}
// Near or extended: the hand's horizontal distance from the neck, by the player's height, with
// hysteresis; a hand or head position that is not tracked is near.
TEST(WristReloadNear, NearBodyTellsTheBodyFromTheReach) {
    const VR::Body::Vec3 head{0, 1.6f, 0};
    const VR::Body::Quat ahead{0, 0, 0, 1}; // looking along -z
    NearBody n;
    EXPECT_TRUE(n.Update(true, {-0.2f, 1.1f, -0.15f}, true, head, ahead, 1.0f)); // the belly
    EXPECT_NEAR(n.Distance(), 0.305f, 0.01f);
    EXPECT_FALSE(n.Update(true, {-0.2f, 1.4f, -0.55f}, true, head, ahead, 1.0f)); // aiming
    EXPECT_FALSE(n.Update(true, {-0.2f, 1.3f, -0.27f}, true, head, ahead, 1.0f)); // 0.40: kept
    EXPECT_TRUE(n.Update(true, {-0.2f, 1.3f, -0.22f}, true, head, ahead, 1.0f));  // 0.36: near
    EXPECT_TRUE(n.Update(true, {-0.2f, 1.3f, -0.27f}, true, head, ahead, 1.0f));  // 0.40: kept
    EXPECT_FALSE(n.Update(true, {-0.2f, 1.3f, -0.33f}, true, head, ahead, 1.0f)); // 0.45: far
    EXPECT_TRUE(n.Update(false, {-0.2f, 1.4f, -0.55f}, true, head, ahead, 1.0f)); // inferred
    EXPECT_LT(n.Distance(), 0.0f);
    EXPECT_TRUE(n.Update(true, {-0.2f, 1.4f, -0.55f}, false, head, ahead, 1.0f)); // no head
    // A taller player (k 1.25): 0.45 m from the neck is still near.
    NearBody tall;
    EXPECT_TRUE(tall.Update(true, {-0.2f, 1.3f, -0.33f}, true, head, ahead, 1.25f));
}

TEST(WristReloadNear, InferredHeadUsesNearThresholdWithoutCancellingQuickFlick) {
    for (float hz : {72.f, 90.f, 120.f}) {
        const auto inferred_head = [](int) {
            VR::Gun::NearBody near_body;
            near_body.Update(true, {-.2f, 1.4f, -.55f}, false,
                             {0, 1.6f, 0}, {0, 0, 0, 1}, 1.f);
            return near_body.Sample();
        };
        EXPECT_EQ(MinJerkTurn(hz, 45., SecondsFor(45., 5.), inferred_head), 0);
        EXPECT_EQ(MinJerkTurn(hz, 30., .12, inferred_head), 1);
    }
    VR::Gun::NearBody near_body;
    near_body.Update(true, {-.2f, 1.4f, -.55f}, false, {0, 1.6f, 0}, {0, 0, 0, 1}, 1.f);
    EXPECT_TRUE(near_body.Sample().inferred);
    near_body.Update(true, {-.2f, 1.4f, -.55f}, true, {0, 1.6f, 0}, {0, 0, 0, 1}, 1.f);
    EXPECT_FALSE(near_body.Near());
    EXPECT_FALSE(near_body.Sample().inferred);
}
