// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// VR_PERF_PROBE (tests): which images the image -> buffer resync handles (vr_perf.h), summed per
// image and reader shader, the busiest listed in the 5 s 'VR perf probe: resyncs' line.
#pragma once

#include "common/types.h"

namespace Vulkan::Perf {

enum class ResyncOutcome : u32 { Full = 0, Skipped = 1, SkippableOff = 2, ImageSameBufferWritten = 3 };

struct ResyncProbe {
    u64 address;
    u32 width, height, format, bits, size;
    bool tiled;
    u64 reader;    // the shader whose bindings asked for it
    u64 writer;    // the shader of the image's last write
    u64 draws_ago; // draws since that write
};

void NoteResync(const ResyncProbe& p, ResyncOutcome outcome);

} // namespace Vulkan::Perf
