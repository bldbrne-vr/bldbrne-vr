// SPDX-License-Identifier: GPL-2.0-or-later

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
