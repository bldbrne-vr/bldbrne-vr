// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// FAST_CPU_FUSED_PAIR (NativeStereo::PairCarried): a draw's descriptor writes paired for both eyes
// from what its binding carried, instead of looking every descriptor up in the registry under its
// lock. Header-only so tests/test_fused_pair.cpp runs it on mock descriptors against a model of
// NativeStereo::PairDescriptors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace NativeStereo::Fused {

enum class Kind : std::uint8_t { Buffer, Image, Sampler, Other };

// Index of `p` in [base, base + count), or `count` when it points elsewhere.
template <class T>
std::size_t IndexIn(const T* p, const T* base, std::size_t count) {
    const auto offset = reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(base);
    return offset % sizeof(T) == 0 && offset / sizeof(T) < count ? offset / sizeof(T) : count;
}

// The binding's info arrays (every write points into them) and what it carried for each info:
// buffer_hints (left handle, registry entry, own camera pair's right) and view_hints (left, right
// view) run parallel to a prefix of them.
template <class BufferInfo, class ImageInfo, class BufferHint, class ViewHint>
struct Bound {
    const BufferInfo* buffers{};
    std::size_t buffer_count{};
    std::span<const BufferHint> buffer_hints;
    const ImageInfo* images{};
    std::size_t image_count{};
    std::span<const ViewHint> view_hints;
};

// Pairs `writes` (copies of the binding's) in place, in PairDescriptors' order and with its rules,
// so whatever `ops` records (a right buffer's creation, a stream mirror) lands where it did:
// - a storage buffer descriptor becomes [left, right] in `out_buffers`: right is the carried
//   entry's right buffer (created first when the binding writes it and has none), the left one
//   when it has none; a stream buffer read through its right copy mirrors the read range; a
//   descriptor that carried its camera pair takes that pair's right. One that carried nothing
//   (no hint for its handle, or no entry) asks `ops` for the registry's answer and the camera scan.
// - a sampled or storage image descriptor becomes [left, right] in `out_images`, right the
//   carried view's right or the registry's (`ops.LookupView`).
// - with `fold`, a sampler is copied into `out_images` unpaired.
// Returns false when an array would overflow; `ops.Overflow()` decides (nothing recorded yet).
template <class Write, class BufferInfo, class ImageInfo, class BufferHint, class ViewHint,
          class Ops>
bool Pair(std::span<Write> writes, const Bound<BufferInfo, ImageInfo, BufferHint, ViewHint>& bound,
          std::span<const std::uint8_t> writable, std::span<BufferInfo> out_buffers,
          std::span<ImageInfo> out_images, std::size_t& buffers_used, std::size_t& images_used,
          bool fold, Ops& ops) {
    std::size_t ob = 0, oi = 0;
    for (auto& w : writes) {
        const Kind kind = ops.KindOf(w);
        const std::size_t n = w.descriptorCount;
        if (kind == Kind::Buffer) {
            if (ob + 2 * n > out_buffers.size()) {
                return ops.Overflow();
            }
            const bool written = w.dstBinding < writable.size() && writable[w.dstBinding];
            BufferInfo* const first = out_buffers.data() + ob;
            for (std::size_t i = 0; i < n; ++i) {
                const BufferInfo left = w.pBufferInfo[i];
                BufferInfo right = left;
                if (left.buffer) {
                    const std::size_t index =
                        IndexIn(w.pBufferInfo + i, bound.buffers, bound.buffer_count);
                    const BufferHint* const hint = index < bound.buffer_hints.size() &&
                                                           bound.buffer_hints[index].left ==
                                                               left.buffer
                                                       ? &bound.buffer_hints[index]
                                                       : nullptr;
                    bool stream = false;
                    if (hint && hint->entry) {
                        auto have = ops.RightOf(*hint->entry);
                        if (!have && written) {
                            have = ops.Create(left.buffer, *hint->entry);
                        }
                        if (have) {
                            right.buffer = have;
                        }
                        stream = ops.Stream(*hint->entry);
                    } else {
                        right.buffer = ops.Lookup(left.buffer, written, stream);
                    }
                    // Stream buffers receive CPU writes directly; mirror only the read range.
                    if (right.buffer != left.buffer && !written && stream) {
                        ops.Mirror(left, right.buffer);
                    }
                    if (!hint) {
                        ops.ScanCamera(left, right);
                    } else if (hint->camera.buffer) {
                        right = hint->camera;
                    }
                }
                first[2 * i] = left;
                first[2 * i + 1] = right;
            }
            ob += 2 * n;
            w.descriptorCount = static_cast<decltype(w.descriptorCount)>(2 * n);
            w.pBufferInfo = first;
        } else if (kind == Kind::Image) {
            if (oi + 2 * n > out_images.size()) {
                return ops.Overflow();
            }
            ImageInfo* const first = out_images.data() + oi;
            for (std::size_t i = 0; i < n; ++i) {
                const ImageInfo left = w.pImageInfo[i];
                ImageInfo right = left;
                const std::size_t index = IndexIn(w.pImageInfo + i, bound.images, bound.image_count);
                if (index < bound.view_hints.size() &&
                    bound.view_hints[index].left == left.imageView &&
                    (bound.view_hints[index].right || !left.imageView)) {
                    right.imageView = bound.view_hints[index].right;
                } else {
                    right.imageView = ops.LookupView(left.imageView);
                }
                first[2 * i] = left;
                first[2 * i + 1] = right;
            }
            oi += 2 * n;
            w.descriptorCount = static_cast<decltype(w.descriptorCount)>(2 * n);
            w.pImageInfo = first;
        } else if (fold && kind == Kind::Sampler) {
            if (oi + n > out_images.size()) {
                return ops.Overflow();
            }
            ImageInfo* const first = out_images.data() + oi;
            for (std::size_t i = 0; i < n; ++i) {
                first[i] = w.pImageInfo[i];
            }
            oi += n;
            w.pImageInfo = first;
        }
    }
    buffers_used = ob;
    images_used = oi;
    return true;
}

} // namespace NativeStereo::Fused
