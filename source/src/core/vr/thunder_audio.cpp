// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/vr/seqlock.h"
#include "core/vr/thunder_audio.h"

namespace VR::ThunderAudio {
namespace {
SeqLock<ClipAudio::Frame> clap;
} // namespace

void Play(std::int64_t now) {
    static ClipAudio::Frame frame{}; // the presenter's frame thread is the only writer
    ++frame.id;
    frame.ns = now;
    frame.state = ClipAudio::PlayOut;
    clap.Store(frame);
}

ClipAudio::Frame Latest() {
    // A bounded seqlock read can overlap a publication: keep this thread's last good frame (a
    // reset id would replay the last start).
    thread_local ClipAudio::Frame frame{};
    clap.Load(frame, 4);
    return frame;
}

} // namespace VR::ThunderAudio
