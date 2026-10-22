// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <cmrc/cmrc.hpp>
#include "core/vr/gun_reload_audio.h"
CMRC_DECLARE(res);
namespace VR::Gun {
namespace { SeqLock<ReloadAudioFrame> channel; }
void PublishReloadAudio(ReloadCue cue, bool enabled, std::int64_t now) {
    static ReloadAudioFrame frame{}; // presenter is the only writer
    for (auto* voice : {&frame.reload, &frame.click}) {
        voice->ns = now; voice->enabled = enabled; voice->pan = -0.25f;
    }
    if (enabled && cue != ReloadCue::None) {
        auto& voice = cue == ReloadCue::Reload ? frame.reload : frame.click;
        ++voice.clink_id; voice.clink_ns = now;
    }
    channel.Store(frame);
}
ReloadAudioFrame LatestReloadAudio() {
    // A bounded seqlock read can overlap publication. Preserve this audio thread's
    // last good frame instead of resetting one-shot IDs and replaying the same cue.
    thread_local ReloadAudioFrame frame{};
    channel.Load(frame, 4);
    return frame;
}
std::span<const char> ReloadPcm() {
    const auto file = cmrc::res::get_filesystem().open("src/resources/vr/gun_reload.pcm");
    return {file.begin(), file.size()};
}
std::span<const char> EmptyPcm() {
    const auto file = cmrc::res::get_filesystem().open("src/resources/vr/gun_empty.pcm");
    return {file.begin(), file.size()};
}
} // namespace VR::Gun
