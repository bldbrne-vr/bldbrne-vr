// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
void LeftReplay(I::NativeAttackRuntime& runtime, const char* path) {
    namespace Tr = VR::Tricks;
    const auto call = [&](const PiAttackLeftFacts& facts) {
        PiNativeAttackInput p{};
        p.operation = PI_ATTACK_LEFT;
        p.decision.left = facts;
        const auto o = runtime.Call(p);
        Check(o.valid, "left policy boundary");
        return o.left;
    };
    int selects = 0, gates = 0, probes = 0, punches = 0;
    for (int variation : {-1, 100, 101, 110, 300, 1100, 1400})
        for (int style : {-1, 1, 2, 3, 5})
            for (int bid : {-1, 100100000, 101400000, 102000000, 103400000})
                for (int family : {0, 190, 191}) {
                    PiAttackLeftFacts p{};
                    p.command = PI_AL_SELECT;
                    p.variation = variation;
                    p.style = style;
                    p.bid = bid;
                    p.family = family;
                    p.flags = PI_AL_FORM_B;
                    auto o = call(p);
                    const bool dual = variation >= 0 && Tr::LeftPartWeapon(variation);
                    const bool query = !dual && style >= 0 && style != 2 && style != 3;
                    Check(bool(o.flags & PI_ALO_QUERY_LEFT) == query, "ordered left weapon query");
                    p.flags |= PI_AL_LEFT_FACTS | PI_AL_BODY_STATUS;
                    o = call(p);
                    const int v =
                        bid >= 100000000 && bid < 200000000 ? (bid - 100000000) / 1000 : -1;
                    const bool shield = Tr::IsShieldFamily(family);
                    int want = -1, hand = -1, punch = 0;
                    bool gun = false;
                    if (dual)
                        want = variation;
                    else if (query && (Tr::LeftMeleeWeapon(v) || Tr::PunchGun(v) || shield)) {
                        gun = Tr::PunchGun(v) || shield;
                        want = gun ? Tr::FistVariation : v;
                        hand = -2;
                        punch = shield ? -family : gun ? v : 0;
                    }
                    Check(o.variation == want && o.hand == hand && o.gun == gun &&
                              o.punch == punch &&
                              o.key == (want >= 0 ? want * 10 + (hand == -1 ? 1 : 0) : -1),
                          "left weapon/form/shield/fist selection parity");
                    ++selects;
                }
    for (int bits = 0; bits < 4096; ++bits) {
        PiAttackLeftFacts p{};
        p.command = PI_AL_GATE;
        p.flags = PI_AL_GAME_FACTS;
        p.variation = bits & 1 ? 100 : -1;
        p.count = 2;
        p.next = bits & 2 ? 0 : 2;
        p.hand = bits & 4 ? -2 : -1;
        p.weight = bits & 8 ? .49f : .5f;
        const uint32_t masks[] = {PI_AL_ENABLED, PI_AL_PHYSICAL,  PI_AL_GUN,  PI_AL_VERIFIED,
                                  PI_AL_MENU,    PI_AL_CINEMATIC, PI_AL_BODY, PI_AL_DRIVEN,
                                  PI_AL_RUN,     PI_AL_LEFT_IK};
        for (int i = 0; i < 10; ++i)
            if (bits & (1 << i))
                p.flags |= masks[i];
        const auto has = [&](uint32_t v) { return bool(p.flags & v); };
        uint32_t why = !has(PI_AL_ENABLED)                      ? 1
                       : !has(PI_AL_PHYSICAL)                   ? 2
                       : p.variation < 0                        ? 3
                       : p.next >= p.count                      ? 4
                       : !has(PI_AL_VERIFIED) && has(PI_AL_GUN) ? 5
                       : !has(PI_AL_VERIFIED) && p.hand == -2   ? 6
                       : has(PI_AL_MENU)                        ? 8
                       : has(PI_AL_CINEMATIC)                   ? 9
                       : !has(PI_AL_BODY) || !has(PI_AL_DRIVEN) ? 10
                       : has(PI_AL_RUN)                         ? 11
                       : !has(PI_AL_LEFT_IK) || p.weight < .5f  ? 12
                                                                : 0;
        Check(call(p).reason == why, "ordered left eligibility gates");
        ++gates;
    }
    for (int bits = 0; bits < 64; ++bits)
        for (int pct : {0, 50, 100})
            for (int stamina : {-1, 0, 1}) {
                PiAttackLeftFacts p{};
                p.command = PI_AL_ADMIT;
                p.light_pct = pct;
                p.stamina = stamina;
                p.hand = bits & 1 ? -1 : -2;
                p.bid = bits & 2 ? 42 : -1;
                p.flags = (bits & 4 ? PI_AL_VERIFIED : 0) | (bits & 8 ? PI_AL_GUN : 0) |
                          (bits & 16 ? PI_AL_UNPAID : 0) |
                          (bits & 32 ? PI_AL_MODULE | PI_AL_DATA : 0) | PI_AL_REGEN;
                const bool verified = p.flags & PI_AL_VERIFIED, gun = p.flags & PI_AL_GUN;
                const bool probe = !verified && p.hand == -1 && !gun;
                const auto rule = M::AttackStaminaOf(M::MoveKind::Light, pct, true);
                const uint32_t why = !verified && !probe                ? 1
                                     : p.flags & PI_AL_UNPAID           ? 2
                                     : !(p.flags & PI_AL_MODULE)        ? 3
                                     : p.bid < 0                        ? 4
                                     : M::StaminaRefuses(rule, stamina) ? 5
                                                                        : 0;
                const auto o = call(p);
                Check(o.reason == why && bool(o.flags & PI_ALO_PROCEED) == (why == 0) &&
                          bool(o.flags & PI_ALO_PROBE) == (why == 0 && probe) &&
                          o.pct == rule.pct && o.charge == rule.charge,
                      "left admission, probe and stamina parity");
                ++gates;
            }
    for (int bits = 0; bits < 32; ++bits)
        for (uint32_t frames : {0u, 5u, 6u})
            for (uint32_t unseen : {0u, 1u, 255u})
                for (float x : {-.9f, -.5f, 0.f, .5f, .9f, 2.f}) {
                    PiAttackLeftFacts p{};
                    p.command = PI_AL_PROBE;
                    p.flags = (bits & 1 ? PI_AL_ENTRY_PRESENT : 0) |
                              (bits & 2 ? PI_AL_HAVE_LEFT : 0) | (bits & 4 ? PI_AL_HAVE_RIGHT : 0);
                    p.frames = frames;
                    p.unseen = unseen;
                    p.next = 2;
                    p.count = 4;
                    p.point[0] = x;
                    p.point[2] = bits & 8 ? .1f : 0;
                    p.left[0] = -1;
                    p.right[0] = bits & 16 ? -.8f : 1;
                    const B::Vec3 a{x, 0, p.point[2]}, l{-1, 0, 0}, r{p.right[0], 0, 0};
                    const bool seen = (p.flags & PI_AL_ENTRY_PRESENT) &&
                                      (p.flags & PI_AL_HAVE_LEFT) && (p.flags & PI_AL_HAVE_RIGHT) &&
                                      B::IsFinite(a) && B::LengthSq(a) > 0 &&
                                      B::Length(l - r) >= .3f;
                    const auto old = Tr::ProbeStep(seen, seen ? B::Length(a - l) : 0,
                                                   seen ? B::Length(a - r) : 0, frames);
                    const auto o = call(p);
                    uint32_t next = p.next, u = unseen, flags = 0;
                    switch (old) {
                    case Tr::Probe::Wait:
                        break;
                    case Tr::Probe::Learned:
                        u = 0;
                        flags = PI_ALO_LEARN;
                        break;
                    case Tr::Probe::Wrong:
                        u = 0;
                        ++next;
                        flags = PI_ALO_FINALIZE | PI_ALO_REPROBE;
                        break;
                    case Tr::Probe::Unseen:
                        u = static_cast<uint8_t>(u + 1);
                        if (u >= 2) {
                            u = 0;
                            ++next;
                        }
                        flags = PI_ALO_FINALIZE;
                        break;
                    }
                    Check(o.probe == static_cast<uint32_t>(old) && o.next == next &&
                              o.unseen == u && o.flags == flags,
                          "native left row probe, unseen retry and next candidate parity");
                    ++probes;
                    p.command = PI_AL_PUNCH;
                    p.check = 111;
                    if (bits & 8)
                        p.flags |= PI_AL_GUN;
                    const bool have = (p.flags & PI_AL_ENTRY_PRESENT) &&
                                      (p.flags & PI_AL_HAVE_LEFT) && (p.flags & PI_AL_HAVE_RIGHT) &&
                                      B::LengthSq(a) > 0;
                    uint32_t want = 0, reason = 0;
                    if (!have) {
                        if (frames >= 6) {
                            want = PI_ALO_BAD | PI_ALO_FINALIZE;
                            reason = 1;
                        }
                    } else if (B::Length(l - r) < .3f) {
                    } else {
                        const float dl = B::Length(a - l), dr = B::Length(a - r),
                                    reach = p.flags & PI_AL_GUN ? .5f : 1.f;
                        const auto side = Tr::SideOf(dl, dr);
                        if (side == Tr::CapsuleSide::Unsure && dl <= reach) {
                        } else {
                            want = dl <= reach && side == Tr::CapsuleSide::Left
                                       ? PI_ALO_GOOD
                                       : PI_ALO_BAD | PI_ALO_FINALIZE;
                            reason = 2;
                        }
                    }
                    const auto hit = call(p);
                    Check(hit.flags == want && hit.reason == reason,
                          "first-hit reach and fist separation parity");
                    ++punches;
                }
    for (int bits = 0; bits < 16; ++bits) {
        PiAttackLeftFacts p{};
        p.command = PI_AL_OPEN_RESULT;
        p.stamina = 30;
        p.after = bits % 4 == 0 ? 35 : 10;
        p.whoosh = bits & 8 ? 611 : -1;
        p.flags = (bits & 1 ? PI_AL_OK : 0) | (bits & 2 ? PI_AL_DRY_RUN : 0) |
                  (bits & 4 ? PI_AL_WHOOSH : 0);
        const auto o = call(p);
        Check(o.live == bool(bits & 1) &&
                  o.paid == (bits & 1 ? std::max(p.stamina - p.after, 0) : 0) &&
                  bool(o.flags & PI_ALO_DENY) == (!(bits & 1) && !(bits & 2)) &&
                  bool(o.flags & PI_ALO_SOUND) == bool((bits & 1) && (bits & 4) && (bits & 8)),
              "left native outcome and sound query gate");
    }
    for (int bits = 0; bits < 64; ++bits)
        for (bool after_probe : {false, true}) {
            PiAttackLeftFacts f{};
            f.command = PI_AL_REFRESH;
            f.check = 222;
            f.flags = (bits & 1 ? PI_AL_LIVE : 0) | (bits & 2 ? PI_AL_MODULE : 0) |
                      (bits & 4 ? PI_AL_VERIFIED : 0) | (bits & 8 ? PI_AL_PROBING : 0) |
                      (bits & 16 ? PI_AL_DRY_RUN : 0) | (after_probe ? PI_AL_GAME_FACTS : 0);
            uint32_t want = 0;
            if (bits & 1) {
                if (!(bits & 2) || (!(bits & 4) && !(bits & 8)))
                    want = PI_ALO_END;
                else {
                    want = PI_ALO_PROCEED;
                    if (!after_probe) {
                        if (bits & 8)
                            want |= PI_ALO_PROBE;
                    } else {
                        want |= PI_ALO_CHECK_PUNCH;
                        if (!(bits & 16))
                            want |= PI_ALO_INVOKE;
                    }
                }
            }
            Check(call(f).flags == want,
                  "left staged refresh checks probe before first hit and native invocation");
        }
    // An isolated context verifies the original eight-slot cache's overflow alias and
    // survival across actor/context resets, without borrowing any native identity.
    I::Module module;
    Check(module.Load(std::filesystem::absolute(path)), "left cache module");
    I::NativeAttackRuntime retained;
    retained.Bind(module);
    const auto cache_call = [&](PiAttackLeftFacts f) {
        PiNativeAttackInput p{};
        p.operation = PI_ATTACK_LEFT;
        p.decision.left = f;
        return retained.Call(p);
    };
    for (int key = 101; key <= 108; ++key) {
        PiAttackLeftFacts f{};
        f.command = PI_AL_PUNCH;
        f.check = key;
        f.frames = 6;
        Check(cache_call(f).left.flags & PI_ALO_BAD, "cache bad first hit");
    }
    PiAttackLeftFacts f{};
    f.command = PI_AL_GATE;
    f.variation = 100;
    f.count = 1;
    f.check = 999;
    f.hand = -1;
    f.flags = PI_AL_ENABLED | PI_AL_PHYSICAL | PI_AL_VERIFIED;
    Check(cache_call(f).left.reason == 7, "overflow check aliases last session slot");
    PiAttackLeftFacts good{};
    good.command = PI_AL_PUNCH;
    good.check = 999;
    good.flags = PI_AL_ENTRY_PRESENT | PI_AL_HAVE_LEFT | PI_AL_HAVE_RIGHT;
    good.point[0] = good.left[0] = -1;
    good.point[2] = .1f;
    good.right[0] = 1;
    Check(cache_call(good).left.flags & PI_ALO_GOOD, "overflow alias can become verified");
    retained.OwnerReset();
    module.Reset();
    f.check = 108;
    Check(cache_call(f).left.flags & PI_ALO_QUERY_GAME,
          "session check survives reset and respects shared overflow slot");
    f.check = 101;
    Check(cache_call(f).left.reason == 7, "other first-hit refusals survive reset");
    Check(!runtime.Faulted() && !retained.Faulted(), "left policy contexts healthy");
    std::printf("native left: %d selections, %d ordered gates/admissions, %d row probes, %d "
                "first-hit checks; outcome and persistent eight-slot cache\n",
                selects, gates, probes, punches);
}
