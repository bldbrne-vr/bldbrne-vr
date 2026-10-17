// SPDX-License-Identifier: GPL-2.0-or-later
#include <set>
#include <gtest/gtest.h>
#include "core/vr/lamp_channel.h"
#include "core/vr/lamp_presentation.h"

namespace {
using namespace VR::Lamps;
auto AllLit = [](int flag) -> std::optional<bool> { return flag < 70000000; };
Offer Example() { return {17, 2412951, TravelPages(AllLit)}; }

TEST(LampTravel, CatalogMatchesVerifiedReturnPointsAndFlags) {
    constexpr std::array<std::pair<int, int>, 44> expected{
        {{2412950, 12417810}, {2412951, 12417830}, {2412952, 12417850}, {2412953, 12417870},
         {2402950, 12407810}, {2402951, 12407830}, {2422950, 12427810}, {2422952, 12427850},
         {2422951, 12427830}, {2302950, 12307810}, {2302951, 12307830}, {2302952, 12307850},
         {2202950, 12207810}, {2202951, 12207830}, {2702950, 12707810}, {2702951, 12707830},
         {3202950, 13207810}, {3202952, 13207850}, {2802950, 12807810}, {2802953, 12807870},
         {2802951, 12807830}, {2802952, 12807850}, {2502950, 12507810}, {2502952, 12507850},
         {2502951, 12507830}, {2112950, 12117810}, {3202951, 13207830}, {3202953, 13207870},
         {3302950, 13307810}, {3302951, 13307830}, {2602950, 12607810}, {2602953, 12607870},
         {2602952, 12607850}, {2602951, 12607830}, {3402950, 13407810}, {3402951, 13407830},
         {3402953, 13407870}, {3402952, 13407850}, {3502950, 13507810}, {3502951, 13507830},
         {3502952, 13507850}, {3602950, 13607810}, {3602951, 13607830}, {3602952, 13607850}}};
    std::set<int> talks, points;
    for (size_t i = 0; i < Table.size(); ++i) {
        EXPECT_EQ(Table[i].ReturnPoint(), expected[i].first);
        EXPECT_EQ(Table[i].Lit(), expected[i].second);
        EXPECT_EQ(ByTalk(Table[i].Talk()), &Table[i]);
        EXPECT_EQ(ByReturnPoint(expected[i].first), &Table[i]);
        talks.insert(Table[i].Talk());
        points.insert(Table[i].ReturnPoint());
    }
    EXPECT_EQ(talks.size(), 44);
    EXPECT_EQ(points.size(), 44);
    EXPECT_EQ(Table[0].Talk(), 241000);
    EXPECT_EQ(Table[0].Blocked(), 70002410);
    EXPECT_EQ(Table[5].Blocked(), 70002401);  // Eileen's Grand Cathedral block
    EXPECT_EQ(Table[21].Blocked(), 70002802); // Hypogean Gaol
    EXPECT_EQ(Table[43].Talk(), 360002);
    EXPECT_EQ(Table[43].NameText(), 70003602);
    EXPECT_EQ(ByReturnPoint(2102950), nullptr); // Dream uses the native box
    EXPECT_EQ(ByTalk(290000), nullptr);         // no chalices
}
TEST(LampTravel, OnlyLitAndUnblockedAndKnownReadsAreOffered) {
    auto flags = [](int id) -> std::optional<bool> {
        if (id == Table[0].Lit())
            return true;
        if (id == Table[0].Blocked())
            return false;
        if (id == Table[5].Lit() || id == Table[5].Blocked())
            return true;
        if (id == Table[1].Lit())
            return true; // missing blocked read must fail closed
        return {};
    };
    auto pages = TravelPages(flags);
    ASSERT_EQ(pages.size(), 1);
    EXPECT_EQ(pages[0].destinations, (std::vector<int>{2412950}));
    EXPECT_TRUE(TravelPages([](int) -> std::optional<bool> { return false; }).empty());
}
TEST(LampTravel, FiveRowsPreserveHeadstoneOrderWithoutDroppingDlc) {
    auto pages = TravelPages(AllLit);
    ASSERT_EQ(pages.size(), 11);
    std::vector<int> flat;
    std::array<int, 5> counts{};
    for (auto& p : pages) {
        EXPECT_LE(p.destinations.size(), 5);
        EXPECT_FALSE(p.destinations.empty());
        ++counts[static_cast<int>(p.group)];
        flat.insert(flat.end(), p.destinations.begin(), p.destinations.end());
    }
    EXPECT_EQ(counts, (std::array<int, 5>{3, 2, 2, 2, 2}));
    ASSERT_EQ(flat.size(), Table.size());
    for (size_t i = 0; i < flat.size(); ++i)
        EXPECT_EQ(flat[i], Table[i].ReturnPoint());
    EXPECT_EQ(pages.front().Title(), "Yharnam 1/3");
    EXPECT_EQ(pages.back().Title(), "Hunter's Nightmare 2/2");
}
TEST(LampTravel, RestUsesInteractedLampAndBackDoesNotWarp) {
    Model m{Example()};
    auto rest = m.Confirm();
    ASSERT_TRUE(rest);
    EXPECT_EQ(rest->destination, 2412951);
    EXPECT_EQ(rest->action, Action::Rest);
    m.Move(1);
    EXPECT_FALSE(m.Confirm());
    EXPECT_TRUE(m.travel);
    m.Tab(-1);
    EXPECT_EQ(m.page, 10);
    m.Move(-1);
    auto travel = m.Confirm();
    ASSERT_TRUE(travel);
    EXPECT_EQ(travel->destination, 3602952);
    EXPECT_EQ(travel->action, Action::Travel);
    EXPECT_FALSE(m.Back());
    EXPECT_FALSE(m.travel);
    EXPECT_EQ(m.focus, 1);
    auto leave = m.Back();
    ASSERT_TRUE(leave);
    EXPECT_EQ(leave->action, Action::Leave);
    m.focus = 2;
    EXPECT_EQ(m.Confirm()->action, Action::Dream);
}
TEST(LampTravel, EmptyTravelListCannotProduceAnAction) {
    Model m{{4, 2412950, {}}};
    m.focus = 1;
    EXPECT_FALSE(m.Confirm());
    m.Move(1);
    m.Tab(1);
    EXPECT_FALSE(m.Confirm());
    EXPECT_FALSE(m.Back());
}
TEST(LampTravel, RejectsStaleOrUnofferedDestinations) {
    Offer o{17, 2412951, {{Group::Yharnam, 1, 1, {2412950}}}};
    EXPECT_TRUE(ValidRequest(o, {17, Action::Travel, 2412950}));
    EXPECT_FALSE(ValidRequest(o, {16, Action::Travel, 2412950}));
    EXPECT_FALSE(ValidRequest(o, {17, Action::Travel, 3402950}));
    EXPECT_FALSE(ValidRequest(o, {17, Action::Rest, 2412950}));
    EXPECT_FALSE(ValidRequest(o, {17, Action::None, 0}));
}
TEST(LampChannel, PresentBeforeSubmitAndConsumeExactlyOnce) {
    SetEnabled(true);
    Channel c;
    auto o = Example();
    c.Publish(o);
    EXPECT_TRUE(c.Input());
    EXPECT_FALSE(c.Submit({17, Action::Rest, 2412951}));
    ASSERT_TRUE(c.TakeOffer());
    EXPECT_FALSE(c.TakeOffer());
    c.Present(17);
    EXPECT_FALSE(c.Submit({16, Action::Rest, 2412951}));
    EXPECT_TRUE(c.Submit({17, Action::Rest, 2412951}));
    EXPECT_FALSE(c.Submit({17, Action::Rest, 2412951}));
    EXPECT_TRUE(c.TakeRequest());
    EXPECT_FALSE(c.TakeRequest());
    c.Cancel(16);
    EXPECT_TRUE(c.Active(17));
    c.Cancel(17);
    EXPECT_FALSE(c.Input());
    EXPECT_FALSE(c.Active(17));
    SetEnabled(false);
}
TEST(LampChannel, ApplicationReceiptFollowsActualPadAndNewCommand) {
    SetEnabled(true);
    Channel c;
    c.Publish(Example());
    c.Output(Pad::Back, true);
    EXPECT_FALSE(c.Delivery());
    int applied = 0;
    EXPECT_TRUE(c.ApplyInput([&](Pad value) { EXPECT_EQ(value,Pad::Back); ++applied; }));
    const auto first = c.Delivery();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->pad,Pad::Back);
    EXPECT_TRUE(c.ApplyInput([&](Pad) { ++applied; }));
    EXPECT_EQ(c.Delivery()->applied_ns,first->applied_ns);
    c.Output(Pad::Back,true);
    EXPECT_FALSE(c.Delivery());
    c.Cancel();
    EXPECT_FALSE(c.ApplyInput([&](Pad) { ++applied; }));
    EXPECT_EQ(applied,2);
    SetEnabled(false);
}
TEST(LampChannel, NewDialogAndDisableInvalidateOldActions) {
    SetEnabled(true);
    Channel c;
    c.Publish(Example());
    c.TakeOffer();
    c.Present(17);
    EXPECT_TRUE(c.Submit({17, Action::Dream, 0}));
    auto next = Example();
    next.serial = 18;
    c.Publish(next);
    EXPECT_FALSE(c.TakeRequest());
    EXPECT_FALSE(c.Submit({17, Action::Dream, 0}));
    c.TakeOffer();
    c.Present(18);
    SetEnabled(false);
    EXPECT_FALSE(c.Input());
    EXPECT_FALSE(c.Active(18));
    EXPECT_FALSE(c.Submit({18, Action::Dream, 0}));
}
} // namespace

