// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/lever_audio.h"
#include "core/vr/seqlock.h"

namespace VR::LeverAudio {
namespace {
SeqLock<Frame> channel;
}

void PublishPull(std::int64_t now) {
    static Frame frame{}; // the game thread is the only writer
    ++frame.id;
    frame.ns = now;
    channel.Store(frame);
}

Frame Latest() {
    // A bounded seqlock read can overlap a publication: keep this thread's last good frame (a
    // reset id would replay the last pull).
    thread_local Frame frame{};
    channel.Load(frame, 4);
    return frame;
}

} // namespace VR::LeverAudio
