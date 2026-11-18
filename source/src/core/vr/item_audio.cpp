// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include "core/vr/item_audio.h"
#include "core/vr/seqlock.h"

namespace VR::ItemAudio {
namespace {
std::array<SeqLock<ClipAudio::Frame>, ClipCount> channels;
}

void Play(Clip clip, std::int64_t now, float volume, float pitch) {
    if (clip >= ClipCount) {
        return;
    }
    static std::array<ClipAudio::Frame, ClipCount> frames{}; // the render tick is the only writer
    auto& frame = frames[clip];
    ++frame.id;
    frame.ns = now;
    frame.state = ClipAudio::PlayOut;
    frame.volume = volume;
    frame.pitch = pitch;
    channels[clip].Store(frame);
}

ClipAudio::Frame Latest(Clip clip) {
    // A bounded seqlock read can overlap a publication: keep this thread's last good frame (a
    // reset id would replay the last start).
    thread_local std::array<ClipAudio::Frame, ClipCount> frames{};
    if (clip >= ClipCount) {
        return {};
    }
    channels[clip].Load(frames[clip], 4);
    return frames[clip];
}

} // namespace VR::ItemAudio
