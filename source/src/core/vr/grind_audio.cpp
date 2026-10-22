// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/game_sounds.h"
#include "core/vr/grind_audio.h"
#include "core/vr/seqlock.h"

namespace VR::Grind {
namespace {
SeqLock<GrindFrame> channel;
} // namespace

void PublishGrind(const GrindFrame& frame) {
    channel.Store(frame);
}

GrindFrame LatestGrind() {
    GrindFrame frame{};
    channel.Load(frame, 4);
    return frame;
}

std::span<const char> GrindPcm() {
    return GameSounds::GrindLoop();
}

bool GrindReady() {
    return !GameSounds::GrindLoop().empty();
}

} // namespace VR::Grind
