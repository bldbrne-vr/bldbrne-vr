// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Single-writer sequence lock for small trivially copyable records shared between threads (the
// OpenXR frame thread publishing hand poses, the render-camera hook publishing the body anchor; the
// game's threads read them). The writer never waits and readers never block it: a reader retries
// while a store is in progress and gives up after a few tries (the caller keeps its last value).
// The payload lives in relaxed atomic words with the fences of Boehm's seqlock ("Can seqlocks get
// along with programming language memory models?", 2012), so a torn read is detected, never used,
// and there is no data race in the C++ sense. On x86-64 every access is a plain move.
namespace VR {

template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock payloads are copied as bytes");
    static constexpr std::size_t Words = (sizeof(T) + 7) / 8;

public:
    // The writer thread only (one writer).
    void Store(const T& value) {
        std::array<std::uint64_t, Words> words{};
        std::memcpy(words.data(), &value, sizeof(T));
        const std::uint64_t sequence = sequence_.load(std::memory_order_relaxed);
        sequence_.store(sequence + 1, std::memory_order_relaxed); // odd: a store is in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < Words; ++i) {
            words_[i].store(words[i], std::memory_order_relaxed);
        }
        sequence_.store(sequence + 2, std::memory_order_release);
    }

    // Any thread. False before the first Store, or when every try overlapped a store (`out` is
    // then left as it was).
    bool Load(T& out, int tries = 64) const {
        for (int attempt = 0; attempt < tries; ++attempt) {
            const std::uint64_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1) != 0) {
                continue;
            }
            std::array<std::uint64_t, Words> words;
            for (std::size_t i = 0; i < Words; ++i) {
                words[i] = words_[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) != before) {
                continue; // torn: a store ran meanwhile
            }
            if (before == 0) {
                return false;
            }
            std::memcpy(&out, words.data(), sizeof(T));
            return true;
        }
        return false;
    }

    // Completed stores so far.
    [[nodiscard]] std::uint64_t Count() const {
        return sequence_.load(std::memory_order_acquire) / 2;
    }

private:
    std::atomic<std::uint64_t> sequence_{0};
    std::array<std::atomic<std::uint64_t>, Words> words_{};
};

} // namespace VR
