// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vector>
void LeftRowsReplay(I::NativeAttackRuntime& runtime, I::Module& module) {
    I::NativeLeftRows rows{runtime};
    int scans = 0, queries = 0;
    const auto config = [&](int variation, int hand, bool gun = false, int punch = 0) {
        PiAttackLeftRowsFacts f{};
        f.command = PI_ALR_CONFIG;
        f.variation = variation;
        f.hand = hand;
        f.punch = punch;
        f.flags = gun ? PI_ALR_GUN : 0;
        const auto o = rows.Call(f);
        Check(o.valid, "left rows configuration");
        return o;
    };
    const auto scan = [&](int hand, bool moves, int mode) {
        const int32_t own[] = {400, 410, 550, 570, -1, 930};
        const auto ref = [&](int j) {
            return mode == 0 ? -1 : mode == 1 ? 0 : j % 17 == 0 ? 0 : 1;
        };
        std::vector<int> want_queries, want_rows;
        const auto add = [&](int judge) {
            if (want_rows.size() >= 24)
                return;
            if (moves && hand == -1)
                for (int j : own)
                    if (j == judge)
                        return;
            want_queries.push_back(judge);
            if (ref(judge) == 0)
                want_rows.push_back(judge);
        };
        // Frozen native ScanLeftRows enumeration/query order; no DLL state is used
        // to generate the expected candidates or behavior-ref queries.
        if (hand == -2) {
            for (int j = 800; j <= 899; ++j)
                add(j);
            for (int j = 0; j <= 399; ++j)
                add(j);
        } else
            for (int j = 400; j <= 999; ++j)
                if (j != 790 && (j < 800 || j > 899))
                    add(j);
        PiAttackLeftRowsFacts f{};
        f.command = PI_ALR_SCAN_BEGIN;
        f.flags = moves ? PI_ALR_MOVES : 0;
        for (int i = 0; i < 6; ++i)
            f.right_judges[i] = own[i];
        auto o = rows.Call(f);
        size_t index = 0;
        while (o.valid && (o.rows.flags & PI_ALRO_QUERY)) {
            Check(index < want_queries.size() && o.rows.query_judge == want_queries[index++],
                  "exact left native query sequence");
            PiAttackLeftRowsFacts reply{};
            reply.command = PI_ALR_SCAN_REPLY;
            reply.judge = o.rows.query_judge;
            reply.ref_type = ref(reply.judge);
            o = rows.Call(reply);
            ++queries;
        }
        Check(o.valid && index == want_queries.size() && rows.State().count == want_rows.size() &&
                  rows.State().next == 0 && !rows.State().verified,
              "bounded candidate list matches native scan");
        for (size_t i = 0; i < want_rows.size(); ++i)
            Check(rows.State().judges[i] == want_rows[i], "candidate prefix parity");
        ++scans;
    };
    for (int hand : {-1, -2})
        for (bool moves : {false, true})
            for (int mode : {0, 1, 2}) {
                runtime.OwnerReset();
                config(101, hand);
                scan(hand, moves, mode);
                const auto before = rows.State();
                Check(!(config(101, hand).rows.flags & PI_ALRO_CHANGED) &&
                          rows.State().count == before.count,
                      "unchanged weapon retains candidate list");
            }
    runtime.OwnerReset();
    config(100, -2);
    scan(-2, false, 1);
    PiAttackLeftRowsFacts f{};
    f.command = PI_ALR_RESTORE;
    f.learned = -1;
    auto o = rows.Call(f);
    Check(o.valid && o.rows.flags == (PI_ALRO_RESTORED | PI_ALRO_KNOWN) && rows.State().verified &&
              rows.State().judges[rows.State().next] == 800 && rows.State().check == 1000,
          "known bare-fist row and first-hit check");
    config(100, -2, true, 1400);
    scan(-2, false, 1);
    o = rows.Call(f);
    Check(o.valid && rows.State().check == 1400 && rows.State().gun,
          "known gun punch preserves its own session-check key");
    f.command = PI_ALR_COST;
    f.flags = PI_ALR_LIGHT;
    f.cost = 15;
    f.whoosh = 777;
    rows.Call(f);
    Check(rows.State().cost == 20 && rows.State().whoosh == -1,
          "left weapon keeps default cost and no dual-blade whoosh");
    runtime.OwnerReset();
    config(101, -1);
    scan(-1, false, 1);
    f = {};
    f.command = PI_ALR_RESTORE;
    f.learned = 402;
    o = rows.Call(f);
    Check(o.valid && o.rows.flags == PI_ALRO_RESTORED && rows.State().next == 2 &&
              rows.State().verified && !rows.State().check,
          "remembered candidate restored without known-row check");
    config(102, -1);
    scan(-1, false, 1);
    f = {};
    f.command = PI_ALR_COST;
    f.flags = PI_ALR_LIGHT;
    f.cost = 15;
    f.whoosh = 778;
    rows.Call(f);
    Check(rows.State().cost == 15 && rows.State().whoosh == 778,
          "dual weapon light cost and sound copied into model");
    const auto begin = [&] {
        PiAttackLeftRowsFacts b{};
        b.command = PI_ALR_BEGIN_OPEN;
        Check(rows.Call(b).valid, "begin modeled probe");
    };
    const auto probe = [&](bool seen, bool left) {
        PiNativeAttackInput p{};
        p.operation = PI_ATTACK_LEFT;
        auto& q = p.decision.left;
        q.command = PI_AL_PROBE;
        q.flags = PI_AL_MODEL;
        q.count = rows.State().count;
        q.next = rows.State().next;
        q.unseen = rows.State().unseen;
        q.frames = 6;
        if (seen) {
            q.flags |= PI_AL_ENTRY_PRESENT | PI_AL_HAVE_LEFT | PI_AL_HAVE_RIGHT;
            q.left[0] = -1;
            q.right[0] = 1;
            q.point[0] = left ? -1 : 1;
            q.point[2] = .1f;
        }
        const auto result = runtime.Call(p);
        Check(result.valid, "modeled native probe");
        return result;
    };
    begin();
    probe(false, false);
    Check(rows.State().probing && rows.State().unseen == 1 && rows.State().next == 0,
          "first unseen swing retains candidate");
    begin();
    probe(false, false);
    Check(rows.State().unseen == 0 && rows.State().next == 1 && !rows.State().reprobe,
          "second unseen swing advances only for next swing");
    begin();
    probe(true, false);
    Check(rows.State().next == 2 && rows.State().reprobe && !rows.State().verified,
          "wrong side advances for same-window reprobe");
    begin();
    probe(true, true);
    Check(rows.State().next == 2 && rows.State().verified && !rows.State().probing &&
              !rows.State().reprobe,
          "left-side candidate becomes verified and clears probe state");
    config(-1, -1);
    Check(rows.State().count == 0 && rows.State().next == 2 && rows.State().verified &&
              rows.State().whoosh == 778 && rows.State().cost == 20,
          "weapon removal preserves original inactive metadata while clearing candidates");
    runtime.OwnerReset();
    Check(rows.State().key == -1 && rows.State().hand == -1 && rows.State().variation == -1 &&
              rows.State().cost == 20 && rows.State().whoosh == -1 && !rows.State().count &&
              !rows.State().verified,
          "actor reset clears retained candidate model");
    config(101, -1);
    scan(-1, false, 1);
    f = {};
    f.command = PI_ALR_OBSERVE;
    f.index = 3;
    rows.Call(f);
    Check(rows.State().next == 3 && rows.State().verified,
          "native observed candidate becomes retained selection");
    module.Reset();
    Check(!rows.State().count && rows.State().key == -1 && !rows.State().verified,
          "generation reset clears approved model snapshot");
    f = {};
    f.command = PI_ALR_CONFIG;
    f.variation = 101;
    f.hand = -1;
    rows.Call(f);
    Check(!rows.State().count && !rows.State().verified && !rows.State().next,
          "generation reset cleared backend model too");
    Check(!runtime.Faulted(), "retained candidate protocol healthy");
    std::printf("native left rows: %d native-order scans, %d behavior-ref queries; restore, cap, "
                "probe, removal and generation parity\n",
                scans, queries);
}
