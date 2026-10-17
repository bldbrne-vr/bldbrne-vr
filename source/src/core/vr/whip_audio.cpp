// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmrc/cmrc.hpp>
#include "core/vr/seqlock.h"
#include "core/vr/whip_audio.h"

CMRC_DECLARE(res);
namespace VR::Whip {
namespace {
SeqLock<RecallAudioFrame> channel;
}
void PublishRecallAudio(RecallSound sound, bool enabled, float pan, std::int64_t now) {
    static RecallAudioFrame frame{}; // game thread only; serial survives weapon changes
    frame.ns = now;
    frame.enabled = enabled;
    frame.drag = sound.drag;
    frame.pan = pan;
    if (sound.clink && enabled) {
        ++frame.clink_id;
        frame.clink_ns = now;
    }
    channel.Store(frame);
}
RecallAudioFrame LatestRecallAudio() {
    RecallAudioFrame frame{};
    channel.Load(frame, 4);
    return frame;
}
std::span<const char> RecallClinkPcm() {
    const auto file = cmrc::res::get_filesystem().open("src/resources/vr/beast_chain_clink.pcm");
    return {file.begin(), file.size()};
}
} // namespace VR::Whip
