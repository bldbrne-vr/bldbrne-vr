// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_GPU_PASS_MERGE (src/video_core/renderer_vulkan/vk_pass_merge.h): the switch and its verify,
// when a UI draw's own depth/stencil writes keep it before its shadows, the order of the
// decision's reasons, and a model of the passes a frame's UI draws begin: each target gets its
// draws in the same order as before, a draw that writes the depth/stencil still comes after its
// shadows, and no sequence of draws begins more passes than the old order did.

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_pass_merge.h"

namespace {

using namespace Vulkan::PassMerge;

TEST(PassMerge, SwitchAndVerifyFromTheEnvironment) {
    EXPECT_TRUE(SwitchOn(nullptr)); // on by default
    EXPECT_TRUE(SwitchOn(""));
    EXPECT_TRUE(SwitchOn("1"));
    EXPECT_FALSE(SwitchOn("0")); // FAST_GPU_PASS_MERGE=0: the old order and ends
    EXPECT_FALSE(SwitchOn("00"));
    EXPECT_FALSE(VerifyOn(nullptr)); // off by default
    EXPECT_FALSE(VerifyOn(""));
    EXPECT_FALSE(VerifyOn("0"));
    EXPECT_TRUE(VerifyOn("1"));
}

TEST(PassMerge, DepthWritesNeedTheAttachmentTheTestAndTheWrite) {
    DepthStencilUse u{.has_depth = true, .depth_test = true, .depth_write = true};
    EXPECT_TRUE(WritesDepthStencil(u));
    u.has_depth = false;
    EXPECT_FALSE(WritesDepthStencil(u)); // no depth attachment
    u = {.has_depth = true, .depth_test = false, .depth_write = true};
    EXPECT_FALSE(WritesDepthStencil(u)); // the test off: nothing is written (a stale write flag)
    u = {.has_depth = true, .depth_test = true, .depth_write = false};
    EXPECT_FALSE(WritesDepthStencil(u)); // tested only: the shadows see the same depth
}

TEST(PassMerge, StencilWritesNeedAMaskAndAnOpOtherThanKeep) {
    const StencilFace keep_all{.write_mask = 0xff, .keeps = true};
    const StencilFace replace{.write_mask = 0xff, .keeps = false};
    const StencilFace masked{.write_mask = 0, .keeps = false};
    DepthStencilUse u{.has_stencil = true, .stencil_test = true, .front = keep_all,
                      .back = keep_all};
    EXPECT_FALSE(WritesDepthStencil(u)); // Scaleform's mask test: read only
    u.front = replace;
    EXPECT_TRUE(WritesDepthStencil(u));
    u.front = masked;
    EXPECT_FALSE(WritesDepthStencil(u)); // a write mask of 0 writes nothing
    u.back = replace;
    EXPECT_TRUE(WritesDepthStencil(u)); // the back face alone
    u.stencil_test = false;
    EXPECT_FALSE(WritesDepthStencil(u)); // the test off
    u.stencil_test = true;
    u.has_stencil = false;
    EXPECT_FALSE(WritesDepthStencil(u)); // no stencil attachment
}

TEST(PassMerge, DecideAsksInOrderAndSwapsOnlyWhenNothingHolds) {
    const Inputs eligible{.on = true, .pass_open = true};
    EXPECT_EQ(Decide(eligible), NumFallbacks);
    Inputs in = eligible;
    in.on = false;
    in.depth_stencil = true;
    EXPECT_EQ(Decide(in), Off); // off: the first reason, whatever else holds
    in = eligible;
    in.pass_open = false;
    in.writes = true;
    EXPECT_EQ(Decide(in), NotOpen);
    const auto only = [&](bool Inputs::*field, Fallback expected) {
        Inputs one = eligible;
        one.*field = true;
        EXPECT_EQ(Decide(one), expected);
    };
    only(&Inputs::hook, Hook);
    only(&Inputs::scaled, Scaled);
    only(&Inputs::clears, Clears);
    only(&Inputs::feedback, Feedback);
    only(&Inputs::writes, Writes);
    only(&Inputs::depth_stencil, DepthStencil);
    in = eligible;
    in.feedback = in.depth_stencil = true;
    EXPECT_EQ(Decide(in), Feedback); // the first that holds is counted
    EXPECT_EQ(FallbackNames.size(), static_cast<size_t>(NumFallbacks));
}

TEST(PassMerge, KeepOpenOnlyForABoundTargetOutsideTheScaledHud) {
    EXPECT_TRUE(KeepOpen(true, true, true, false));
    EXPECT_FALSE(KeepOpen(false, true, true, false)); // the switch off: every end explicit
    EXPECT_FALSE(KeepOpen(true, false, true, false)); // no pass open: nothing to keep
    EXPECT_FALSE(KeepOpen(true, true, false, false)); // TargetView records its first barrier
    EXPECT_FALSE(KeepOpen(true, true, true, true));   // bit 58/59's depth twin may copy
}

TEST(PassMerge, OrderPutsTheDrawLastOrFirst) {
    constexpr auto before = Order(false);
    constexpr auto after = Order(true);
    EXPECT_EQ(before, (std::array{Target::W, Target::K, Target::X})); // as always
    EXPECT_EQ(after, (std::array{Target::X, Target::K, Target::W}));
    // The two orders chain: one's last pass is the other's first.
    EXPECT_EQ(before.back(), after.front());
    EXPECT_EQ(after.back(), before.front());
}

// A model of one frame's draws on the GPU thread: a pass is begun for a target unless the open
// pass is that target's (the same state: the scheduler goes on with it).
enum class Event : std::uint8_t {
    Ui,       // a UI draw that may go first (nothing the shadows read differs)
    UiFixed,  // a UI draw that keeps the old order (it writes the depth/stencil, a hook, ...)
    Plain,    // a draw into X without shadows (a skipped backdrop, a banner rectangle)
    Other,    // a draw into another target (a mask pass, an off-target draw)
};

struct Pass {
    Target target;
    int draw; // the event's index
};

struct Model {
    explicit Model(bool merge_, int open_ = -1) : merge{merge_}, open{open_} {}

