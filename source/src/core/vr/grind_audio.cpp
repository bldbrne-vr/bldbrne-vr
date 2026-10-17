// SPDX-License-Identifier: GPL-2.0-or-later
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
