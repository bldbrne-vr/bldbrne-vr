// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include "common/alignment.h"
#include "common/types.h"

struct AVFrame;

namespace Libraries::Videodec {

struct NV12Layout {
    u32 width, height, pitch, padded_height;

    static constexpr NV12Layout From(int w, int h) {
        if (w <= 0 || h <= 0) {
            return {};
        }
        return {u32(w), u32(h), Common::AlignUp<u32>(w, 64), Common::AlignUp<u32>(h, 16)};
    }

    constexpr u64 Bytes() const {
        return u64(pitch) * (padded_height + padded_height / 2);
    }

    constexpr bool Fits(u64 capacity, size_t trailer = 0) const {
        return width && trailer <= capacity && Bytes() <= capacity - trailer;
    }

    bool ValidPlanes(const u8* y, int y_stride, const u8* uv, int uv_stride) const {
        const auto wide_enough = [](int stride, u32 bytes) {
            return (stride < 0 ? -s64(stride) : s64(stride)) >= bytes;
        };
        return y && uv && wide_enough(y_stride, width) && wide_enough(uv_stride, (width + 1) & ~1u);
    }

    // The player owns its whole frame and initializes cropped row padding. VideoDec's
    // caller owns its output and retains the existing contents of horizontal padding.
    template <bool fill_padding>
    void Copy(u8* dst, const u8* y, int y_stride, const u8* uv, int uv_stride) const {
        const auto plane = [this](u8* out, const u8* in, int stride, u32 row_bytes, u32 rows,
                                  u32 padded_rows, u8 padding) {
            if (row_bytes == pitch && stride == pitch && stride > 0) {
                std::memcpy(out, in, size_t(pitch) * rows);
                out += size_t(pitch) * rows;
            } else {
                for (u32 i = 0; i < rows; ++i, out += pitch) {
                    const u8* line = in + s64(i) * stride;
                    if constexpr (fill_padding) {
                        if (row_bytes != pitch) {
                            // Padding is confined to the last 64-byte block. A fixed fill
                            // inlines; the visible bytes in that block are copied just below.
                            std::memset(out + pitch - 64, padding, 64);
                            // Inline wide copies for pitched rows. The last block may overlap
                            // its predecessor in the output, but both stay inside the pixels.
                            constexpr u32 block = 256;
                            if (row_bytes >= block) {
                                const u32 last = row_bytes - block;
                                for (u32 x = 0; x < last; x += block) {
                                    std::memcpy(out + x, line + x, block);
                                }
                                std::memcpy(out + last, line + last, block);
                                continue;
                            }
                        }
                    }
                    std::memcpy(out, line, row_bytes);
                }
            }
            if (rows < padded_rows) {
                const u8* last = out - pitch;
                const u32 bytes = fill_padding ? pitch : row_bytes;
                for (u32 i = rows; i < padded_rows; ++i, out += pitch) {
                    std::memcpy(out, last, bytes);
                }
            }
        };
        plane(dst, y, y_stride, width, height, padded_height, 16);
        plane(dst + size_t(pitch) * padded_height, uv, uv_stride, (width + 1) & ~1u,
              (height + 1) / 2, padded_height / 2, 128);
    }
};

// The second decoder stores its picture info immediately after the pixels.
u64 CopyNV12Data(u8* dst, u64 capacity, const AVFrame& src, size_t trailer = 0);

} // namespace Libraries::Videodec
