// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Whether memory reached through a game structure can be read, asked before it is read: a stale
// or odd pointer that passes Hook::Plausible (in range, aligned) may still point at nothing, and
// reading it would end the game. For reads on a rare path (a cut's pose at its killing blow), never
// per frame: each region not yet seen costs one system query (about a microsecond); the last few
// found are remembered, so a skeleton's arrays and its names cost one or two. One object per read
// (what it remembers is only as fresh as that read); one thread.
namespace VR {

class ReadableMemory {
public:
    // Whether all of [p, p + size) can be read now (size 0: yes).
    bool Has(const void* p, std::size_t size) {
        return Span(p, size) == size;
    }
    // How many bytes from `p` can be read now, up to `max`: to the first byte that cannot.
    std::size_t Span(const void* p, std::size_t max);

    struct Region {
        std::uintptr_t lo{0}, hi{0}; // [lo, hi); empty: none
    };
    // The readable region holding `at` right now (committed, readable, not a guard page), or none.
    // Not on Windows: all of it (the reads stay as Hook::Plausible allows).
    static Region At(std::uintptr_t at);

private:
    static constexpr std::size_t Known = 8;
    static constexpr int MaxRegions = 8; // one read across more than these: not read
    std::array<Region, Known> known_{};
    std::size_t next_{0};
};

} // namespace VR