    bool merge;
    int open; // -1: none, 3: another target, else the Target
    int passes{};
    std::vector<Pass> order;

    void Draw(int target, int draw) {
        if (open != target) {
            ++passes;
            open = target;
        }
        if (target <= 2) {
            order.push_back({static_cast<Target>(target), draw});
        }
    }

    void Run(const std::vector<Event>& events) {
        for (int i = 0; i < static_cast<int>(events.size()); ++i) {
            switch (events[i]) {
            case Event::Ui:
            case Event::UiFixed: {
                const bool after = merge && events[i] == Event::Ui &&
                                   open == static_cast<int>(Target::X);
                for (const Target t : Order(after)) {
                    Draw(static_cast<int>(t), i);
                }
                break;
            }
            case Event::Plain:
                Draw(static_cast<int>(Target::X), i);
                break;
            case Event::Other:
                Draw(3, i);
                break;
            }
        }
    }

    std::vector<int> DrawsInto(Target target) const {
        std::vector<int> draws;
        for (const Pass& p : order) {
            if (p.target == target) {
                draws.push_back(p.draw);
            }
        }
        return draws;
    }
};

TEST(PassMerge, ARunOfUiDrawsBeginsTwoPassesEach) {
    const std::vector<Event> run(40, Event::Ui);
    Model old_order{false};
    Model merged{true};
    old_order.Run(run);
    merged.Run(run);
    EXPECT_EQ(old_order.passes, 3 * 40);
    EXPECT_EQ(merged.passes, 2 * 40 + 1); // the first draw finds no X pass open
    // The composite's pass still open: every draw begins two.
    Model after_composite{true, static_cast<int>(Target::X)};
    after_composite.Run(run);
    EXPECT_EQ(after_composite.passes, 2 * 40);
}

TEST(PassMerge, EachTargetGetsItsDrawsInTheSameOrder) {
    std::vector<Event> events;
    std::uint32_t seed = 12345;
    for (int i = 0; i < 4000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const std::uint32_t r = seed >> 24;
        events.push_back(r < 150   ? Event::Ui
                         : r < 200 ? Event::UiFixed
                         : r < 230 ? Event::Plain
                                   : Event::Other);
    }
    Model old_order{false};
    Model merged{true};
    old_order.Run(events);
    merged.Run(events);
    for (const Target t : {Target::X, Target::K, Target::W}) {
        EXPECT_EQ(merged.DrawsInto(t), old_order.DrawsInto(t));
    }
    // Never more passes than the old order, and fewer here.
    EXPECT_LE(merged.passes, old_order.passes);
    EXPECT_LT(merged.passes, old_order.passes);
    // A draw that keeps the old order (its depth/stencil writes) still comes after its shadows.
    std::vector<int> x_at(events.size(), -1), k_at(events.size(), -1), w_at(events.size(), -1);
    for (int at = 0; at < static_cast<int>(merged.order.size()); ++at) {
        const Pass& p = merged.order[at];
        (p.target == Target::X ? x_at : p.target == Target::K ? k_at : w_at)[p.draw] = at;
    }
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i] == Event::UiFixed) {
            EXPECT_GT(x_at[i], k_at[i]);
            EXPECT_GT(x_at[i], w_at[i]);
        }
    }
}

TEST(PassMerge, NoMixOfDrawsBeginsMorePassesThanBefore) {
    // Every sequence of four events, from every open pass.
    constexpr std::array kinds{Event::Ui, Event::UiFixed, Event::Plain, Event::Other};
    for (int start = -1; start <= 3; ++start) {
        for (int code = 0; code < 4 * 4 * 4 * 4; ++code) {
            std::vector<Event> events;
            for (int c = code, n = 0; n < 4; ++n, c /= 4) {
                events.push_back(kinds[c % 4]);
            }
            Model old_order{false, start};
            Model merged{true, start};
            old_order.Run(events);
            merged.Run(events);
            EXPECT_LE(merged.passes, old_order.passes) << "start " << start << " code " << code;
        }
    }
}

} // namespace
