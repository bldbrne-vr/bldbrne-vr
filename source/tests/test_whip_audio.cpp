// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <vector>
#include <gtest/gtest.h>
#include "core/vr/whip_audio.h"
#include "core/vr/whip_damage.h"
#include <limits>
namespace W = VR::Whip;

TEST(WhipRecallAudio, DragTracksActualReturnAndClinksOnlyOnceAtClosure) {
    W::RecallSoundGate gate;
    EXPECT_EQ(gate.Step(true, false, 4, 1, .01f).drag, 0);
    EXPECT_GT(gate.Step(true, true, 3.8f, 1, .01f).drag, 0);
    EXPECT_FALSE(gate.Step(true, true, 1.02f, 0, .01f).clink);
    EXPECT_TRUE(gate.Step(true, true, 1, 0, .01f).clink);
    for (int i = 0; i < 100; ++i) {
        const auto s = gate.Step(true, true, 1, 0, .01f);
        EXPECT_FALSE(s.clink); EXPECT_EQ(s.drag, 0);
    }
}
TEST(WhipRecallAudio, NewSwingStopsDragAndDoesNotClink) {
    W::RecallSoundGate gate;
    gate.Step(true, false, 4, 1, .01f);
    EXPECT_GT(gate.Step(true, true, 3.8f, 1, .01f).drag, 0);
    const auto swing = gate.Step(true, false, 4, 1, .01f);
    EXPECT_EQ(swing.drag, 0); EXPECT_FALSE(swing.clink);
    EXPECT_TRUE(gate.Step(true, true, 1, 0, .05f).clink);
}
TEST(WhipRecallAudio, WeaponChangesLoadsAndTinyPayoutsStaySilent) {
    W::RecallSoundGate gate;
    gate.Step(true, false, 4, 1, .01f);
    EXPECT_FALSE(gate.Step(false, true, 1, 0, .01f).clink);
    EXPECT_FALSE(gate.Step(true, true, 1, 0, .01f).clink);
    gate.Step(true, false, 4, 1, .01f);
    EXPECT_FALSE(gate.Step(true, true, 1, 0, .5f).clink);
    EXPECT_FALSE(gate.Step(true, true, 1, 0, .01f).clink);
    gate.Step(true, false, 1.03f, 0, .01f);
    EXPECT_FALSE(gate.Step(true, true, 1, 0, .01f).clink);
}
namespace {
// Paid out to x4 and reeled in to just before the length closes.
void ReelIn(W::RecallSoundGate& gate) {
    gate.Step(true, false, 4, 1, .01f);
    for (float s = 3.6f; s > 1.01f; s -= .4f) ASSERT_FALSE(gate.Step(true, true, s, .5f, .01f).clink);
}
}
TEST(WhipRecallAudio, ClinkWaitsForTheSwingingLinksToSettleThenPlaysOnce) {
    W::RecallSoundGate gate;
    ReelIn(gate);
    // The length closed (t_in .40: 0.39 s) while the links still swing (2 cm only at 0.47 s).
    int clinks = 0, frame = -1;
    for (int i = 0; i < 30; ++i) {
        const float deviation = std::max(0.0f, .31f - .04f * i); // within 2 cm from frame 8
        const auto s = gate.Step(true, true, 1, deviation, .01f);
        // The drag runs on while the links settle, into the clink; then silence.
        if (i < 8) EXPECT_GT(s.drag, 0) << i;
        else EXPECT_EQ(s.drag, 0) << i;
        if (s.clink) { ++clinks; frame = i; }
    }
    EXPECT_EQ(clinks, 1);
    EXPECT_EQ(frame, 8) << "not at the length's closure (frame 0), at the settled blade";
}
TEST(WhipRecallAudio, ABladeThatNeverSettlesClinksOnceAtTheTimeout) {
    W::RecallSoundGate gate;
    ReelIn(gate);
    float t = 0, at = -1;
    int clinks = 0;
    for (int i = 0; i < 100; ++i, t += .01f) {
        if (gate.Step(true, true, 1, .10f, .01f).clink) { ++clinks; at = t; }
    }
    EXPECT_EQ(clinks, 1);
    EXPECT_NEAR(at, W::RecallSoundGate::SettleTimeoutS, .011f);
}
TEST(WhipRecallAudio, ANewSwingWhileTheBladeSettlesCancelsTheClink) {
    for (const bool payout : {false, true}) {
        SCOPED_TRACE(payout ? "the length pays out again" : "a new swing is asked for");
        W::RecallSoundGate gate;
        ReelIn(gate);
        for (int i = 0; i < 5; ++i) ASSERT_FALSE(gate.Step(true, true, 1, .2f, .01f).clink);
        // Either way the interrupted reel-in stays silent (x1.05 does not re-arm).
        EXPECT_FALSE(gate.Step(true, payout, payout ? 1.05f : 1, .2f, .01f).clink);
        for (int i = 0; i < 50; ++i) {
            const auto s = gate.Step(true, true, 1, i < 5 ? .2f : 0, .01f);
            ASSERT_FALSE(s.clink) << i;
        }
    }
    // A real payout re-arms: its own reel-in clinks.
    W::RecallSoundGate gate;
    ReelIn(gate);
    EXPECT_FALSE(gate.Step(true, true, 1, .2f, .01f).clink);
    EXPECT_FALSE(gate.Step(true, false, 3, .2f, .01f).clink);
    EXPECT_FALSE(gate.Step(true, true, 1.5f, .2f, .01f).clink);
    EXPECT_TRUE(gate.Step(true, true, 1, 0, .01f).clink);
}
TEST(WhipRecallAudio, ALockedBladeClinksAtTheLengthsClosure) {
    W::RecallSoundGate gate;
    ReelIn(gate);
    EXPECT_TRUE(gate.Step(true, true, 1, 0, .01f).clink); // lock_closed: rest shape at once
    EXPECT_FALSE(gate.Step(true, true, 1, 0, .01f).clink);
    // Disabled while waiting: reset, no clink after.
    ReelIn(gate);
    EXPECT_FALSE(gate.Step(true, true, 1, .2f, .01f).clink);
    EXPECT_FALSE(gate.Step(false, true, 1, 0, .01f).clink);
    EXPECT_FALSE(gate.Step(true, true, 1, 0, .01f).clink);
}
namespace {
constexpr std::array<char, 8> Pcm{0, 32, 0, 32, 0, 32, 0, 32}; // 0.25 FS
constexpr std::int64_t Now = 1'000'000'000;
float Peak(const std::vector<float>& x) { float p=0; for(float v:x) p=std::max(p,std::abs(v)); return p; }
}
TEST(WhipRecallAudio, InactiveMixIsBitIdenticalAndSurroundChannelsAreUntouched) {
    W::RecallAudioMixer mix(Pcm, Pcm);
    std::vector<float> data(480 * 8, .1f);
    const auto original = data;
    mix.Mix(data.data(),480,8,48000,true,{},Now);
    EXPECT_EQ(data,original);
    W::RecallAudioFrame f{Now,0,0,1,1,true};
    mix.Mix(data.data(),480,8,48000,true,f,Now);
    EXPECT_GT(data[479*8+1],.1f);
    for (int i=0;i<480;++i) {
        EXPECT_EQ(data[i*8],.1f);
        for (int j=2;j<8;++j) EXPECT_EQ(data[i*8+j],.1f);
    }
}
TEST(WhipRecallAudio, StaleStateFadesDragAndDoesNotReplayClink) {
    W::RecallAudioMixer mix(Pcm,Pcm);
    std::vector<float> data(48000*2,0);
    W::RecallAudioFrame f{Now,Now,1,1,0,true};
    mix.Mix(data.data(),480,2,48000,true,f,Now);
    EXPECT_GT(Peak(data),0);
    std::fill(data.begin(),data.end(),0);
    mix.Mix(data.data(),48000,2,48000,true,f,Now+200'000'000);
    EXPECT_TRUE(std::all_of(data.begin()+4800, data.end(), [](float v) { return v == 0; }));
    f.ns=Now+1'000'000'000; f.drag=0;
    std::fill(data.begin(),data.end(),0);
    mix.Mix(data.data(),48000,2,48000,true,f,f.ns);
    EXPECT_EQ(Peak(data),0);
}
TEST(WhipRecallAudio, ClinkSerialPlaysOnceAndPcmClampsWithoutIntegerWrap) {
    W::RecallAudioMixer mix(Pcm,Pcm);
    std::array<std::int16_t,480*2> data{}; data.fill(32760);
    W::RecallAudioFrame f{Now,Now,1,0,0,true};
    mix.Mix(data.data(),480,2,48000,false,f,Now);
    EXPECT_EQ(data[0],32767);
    data.fill(0);
    mix.Mix(data.data(),480,2,48000,false,f,Now+10'000'000);
    for(auto v:data) EXPECT_EQ(v,0);
    f.clink_id++; f.ns+=20'000'000;f.clink_ns=f.ns;
    mix.Mix(data.data(),480,2,48000,false,f,f.ns);
    EXPECT_GT(data[0],0);
}
TEST(WhipRecallAudio, ResamplingKeepsOneShotDurationAcrossOutputRates) {
    std::vector<char> pcm(2400*2,0);
    for(int i=0;i<2400;++i) pcm[2*i+1]=32; // 100 ms at 24 kHz
    for(unsigned rate:{24000,44100,48000}) {
        W::RecallAudioMixer mix(Pcm,pcm);
        std::vector<float> data(rate/5,0);
        W::RecallAudioFrame f{Now,Now,1,0,0,true};
        mix.Mix(data.data(),data.size(),1,rate,true,f,Now);
        EXPECT_GT(data[static_cast<std::size_t>(rate*.099)],0);
        EXPECT_EQ(data[static_cast<std::size_t>(rate*.102)],0);
    }
}

