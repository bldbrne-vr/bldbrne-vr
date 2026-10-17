// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include "core/vr/gun_reload_audio.h"
namespace {
TEST(GunReloadAssets, EmbeddedCuesPlayToCompletionWithoutRepeating) {
    using namespace VR::Gun;
    const auto reload = ReloadPcm(), click = EmptyPcm();
    ASSERT_EQ(reload.size(), 26880u);
    ASSERT_EQ(click.size(), 10560u);
    for (const auto cue : {ReloadCue::Reload, ReloadCue::Empty}) {
        ReloadAudioMixer mixer(reload, click);
        const std::int64_t start = 10'000'000'000;
        PublishReloadAudio(cue, true, start);
        float energy = 0;
        // The actual publication path and 48 kHz stereo game output, across the full sound.
        for (int block=0; block<80; ++block) {
            std::array<float, 960> samples{};
            const auto now = start + block * 10'000'000LL;
            PublishReloadAudio(ReloadCue::None, true, now);
            mixer.Mix(samples.data(), 480, 2, 48000, true, LatestReloadAudio(), now);
            for (auto x:samples) {
                ASSERT_TRUE(std::isfinite(x)); ASSERT_LE(std::abs(x), 1.0f);
                energy += std::abs(x);
                if (block >= 60) EXPECT_EQ(x, 0.0f); // no replay from the heartbeat
            }
        }
        EXPECT_GT(energy, 1.0f);
    }
}
} // namespace
