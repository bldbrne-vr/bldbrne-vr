// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_utils.h"

#include <libavutil/frame.h>

namespace Libraries::Videodec {

u64 CopyNV12Data(u8* dst, u64 capacity, const AVFrame& src, size_t trailer) {
    const auto layout = NV12Layout::From(src.width, src.height);
    if (!dst || !layout.Fits(capacity, trailer) ||
        !layout.ValidPlanes(src.data[0], src.linesize[0], src.data[1], src.linesize[1])) {
        return 0;
    }
    layout.Copy<false>(dst, src.data[0], src.linesize[0], src.data[1], src.linesize[1]);
    return layout.Bytes();
}

} // namespace Libraries::Videodec