#include "core/vr/lamp_sequence.h"
namespace {
using Outcome = Sequence::Result;
constexpr std::int64_t Ms(int n) { return static_cast<std::int64_t>(n) * 1000000; }
struct Driver {
    Sequence sequence;
    Pad pad{Pad::Neutral};
    std::optional<PadReceipt> receipt;
    Driver(Action action = Action::Rest, int selection = 0) {
        sequence.Start(action, selection, Ms(1));
    }
    void Apply(int t) {
        if (!receipt)
            receipt = PadReceipt{pad, Ms(t)};
    }
    Sequence::Output Tick(int t, DialogState state = DialogState::Original, int result = -1,
                          bool talk = true) {
        const auto command = sequence.Command();
        auto out = sequence.Tick(Ms(t), state, result, talk, receipt);
        if (command != sequence.Command() || pad != out.pad)
            receipt.reset();
        pad = out.pad;
        return out;
    }
    void Answer() {
        Apply(2);
        Tick(152);
        Apply(153);
    }
};
TEST(LampDialog, RecognizesObservedLampJobWithoutMatchingObjectDescriptions) {
    EXPECT_TRUE(IsLampDialogJob("MessageBox", false));
    EXPECT_FALSE(IsLampDialogJob("MessageBox", true));
    EXPECT_FALSE(IsLampDialogJob("IngameMessageBox", false));
    EXPECT_FALSE(IsLampDialogJob("CommandList", false));
    EXPECT_FALSE(IsLampDialogJob("WarpMenu", false));
}
TEST(LampDialog, RequiresOriginalDialogAndItsOwnClose) {
    // The serial counts opens; a close leaves it.
    EXPECT_EQ(DialogFor(17, 17, true, true), DialogState::Original);
    EXPECT_EQ(DialogFor(17, 17, false, true), DialogState::Closed);
    EXPECT_EQ(DialogFor(17, 17, false, false), DialogState::Changed);
    EXPECT_EQ(DialogFor(17, 18, true, true), DialogState::Changed);  // another menu opened
    EXPECT_EQ(DialogFor(17, 18, false, true), DialogState::Changed); // ... and closed
}
TEST(LampSequence, RestWaitsForAppliedBackNativeCancelReleaseAndConversation) {
    Driver d;
    EXPECT_EQ(d.Tick(100).pad, Pad::Neutral);
    d.Apply(101);
    EXPECT_EQ(d.Tick(250).pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(251).pad, Pad::Back);
    d.Apply(252);
    EXPECT_EQ(d.Tick(260, DialogState::Closed, 0).result, Outcome::Waiting);
    EXPECT_EQ(d.pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(400, DialogState::Closed, 0, false).result,
              Outcome::Waiting); // no release ack
    d.Apply(401);
    d.Tick(410, DialogState::Closed, 0, false);
    d.Tick(500, DialogState::Closed, 0, true); // resets quiet interval
    d.Tick(600, DialogState::Closed, 0, false);
    EXPECT_EQ(d.Tick(750, DialogState::Closed, 0, false).result, Outcome::Warp);
    EXPECT_EQ(d.Tick(800, DialogState::Closed, 0, false).result, Outcome::Finished);
}
TEST(LampSequence, ExternalDismissalBeforeDeliveryNeverWarps) {
    Driver d;
    d.Apply(2);
    EXPECT_EQ(d.Tick(152).pad, Pad::Back);
    // The command is published but the presenter has not applied it.
    EXPECT_EQ(d.Tick(153, DialogState::Closed, 0, false).result, Outcome::Cancelled);
    EXPECT_EQ(d.Tick(500, DialogState::Closed, 0, false).result, Outcome::Finished);
    Driver early;
    EXPECT_EQ(early.Tick(100, DialogState::Closed, 2, false).result, Outcome::Cancelled);
}
TEST(LampSequence, RefusesYesButAcceptsAnyOtherAnswerForRestAndTravel) {
    for (auto action : {Action::Rest, Action::Travel}) {
        // Yes (the game's own warp to the Dream): never.
        Driver yes{action};
        yes.Answer();
        EXPECT_EQ(yes.Tick(160, DialogState::Closed, 1, false).result, Outcome::Cancelled);
        // Our Back closed the box: whatever answer the game left (none, 0, 2, others).
        for (int result : {-1, 0, 2, 3, 999}) {
            Driver d{action};
            d.Answer();
            EXPECT_EQ(d.Tick(160, DialogState::Closed, result, false).result, Outcome::Waiting)
                << result;
        }
    }
}
TEST(LampSequence, DifferentOrInterveningDialogCancelsAfterDelivery) {
    Driver d;
    d.Answer();
    EXPECT_EQ(d.Tick(160, DialogState::Changed, 0, false).result, Outcome::Cancelled);
    Driver cleared;
    cleared.Answer();
    cleared.Tick(160, DialogState::Closed, 2, false);
    cleared.Apply(161);
    EXPECT_EQ(cleared.Tick(400, DialogState::Changed, 2, false).result, Outcome::Cancelled);
}
TEST(LampSequence, NoPresenterNeverProducesAnActionAndTimesOut) {
    Driver d;
    EXPECT_EQ(d.Tick(500).pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(7999).pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(8002).result, Outcome::Cancelled);
}
TEST(LampSequence, DelayedPresenterCannotSkipSelectYesOrItsRelease) {
    Driver d{Action::Dream, 1};
    d.Apply(2);
    EXPECT_EQ(d.Tick(152).pad, Pad::Left);
    EXPECT_EQ(d.Tick(300).pad, Pad::Left); // missed the entire old 120-ms window
    EXPECT_EQ(d.Tick(1000).pad, Pad::Left);
    d.Apply(1001);
    EXPECT_EQ(d.Tick(1120).pad, Pad::Left);
    EXPECT_EQ(d.Tick(1121).pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(2000).pad, Pad::Neutral); // release has not reached presenter
    d.Apply(2001);
    EXPECT_EQ(d.Tick(2150).pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(2151).pad, Pad::Yes);
    d.Apply(2152);
    EXPECT_EQ(d.Tick(2160, DialogState::Closed, 1).result, Outcome::Waiting);
    d.Apply(2161);
    EXPECT_EQ(d.Tick(2162, DialogState::Closed, 1).result, Outcome::Finished);
}
TEST(LampSequence, DreamCannotReportSuccessForNo) {
    Driver d{Action::Dream, 0};
    d.Answer();
    EXPECT_EQ(d.Tick(160, DialogState::Closed, 2, false).result, Outcome::Cancelled);
}
TEST(LampSequence, DeliveredButtonReleasesOnceThenTimesOutWithoutRepeating) {
    Driver d;
    d.Answer();
    EXPECT_EQ(d.Tick(272).pad, Pad::Back);
    EXPECT_EQ(d.Tick(273).pad, Pad::Neutral);
    d.Apply(274);
    EXPECT_EQ(d.Tick(7000).pad, Pad::Neutral);
    EXPECT_EQ(d.Tick(8002).result, Outcome::Cancelled);
}
TEST(LampSequence, NoResponseAllowsTravelAndLeaveNeverWarps) {
    Driver travel{Action::Travel};
    travel.Answer();
    travel.Tick(160, DialogState::Closed, 2, false);
    travel.Apply(161);
    travel.Tick(162, DialogState::Closed, 2, false);
    EXPECT_EQ(travel.Tick(312, DialogState::Closed, 2, false).result, Outcome::Warp);
    Driver leave{Action::Leave};
    leave.Answer();
    leave.Tick(160, DialogState::Closed, 0);
    leave.Apply(161);
    EXPECT_EQ(leave.Tick(162, DialogState::Closed, 0).result, Outcome::Finished);
}
TEST(LampChannel, OnlyAnAppliedCommandIsAcknowledgedAndEveryTransitionNeedsFreshReceipt) {
    SetEnabled(true);
    Channel c;
    c.Publish(Example());
    EXPECT_FALSE(c.Delivery());
    c.Input();
    EXPECT_FALSE(c.Delivery()); // inspecting input is not delivering it
    Pad pad = Pad::Yes;
    EXPECT_TRUE(c.ApplyInput([&](Pad value) { pad = value; }));
    EXPECT_EQ(pad, Pad::Neutral);
    ASSERT_TRUE(c.Delivery());
    const auto first = c.Delivery()->applied_ns;
    c.Output(Pad::Neutral);
    EXPECT_EQ(c.Delivery()->applied_ns, first);
    c.Output(Pad::Neutral, true);
    EXPECT_FALSE(c.Delivery());
    c.Output(Pad::Left);
    EXPECT_FALSE(c.Delivery());
    c.ApplyInput([&](Pad value) { pad = value; });
    EXPECT_EQ(pad, Pad::Left);
    ASSERT_TRUE(c.Delivery());
    EXPECT_EQ(c.Delivery()->pad, Pad::Left);
    c.Output(Pad::Neutral);
    EXPECT_FALSE(c.Delivery());
    c.Cancel();
    EXPECT_FALSE(c.Delivery());
    EXPECT_FALSE(c.ApplyInput([](Pad) { FAIL() << "cancelled command applied"; }));
    c.Publish(Example());
    SetEnabled(false);
    EXPECT_FALSE(c.ApplyInput([](Pad) { FAIL() << "disabled command applied"; }));
}
} // namespace

