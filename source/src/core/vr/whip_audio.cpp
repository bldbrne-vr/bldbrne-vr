// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
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