TEST(WhipContactDamage, DistanceTapersEachEnemyHitWithoutChangingOtherWeapons) {
    EXPECT_FLOAT_EQ(W::ContactDamageFactor(2400,true,1.5f),1.0f);
    EXPECT_NEAR(W::ContactDamageFactor(2400,true,3),.85f,1e-6f);
    EXPECT_NEAR(W::ContactDamageFactor(2400,true,4),.75f,1e-6f);
    EXPECT_NEAR(W::ContactDamageFactor(2400,true,5.5f),.60f,1e-6f);
    float previous=1;
    for(float distance=0;distance<15;distance+=.01f) {
        const auto factor=W::ContactDamageFactor(2400,true,distance);
        ASSERT_LE(factor,previous);ASSERT_GE(factor,.60f);previous=factor;
        ASSERT_FLOAT_EQ(W::ContactDamageFactor(2200,true,distance),1);
        ASSERT_FLOAT_EQ(W::ContactDamageFactor(2400,false,distance),1);
        ASSERT_FLOAT_EQ(W::ContactDamageFactor(2000,true,distance),1);
    }
    EXPECT_FLOAT_EQ(W::ContactDamageFactor(2400,true,std::numeric_limits<float>::quiet_NaN()),1);
    EXPECT_FLOAT_EQ(W::ContactDamageFactor(2400,true,-1),1);
}