namespace {
TEST(LampPresentation, DelayedTravelPageCannotSelectTheNextPagesLamp) {
    Model model{Example()};
    Presentation view;
    model.focus = 1; // Travel
    const auto main = view.Update(model);
    view.Presented(main);
    EXPECT_FALSE(view.Confirm(model)); // enters Travel
    ASSERT_TRUE(model.travel);
    const auto first = view.Update(model);
    EXPECT_FALSE(view.Ready(model));
    EXPECT_FALSE(view.Confirm(model, 0)); // Lamp screen is still displayed
    view.Presented(first);
    ASSERT_TRUE(view.Ready(model));
    const int visible_destination = model.offer.pages[0].destinations[0];
    model.Tab(1);
    const auto second = view.Update(model);
    EXPECT_FALSE(view.Confirm(model, 0)); // click on the old first row is ignored
    view.Presented(first); // an older staged image finishes uploading
    EXPECT_FALSE(view.Ready(model));
    EXPECT_FALSE(view.Confirm(model, 0));
    view.Presented(second);
    const auto action = view.Confirm(model, 0);
    ASSERT_TRUE(action);
    EXPECT_EQ(action->action, Action::Travel);
    EXPECT_EQ(action->destination, model.offer.pages[1].destinations[0]);
    EXPECT_NE(action->destination, visible_destination);
}
TEST(LampPresentation, KeyboardConfirmWaitsForTheDisplayedFocus) {
    Model model{Example()};
    Presentation view;
    const auto rest = view.Update(model);
    view.Presented(rest);
    model.Move(2); // Dream
    EXPECT_FALSE(view.Confirm(model)); // even before a redraw is requested
    const auto dream = view.Update(model);
    EXPECT_FALSE(view.Confirm(model));
    view.Presented(dream);
    const auto action = view.Confirm(model);
    ASSERT_TRUE(action);
    EXPECT_EQ(action->action, Action::Dream);
}
TEST(LampPresentation, CpuRenderOrRejectedAndMissingFramesCannotEnableSelection) {
    Model model{Example()};
    Presentation view;
    const auto revision = view.Update(model); // rendered/staged, not submitted
    EXPECT_FALSE(view.Confirm(model));
    view.Presented(0); // rejected frame, hidden layer, or shouldRender=false
    EXPECT_FALSE(view.Confirm(model));
    view.Presented(revision);
    EXPECT_TRUE(view.Confirm(model));
    view.Presented(0);
    EXPECT_FALSE(view.Confirm(model));
    view.Presented(revision);
    EXPECT_TRUE(view.Confirm(model));
}
TEST(LampPresentation, PreviousSessionCannotAcknowledgeAReopenedLamp) {
    Model model{Example()};
    Presentation view;
    const auto old = view.Update(model);
    view.Presented(old);
    ASSERT_TRUE(view.Ready(model));
    view.Reset();
    const auto reopened = view.Update(model); // same lamp/native serial, fresh session
    ASSERT_NE(reopened, old);
    view.Presented(old);
    EXPECT_FALSE(view.Confirm(model));
    view.Presented(reopened);
    EXPECT_TRUE(view.Confirm(model));
}
TEST(LampPresentation, CosmeticRedrawKeepsSelectionReadyAndBackCanEscapePendingPage) {
    Model model{Example()};
    Presentation view;
    const auto main = view.Update(model);
    view.Presented(main);
    EXPECT_EQ(view.Update(model), main); // hover/glyph updates do not change the model
    EXPECT_TRUE(view.Ready(model));
    model.focus = 1;
    model.Confirm();
    view.Update(model);
    EXPECT_FALSE(view.Ready(model));
    EXPECT_FALSE(model.Back()); // pending Travel can still return to Lamp
    EXPECT_FALSE(model.travel);
    const auto back = view.Update(model);
    view.Presented(back);
    const auto leave = model.Back();
    ASSERT_TRUE(leave);
    EXPECT_EQ(leave->action, Action::Leave);
}
} // namespace
