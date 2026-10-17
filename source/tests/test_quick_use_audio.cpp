// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <vector>
#include <gtest/gtest.h>
#include "core/vr/quick_settings.h"
#include "core/vr/quick_use_audio.h"

namespace {
namespace Q = VR::Quick;
constexpr std::int64_t Now = 1'000'000'000;
constexpr unsigned Rate = 48000;

double Energy(const std::vector<float>& samples, bool difference = false) {
    double energy = 0;
    for (std::size_t i = samples.size() / 2; i < samples.size(); ++i) {
        const double value = samples[i] - (difference ? samples[i - 1] : 0);
        energy += value * value;
    }
    return energy;
}

TEST(QuickUseAudio, RisesInLoudnessAndBrightnessWithoutAStartClick) {
    std::vector<float> quiet(Rate / 4), full(quiet.size());
    Q::UseAudioMixer a, b;
    a.Mix(quiet.data(), quiet.size(), 1, Rate, true, {Now, 0, 1}, Now);
    b.Mix(full.data(), full.size(), 1, Rate, true, {Now, 1, 1}, Now);
    EXPECT_GT(Energy(quiet), 0);
    EXPECT_GT(Energy(full), Energy(quiet) * 5);
    EXPECT_GT(Energy(full, true) / Energy(full), Energy(quiet, true) / Energy(quiet));
    EXPECT_LT(std::abs(full.front()), 0.001f);
    for (float sample : full) EXPECT_LT(std::abs(sample), 0.3f);
}

TEST(QuickUseAudio, CompletionCancellationStaleInputAndMuteFadeToSilence) {
    for (const Q::UseAudioFrame stop : {
             Q::UseAudioFrame{Now, -1, 1}, Q::UseAudioFrame{Now, 1, 0},
             Q::UseAudioFrame{Now - 150'000'000, 1, 1},
             Q::UseAudioFrame{Now + 1, 1, 1},
             Q::UseAudioFrame{Now, std::numeric_limits<float>::quiet_NaN(), 1},
             Q::UseAudioFrame{Now, 1, std::numeric_limits<float>::infinity()}}) {
        Q::UseAudioMixer mixer;
        std::vector<float> samples(Rate / 10);
        mixer.Mix(samples.data(), samples.size(), 1, Rate, true, {Now, 1, 2}, Now);
        ASSERT_GT(Energy(samples), 0);
        std::fill(samples.begin(), samples.end(), 0);
        mixer.Mix(samples.data(), samples.size(), 1, Rate, true, stop, Now);
        EXPECT_TRUE(std::all_of(samples.begin() + Rate / 50, samples.end(),
                                [](float s) { return s == 0; }));
        std::fill(samples.begin(), samples.end(), 0.125f);
        mixer.Mix(samples.data(), samples.size(), 1, Rate, true, stop, Now);
        EXPECT_TRUE(std::all_of(samples.begin(), samples.end(),
                                [](float s) { return s == 0.125f; }));
    }
}

TEST(QuickUseAudio, FloatAndPcmMatchAndOnlyFrontChannelsAreChanged) {
    for (unsigned rate : {8000u, 24000u, 44100u, 48000u}) {
        for (unsigned channels : {1u, 2u, 8u}) {
            std::vector<float> floats(2048 * channels);
            std::vector<std::int16_t> pcm(floats.size());
            Q::UseAudioMixer a, b;
            a.Mix(floats.data(), 2048, channels, rate, true, {Now, 0.8f, 1}, Now);
            b.Mix(pcm.data(), 2048, channels, rate, false, {Now, 0.8f, 1}, Now);
            for (std::size_t i = 0; i < floats.size(); ++i) {
                EXPECT_NEAR(floats[i], pcm[i] / 32768.0f, 1.0f / 32768);
                if (i % channels >= 2) EXPECT_EQ(floats[i], 0);
            }
        }
    }
}

TEST(QuickUseAudio, QuietOrDisabledCueDoesNotAlterGameAudioAndMixingDoesNotOverflow) {
    Q::UseAudioMixer mixer;
    std::vector<float> floats(4096, 0.999f);
    mixer.Mix(floats.data(), floats.size(), 1, Rate, true, {}, Now);
    EXPECT_TRUE(std::all_of(floats.begin(), floats.end(), [](float s) { return s == 0.999f; }));
    mixer.Mix(floats.data(), floats.size(), 1, Rate, true, {Now, 1, 2}, Now);
    for (float s : floats) EXPECT_LE(std::abs(s), 1);
    for (std::int16_t extreme : {std::int16_t{32767}, std::int16_t{-32768}}) {
        std::vector<std::int16_t> pcm(4096, extreme);
        mixer.Mix(pcm.data(), pcm.size(), 1, Rate, false, {Now, 1, 2}, Now);
        for (auto s : pcm) EXPECT_EQ(s > 0, extreme > 0); // no integer wraparound
    }
}

TEST(QuickUseAudio, VolumeSettingIsIndependentAndWrittenToNewConfigs) {
    const auto saved = Q::GetQuickSettings();
    EXPECT_TRUE(Q::ParseQuickSetting("quick_use_whoosh", "0"));
    EXPECT_EQ(Q::GetQuickSettings().use_whoosh, 0);
    EXPECT_EQ(Q::GetQuickSettings().use_sounds, saved.use_sounds);
    Q::ParseQuickSetting("quick_use_whoosh", "9");
    EXPECT_EQ(Q::GetQuickSettings().use_whoosh, 2);
    Q::ParseQuickSetting("quick_use_whoosh", "nan");
    EXPECT_EQ(Q::GetQuickSettings().use_whoosh, 0);
    std::ostringstream defaults;
    Q::WriteQuickDefaults(defaults);
    EXPECT_NE(defaults.str().find("quick_use_whoosh=1\n"), std::string::npos);
    Q::GetQuickSettings() = saved;
}
} // namespace
