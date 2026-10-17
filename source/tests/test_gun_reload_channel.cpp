// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <array>
#include "core/vr/combat_channel.h"
#include "core/vr/gun_reload.h"
namespace {
namespace Cb = VR::Combat;
void Drain() {
    std::array<Cb::GunEvent, 64> events{};
    while (Cb::PollGunEvents(events.data(), events.size()) != 0) {}
}
TEST(GunReloadChannel, ConfirmedShotSurvivesFullFeedbackQueue) {
    Drain();
    const auto shot_ns = Cb::LatestGunShotNs() + 1'000'000'000;
    for (unsigned i=0; i<64; ++i)
        Cb::PushGunEvent({Cb::GunEventKind::Denied, i, 1, 0, shot_ns-1});
    Cb::PushGunEvent({Cb::GunEventKind::Fired, 65, 1, 1, shot_ns});
    std::array<Cb::GunEvent, 64> events{};
    ASSERT_EQ(Cb::PollGunEvents(events.data(), events.size()), 64);
    for (const auto& e:events) EXPECT_EQ(e.kind, Cb::GunEventKind::Denied);
    ASSERT_EQ(Cb::LatestGunShotNs(), shot_ns);
    VR::Gun::WristReload r;
    r.Shot(Cb::LatestGunShotNs());
    EXPECT_TRUE(r.Empty()); EXPECT_TRUE(r.RejectPull());
}
TEST(GunReloadChannel, FailedShotsAndFeedbackDoNotEmptyChamber) {
    Drain();
    const auto before = Cb::LatestGunShotNs();
    for (auto kind:{Cb::GunEventKind::Fired, Cb::GunEventKind::Dry,
                    Cb::GunEventKind::Denied, Cb::GunEventKind::Vanilla,
                    Cb::GunEventKind::ToolBurst})
        Cb::PushGunEvent({kind, 100, 1, 0, before+1});
    EXPECT_EQ(Cb::LatestGunShotNs(), before);
    Drain();
}
TEST(GunReloadChannel, VanillaShotCountsWithoutAimAndOldEventCannotReplaceIt) {
    Drain();
    const auto now = Cb::LatestGunShotNs()+1'000'000'000;
    // GameShot.a describes whether aim was changed, not whether a projectile was emitted.
    Cb::PushGunEvent({Cb::GunEventKind::GameShot, 1, 1, 0, now});
    Cb::PushGunEvent({Cb::GunEventKind::Fired, 2, 1, 1, now-1});
    EXPECT_EQ(Cb::LatestGunShotNs(), now);
    Drain();
}
} // namespace
