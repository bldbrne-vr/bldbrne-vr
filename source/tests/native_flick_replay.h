// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Native flick handoff scenarios frozen from the host flow before ABI v59. Expected
// transitions and side effects are explicit; the new kernel does not generate an oracle.
void FlickReplay(I::NativeAttackRuntime& runtime, I::Module& module) {
    I::NativeFlick flick{runtime};
    I::NativeLatch right{runtime, 1};
    uint32_t window = 10000;
    int cases = 0, finals = 0, live = 0, refunds = 0;
    int64_t now = 20'000'000'000ll;
    PiAttackFlickFacts tick{};
    const auto start = [&](bool to_b = false) {
        runtime.OwnerReset();
        right.End();
        M::LatchInput l{};
        l.have = l.open = l.gates = true;
        l.serial = ++window;
        Check(right.Step(l) == M::LatchStep::Create, "flick scenario latch create");
        tick = {};
        tick.command = PI_AF_TICK;
        tick.window = window;
        tick.run = window + 1;
        tick.serial = window + 2;
        tick.cur_serial = window;
        tick.style = 3;
        tick.open_ns = now - 30'000'000;
        tick.press_ns = now;
        tick.flags = PI_AF_HAVE | PI_AF_OPEN | PI_AF_ACTIVE | PI_AF_ENABLED | PI_AF_CUR_LIVE |
                     (to_b ? PI_AF_TO_B : 0);
        const auto o = flick.Call(tick, now);
        Check(o.valid && o.flick.flags == (PI_AFO_JOINED | PI_AFO_MAKE_FLICK) &&
                  flick.State().phase == PI_AF_OLD && flick.State().window == window &&
                  flick.State().run == tick.run && flick.State().to_b == to_b &&
                  flick.State().style == 3,
              "join captures stroke and transform once");
        Check(flick.Call(tick, now).flick.flags == 0,
              "repeated tick does not join or accelerate twice");
    };
    for (int hz : {30, 60, 90})
        for (bool to_b : {false, true})
            for (uint32_t hits : {0u, 2u})
                for (bool accepted : {false, true}) {
                    const int64_t dt = 1000000000ll / hz;
                    start(to_b);
                    now += dt;
                    tick.flags |= PI_AF_SWITCH_ASKED;
                    const auto handoff = flick.Call(tick, now);
                    Check(handoff.valid && handoff.flick.flags == PI_AFO_FINALIZE &&
                              flick.State().phase == PI_AF_GAP && flick.State().gap_ns == now,
                          "switch finalizes old attack before waiting for new part");
                    ++finals;
                    PiAttackFlickFacts ack{};
                    ack.command = PI_AF_OLD_RESULT;
                    ack.window = window;
                    ack.run = tick.run;
                    ack.old_hits = hits;
                    ack.old_created_ns = now - 250'000'000;
                    Check(flick.Call(ack, now).valid && flick.State().old_hits == hits &&
                              flick.State().old_created_ns == ack.old_created_ns,
                          "native finalization acknowledgement retained");
                    PiAttackFlickFacts gate{};
                    gate.command = PI_AF_OPEN_GATE;
                    gate.run = tick.run;
                    gate.style = 3;
                    gate.flags = PI_AF_OPEN;
                    Check(flick.Call(gate, now).flick.flags == 0,
                          "no new attack before switch acknowledgement");
                    gate.flags |= PI_AF_SWITCHED;
                    gate.style = 1;
                    Check(flick.Call(gate, now).flick.flags == 0,
                          "new form must match observed style");
                    gate.style = 3;
                    Check(flick.Call(gate, now).flick.flags == PI_AFO_PROCEED,
                          "new form ready for native row and dummy queries");
                    PiAttackFlickFacts part{};
                    part.command = PI_AF_PART;
                    part.flags = PI_AF_HAVE_ROW;
                    part.near_m = .75f;
                    part.since_s = 1.25f;
                    for (int i = 1; i <= 8; ++i) {
                        now += dt;
                        const auto waiting = flick.Call(part, now);
                        Check(waiting.valid && waiting.flick.flags == 0 &&
                                  flick.State().phase == PI_AF_GAP &&
                                  flick.State().waited == uint32_t(i),
                              "eight updates can wait for the new part");
                    }
                    part.flags |= PI_AF_HAVE_DUMMY | PI_AF_HAVE_GRIP;
                    part.dummy[0] = .75f;
                    const auto ready = flick.Call(part, now);
                    Check(ready.valid && ready.flick.flags == PI_AFO_PROCEED &&
                              flick.State().waited == 8,
                          "exact part distance is accepted without resetting wait count");
                    Near(ready.flick.since_s,
                         hits ? static_cast<float>(now - ack.old_created_ns) * 1e-9f
                              : part.since_s);
                    PiAttackFlickFacts result{};
                    result.command = PI_AF_OPEN_RESULT;
                    result.window = window;
                    result.run = tick.run;
                    result.flags = accepted ? PI_AF_CUR_LIVE : 0;
                    result.budget_used = 2;
                    const auto done = flick.Call(result, now);
                    const uint32_t want =
                        accepted ? (PI_AFO_MARK_FLICK | (hits ? 0 : PI_AFO_REFUND_BUDGET)) : 0;
                    Check(done.valid && done.flick.flags == want &&
                              flick.State().phase == (accepted ? PI_AF_LIVE : PI_AF_DONE),
                          "native open acceptance marks flick and refunds only unused budget");
                    live += accepted;
                    refunds += accepted && hits == 0;
                    ++cases;
                }
    start();
    now += 1;
    tick.flags |= PI_AF_ABORTED;
    auto o = flick.Call(tick, now);
    Check(o.valid && o.flick.reason == PI_AFR_OLD_ABORTED && !o.flick.flags &&
              flick.State().phase == PI_AF_DONE,
          "abort before switch leaves old native attack alone");
    start();
    tick.flags |= PI_AF_SWITCH_ASKED;
    flick.Call(tick, ++now);
    PiAttackFlickFacts part{};
    part.command = PI_AF_PART;
    part.flags = PI_AF_HAVE_ROW;
    for (int i = 0; i < 8; ++i)
        flick.Call(part, ++now);
    o = flick.Call(part, ++now);
    Check(o.valid && o.flick.reason == PI_AFR_NO_PART &&
              o.flick.flags == (PI_AFO_LOG_DONE | PI_AFO_END_LATCH) && flick.State().waited == 9 &&
              flick.State().phase == PI_AF_DONE,
          "ninth missing-part update ends a latch without a live attack");
    right.End();
    start();
    tick.flags |= PI_AF_SWITCH_ASKED;
    flick.Call(tick, ++now);
    part = {};
    part.command = PI_AF_PART;
    o = flick.Call(part, ++now);
    Check(o.valid && o.flick.reason == PI_AFR_NO_MOVE && (o.flick.flags & PI_AFO_END_LATCH),
          "missing new-form row ends handoff");
    for (uint32_t why : {PI_AFR_STOPPED, PI_AFR_CLOSED}) {
        start();
        tick.flags |= PI_AF_SWITCH_ASKED;
        flick.Call(tick, ++now);
        PiAttackFlickFacts g{};
        g.command = PI_AF_OPEN_GATE;
        g.run = tick.run;
        g.style = 3;
        g.flags = why == PI_AFR_STOPPED ? PI_AF_ABORTED : PI_AF_SWITCHED;
        o = flick.Call(g, ++now);
        Check(o.valid && o.flick.reason == why && flick.State().phase == PI_AF_DONE &&
                  (o.flick.flags & PI_AFO_END_LATCH),
              "cancel or closed window ends pending new-form attack");
    }
    start();
    tick.flags &= ~PI_AF_CUR_LIVE;
    tick.flags |= PI_AF_NO_JOIN;
    ++tick.run;
    o = flick.Call(tick, ++now);
    Check(o.valid && o.flick.reason == PI_AFR_NEWER && (o.flick.flags & PI_AFO_END_LATCH),
          "newer transform cancels old handoff");
    start();
    right.End();
    tick.flags &= ~PI_AF_CUR_LIVE;
    o = flick.Call(tick, ++now);
    Check(o.valid && o.flick.reason == PI_AFR_ENDED && !o.flick.flags &&
              flick.State().phase == PI_AF_DONE,
          "covered ended latch cannot reopen flick stroke");
    start();
    flick.Gap();
    Check(flick.State().phase == PI_AF_GAP && flick.State().gap_ns == 0,
          "unpaid or refused old-form swing skips directly to gap");
    runtime.OwnerReset();
    Check(flick.State().phase == PI_AF_DONE && !flick.State().window && !flick.State().old_hits &&
              !flick.State().gap_ns,
          "owner reset clears flick history and pending handoff");
    // Exact join timing boundaries for an inactive, already-switched run.
    for (int64_t offset : {349999999ll, 350000000ll, 350000001ll}) {
        runtime.OwnerReset();
        right.End();
        PiAttackFlickFacts f{};
        f.command = PI_AF_TICK;
        f.window = ++window;
        f.run = window + 1;
        f.serial = window + 2;
        f.style = 3;
        f.press_ns = now;
        f.open_ns = now + offset;
        f.flags = PI_AF_HAVE | PI_AF_OPEN | PI_AF_ENABLED | PI_AF_SWITCHED;
        o = flick.Call(f, now + offset);
        Check(o.valid && bool(o.flick.flags & PI_AFO_JOINED) == (offset <= 350000000),
              "exact post-squeeze attach boundary");
    }
    for (int64_t age : {1499999999ll, 1500000000ll, 1500000001ll}) {
        runtime.OwnerReset();
        right.End();
        PiAttackFlickFacts f{};
        f.command = PI_AF_TICK;
        f.window = ++window;
        f.run = window + 1;
        f.press_ns = now;
        f.open_ns = now - 1;
        f.flags = PI_AF_HAVE | PI_AF_OPEN | PI_AF_ENABLED;
        o = flick.Call(f, now + age);
        Check(o.valid && bool(o.flick.flags & PI_AFO_JOINED) == (age <= 1500000000),
              "exact run age boundary");
    }
    start();
    module.Reset();
    Check(flick.State().phase == PI_AF_DONE && !flick.State().window && !flick.State().old_hits,
          "generation reset clears approved flick snapshot");
    const auto covered = flick.Call(tick, now);
    Check(covered.valid && !covered.flick.flags && !flick.State().window,
          "generation reset cannot rejoin an already covered stroke");
    Check(!runtime.Faulted(), "flick and attack commands share one healthy pose sequence");
    std::printf("native flick: %d handoffs at 30/60/90 Hz, %d old finalizations, %d live attacks, "
                "%d budget refunds; wait, abort, reset and exact join thresholds\n",
                cases, finals, live, refunds);
}
