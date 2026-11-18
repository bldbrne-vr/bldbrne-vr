// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/vr/seqlock.h"
#include "core/vr/tonitrus_charge.h"

namespace VR::TonitrusCharge {
namespace {
SeqLock<State> latest;
} // namespace

void Publish(const State& state) {
    latest.Store(state);
}

bool Latest(State& out) {
    return latest.Load(out, 4);
}

} // namespace VR::TonitrusCharge
