// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmrc/cmrc.hpp>
#include "core/vr/tool_empty_audio.h"
#include "core/vr/seqlock.h"
CMRC_DECLARE(res);
namespace VR::ToolEmptyAudio {
namespace { SeqLock<ClipAudio::Frame> channel; }
void Publish(bool failed, bool enabled, std::int64_t now) {
    static Cue cue; // presenter owns publication
    channel.Store(cue.Update(failed, enabled, now));
}
ClipAudio::Frame Latest() {
    thread_local ClipAudio::Frame frame{};
    channel.Load(frame, 4); // retain the last good ID on a concurrent publication
    return frame;
}
std::span<const char> Pcm() {
    const auto file = cmrc::res::get_filesystem().open("src/resources/vr/tool_empty.pcm");
    return {file.begin(), file.size()};
}
} // namespace VR::ToolEmptyAudio
