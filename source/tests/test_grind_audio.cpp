// SPDX-License-Identifier: GPL-2.0-or-later

// The blades' grind (core/vr/grind_audio.h): silent without its loop, at level 0, or on a stale
// frame; louder with the level, eased in and out, darker when slow; the seam crossfaded. With
// VR_FSB_BANK set to the player's own game asset: the sparks loop decodes from it
// (core/vr/fsb_sample.h); nothing of the game is in the repo, so without it that test skips.

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/fsb_sample.h"
#include "core/vr/grind_audio.h"
#include "core/vr/whip_audio.h"

namespace {

namespace G = VR::Grind;

constexpr std::int64_t Ms = 1'000'000;

// A noise loop (24 kHz S16LE) of `n` samples.
std::vector<char> Noise(std::size_t n, int amp = 12000) {
    std::vector<char> pcm(n * 2);
    std::uint32_t x = 12345;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        const auto v = static_cast<std::int16_t>(static_cast<int>(x >> 16) % (2 * amp) - amp);
        pcm[2 * i] = static_cast<char>(v & 0xFF);
        pcm[2 * i + 1] = static_cast<char>((v >> 8) & 0xFF);
    }
    return pcm;
}

float Rms(const std::vector<float>& b) {
    double s = 0.0;
    for (const float v : b) {
        s += static_cast<double>(v) * v;
    }
    return static_cast<float>(std::sqrt(s / static_cast<double>(b.size())));
}

// One 10 ms stereo float buffer at 48 kHz of the frame `f`.
std::vector<float> Mix(G::GrindMixer& m, G::GrindFrame f, std::int64_t now) {
    std::vector<float> out(480 * 2, 0.0f);
    m.Mix(out.data(), 480, 2, 48000, true, f, now);
    return out;
}

TEST(GrindAudio, SilentWithoutItsLoopOrLevel) {
    G::GrindMixer none;
    EXPECT_EQ(Rms(Mix(none, {Ms, 1.0f}, Ms)), 0.0f);
    const auto pcm = Noise(24000);
    G::GrindMixer m(pcm);
    EXPECT_EQ(Rms(Mix(m, {Ms, 0.0f}, Ms)), 0.0f);
    // A frame not renewed for 150 ms: silence.
    EXPECT_EQ(Rms(Mix(m, {Ms, 1.0f}, Ms + 200 * Ms)), 0.0f);
}

TEST(GrindAudio, LouderWithTheLevelAndEasedOut) {
    const auto pcm = Noise(24000);
    G::GrindMixer soft(pcm), loud(pcm);
    for (int i = 0; i < 10; ++i) {
        Mix(soft, {Ms, 0.25f}, Ms);
        Mix(loud, {Ms, 1.0f}, Ms);
    }
    const float a = Rms(Mix(soft, {Ms, 0.25f}, Ms));
    const float b = Rms(Mix(loud, {Ms, 1.0f}, Ms));
    EXPECT_GT(a, 0.0f);
    EXPECT_GT(b, 2.0f * a); // louder (and brighter: the lowpass opens)
    EXPECT_LT(b, G::Gain);
    // Stopped: eased out over tens of ms, then silent.
    const float first = Rms(Mix(loud, {Ms, 0.0f}, Ms));
    EXPECT_GT(first, 0.0f);
    EXPECT_LT(first, b);
    for (int i = 0; i < 40; ++i) {
        Mix(loud, {Ms, 0.0f}, Ms);
    }
    EXPECT_EQ(loud.Level(), 0.0f);
    EXPECT_EQ(Rms(Mix(loud, {Ms, 0.0f}, Ms)), 0.0f);
}

TEST(GrindAudio, PansAndMixesIntoWhatIsThere) {
    const auto pcm = Noise(24000);
    G::GrindMixer m(pcm);
    std::vector<float> out(480 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        std::fill(out.begin(), out.end(), 0.0f);
        m.Mix(out.data(), 480, 2, 48000, true, {Ms, 1.0f, 0.85f}, Ms);
    }
    double l = 0.0, r = 0.0;
    for (std::size_t i = 0; i < 480; ++i) {
        l += std::abs(out[2 * i]);
        r += std::abs(out[2 * i + 1]);
    }
    EXPECT_GT(r, 3.0 * l); // to the right
    // S16 too, added to the game's own samples.
    std::vector<std::int16_t> s16(480 * 2, 1000);
    m.Mix(s16.data(), 480, 2, 48000, false, {Ms, 1.0f}, Ms);
    bool changed = false;
    for (const auto v : s16) {
        changed = changed || v != 1000;
    }
    EXPECT_TRUE(changed);
}

TEST(GrindAudio, TheSeamIsCrossfaded) {
    // A ramp: the seamless loop's end runs into its start without a jump.
    const std::size_t n = 24000;
    std::vector<char> pcm(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<std::int16_t>(-20000 + static_cast<int>(40000 * i / n));
        pcm[2 * i] = static_cast<char>(v & 0xFF);
        pcm[2 * i + 1] = static_cast<char>((v >> 8) & 0xFF);
    }
    const auto loop = G::MakeSeamless(pcm);
    const std::size_t m = loop.size() / 2;
    ASSERT_EQ(m, n - G::LoopFade);
    const auto at = [&](std::size_t i) {
        return static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(
            static_cast<unsigned char>(loop[2 * i]) |
            (static_cast<unsigned char>(loop[2 * i + 1]) << 8))));
    };
    EXPECT_LT(std::abs(at(m - 1) - at(0)), 400); // was a 40000 jump
    // Too short: unchanged.
    const auto tiny = Noise(100);
    EXPECT_EQ(G::MakeSeamless(tiny), tiny);
}

