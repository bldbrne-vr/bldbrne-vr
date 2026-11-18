// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/vr/seqlock.h"
#include "core/vr/whistle_audio.h"

namespace VR::WhistleAudio {
namespace {
SeqLock<Frame> channel;
}

void Publish(std::uint64_t id, std::uint8_t state, std::int64_t now) {
    channel.Store({id, state, now}); // the presenter is the only writer
}

Frame Latest() {
    // A bounded seqlock read can overlap a publication: keep this thread's last good frame.
    thread_local Frame frame{};
    channel.Load(frame, 4);
    return frame;
}

} // namespace VR::WhistleAudio