TEST(GrindAudio, NotABankIsNothing) {
    std::vector<std::uint8_t> junk(256, 7);
    EXPECT_TRUE(VR::Fsb::DecodeSample(junk, G::SampleName).empty());
    EXPECT_TRUE(VR::Fsb::DecodeSample({}, G::SampleName).empty());
}

TEST(GrindAudio, TheSparksLoopDecodesFromThePlayersBank) {
    const char* path = std::getenv("VR_FSB_BANK");
    if (path == nullptr) {
        GTEST_SKIP() << "VR_FSB_BANK (the player's sprj_main_eng.fsb) not set";
    }
    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());
    const std::vector<std::uint8_t> bank{std::istreambuf_iterator<char>(f), {}};
    const auto pcm = VR::Fsb::DecodeSample(bank, G::SampleName);
    // 48128 samples at 48 kHz: ~1.0 s at 24 kHz.
    EXPECT_NEAR(static_cast<double>(pcm.size() / 2), 24064.0, 64.0);
    std::vector<float> v;
    for (std::size_t i = 0; i < pcm.size() / 2; ++i) {
        v.push_back(static_cast<float>(static_cast<std::int16_t>(static_cast<std::uint16_t>(
                        static_cast<unsigned char>(pcm[2 * i]) |
                        (static_cast<unsigned char>(pcm[2 * i + 1]) << 8)))) /
                    32768.0f);
    }
    EXPECT_GT(Rms(v), 0.02f); // sound, not silence
    if (const char* dump = std::getenv("VR_FSB_DUMP")) { // the raw PCM, to listen to
        std::ofstream(dump, std::ios::binary).write(pcm.data(), static_cast<std::streamsize>(pcm.size()));
    }
    EXPECT_TRUE(VR::Fsb::DecodeSample(bank, "no_such_sample").empty());
}

TEST(CutterClose, TheLockFromItsLeadInWithTheRingUnderIt) {
    // A 0.5 s lock (silent for its 0.245 s lead-in) and a 0.3 s ring.
    auto lock = Noise(12000, 8000);
    for (std::size_t i = 0; i < 5880; ++i) {
        lock[2 * i] = lock[2 * i + 1] = 0;
    }
    const auto ring = Noise(7200, 3000);
    const auto close = VR::Whip::MakeCutterClose(lock, ring);
    EXPECT_EQ(close.size() / 2, 7200u); // as long as the longer: the ring (the lock cut to 6120)
    int peak = 0;
    for (std::size_t i = 0; i < close.size() / 2; ++i) {
        const int v = static_cast<std::int16_t>(static_cast<std::uint16_t>(
            static_cast<unsigned char>(close[2 * i]) |
            (static_cast<unsigned char>(close[2 * i + 1]) << 8)));
        peak = std::max(peak, std::abs(v));
    }
    EXPECT_GE(peak, 32700); // to full scale
    // No lock (or only its lead-in): nothing, the old clink stays.
    EXPECT_TRUE(VR::Whip::MakeCutterClose({}, ring).empty());
    EXPECT_TRUE(VR::Whip::MakeCutterClose(Noise(5000), ring).empty());
    // No ring: the lock alone.
    EXPECT_EQ(VR::Whip::MakeCutterClose(lock, {}).size() / 2, 12000u - 5880u);
}

TEST(CutterClose, TheMixerTakesItBetweenClinks) {
    const auto old_clink = Noise(3120);
    const auto close = Noise(9000);
    VR::Whip::RecallAudioMixer m(old_clink, old_clink, 0.48f, VR::Whip::CutterReelGain);
    EXPECT_FALSE(m.SetClink({}, 0.39f)); // not decoded yet: the old one stays
    EXPECT_TRUE(m.SetClink(close, VR::Whip::CutterCloseGain));
    // While a clink plays, it is not swapped under it.
    VR::Whip::RecallAudioMixer busy(old_clink, old_clink);
    std::vector<float> out(480 * 2, 0.0f);
    busy.Mix(out.data(), 480, 2, 48000, true, {Ms, Ms, 1, 0.0f, 0.0f, true}, Ms);
    EXPECT_FALSE(busy.SetClink(close, 0.39f));
}

TEST(CutterClose, DecodesFromThePlayersBank) {
    const char* path = std::getenv("VR_FSB_BANK");
    if (path == nullptr) {
        GTEST_SKIP() << "VR_FSB_BANK (the player's sprj_main_eng.fsb) not set";
    }
    std::ifstream f(path, std::ios::binary);
    ASSERT_TRUE(f.good());
    const std::vector<std::uint8_t> bank{std::istreambuf_iterator<char>(f), {}};
    const auto lock = VR::Fsb::DecodeSample(bank, VR::Whip::CutterLockSample);
    const auto ring = VR::Fsb::DecodeSample(bank, VR::Whip::CutterRingSample);
    ASSERT_FALSE(lock.empty());
    ASSERT_FALSE(ring.empty());
    const auto close = VR::Whip::MakeCutterClose(lock, ring);
    EXPECT_GT(close.size() / 2, 2400u); // > 0.1 s
    if (const char* dump = std::getenv("VR_CUTTER_DUMP")) {
        std::ofstream(dump, std::ios::binary).write(close.data(), static_cast<std::streamsize>(close.size()));
    }
}

} // namespace
