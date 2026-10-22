// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Compact owned register updates for the scratch ordered recording pipeline.
#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ScratchDraw {
template<size_t WordCount, size_t BlockWords = 32>
class RegisterDeltaTracker {
    static_assert(WordCount > 0 && BlockWords > 0);
    static constexpr size_t Blocks = (WordCount + BlockWords - 1) / BlockWords;
    static_assert(Blocks <= 65536);
    std::bitset<Blocks> dirty;
    std::vector<uint16_t> pending;
    bool reset{};
    struct Checksums {
        std::array<uint64_t, Blocks> blocks{};
        std::bitset<Blocks> dirty;
        std::vector<uint16_t> pending;
        uint64_t total{};
        Checksums() { pending.reserve(Blocks); }
    };
    // Allocated only by the speculative path. Transport Capture must not consume
    // checksum dirtiness: Stage B can capture register deltas between two draws.
    std::unique_ptr<Checksums> checksums;
    template<class Hash>
    static uint64_t HashBlock(std::span<const uint32_t, WordCount> source,
                              size_t block, Hash hash) {
        const size_t first = block * BlockWords;
        // Include the position in each block contribution (identical blocks at
        // different register addresses must not cancel).
        const uint64_t value = hash(source.subspan(first, std::min(BlockWords, WordCount - first)));
        uint64_t mixed = value ^ ((block + 1) * 0x9e3779b97f4a7c15ULL);
        mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
        mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
        return mixed ^ (mixed >> 31);
    }
public:
    struct Block {
        uint16_t index;
        std::array<uint32_t, BlockWords> words{};
    };
    using Packet = std::vector<Block>;
    static constexpr size_t NumBlocks = Blocks;
    static constexpr size_t WordsPerBlock = BlockWords;
    RegisterDeltaTracker() { pending.reserve(Blocks); MarkAll(); }
    void MarkAll() { Mark(0, WordCount); }
    // Block checksums of one fixed state (ClearState defaults): adopting them replaces
    // rehashing every block (bbport regs_reset). Only valid while the registers equal it.
    struct ResetChecksums {
        std::array<uint64_t, Blocks> blocks{};
        uint64_t total{};
    };
    template<class Hash>
    static std::unique_ptr<ResetChecksums> MakeReset(std::span<const uint32_t, WordCount> source, Hash hash) {
        auto result = std::make_unique<ResetChecksums>();
        for (size_t block = 0; block < Blocks; ++block) {
            result->blocks[block] = HashBlock(source, block, hash);
            result->total ^= result->blocks[block];
        }
        return result;
    }
    void AdoptChecksums(const ResetChecksums& reset) {
        if (!checksums) checksums = std::make_unique<Checksums>();
        checksums->blocks = reset.blocks;
        checksums->total = reset.total;
        checksums->pending.clear();
        checksums->dirty.reset();
    }
    // Transport: full file (MarkAll); checksums: the precomputed state.
    void MarkAll(const ResetChecksums& reset) {
        for (size_t block = 0; block < Blocks; ++block) {
            if (!dirty.test(block)) {
                pending.push_back(static_cast<uint16_t>(block));
                dirty.set(block);
            }
        }
        AdoptChecksums(reset);
    }
    // ClearState with the source now equal to the consumer's defaults (bbport regs.h:203
    // RegDirty::reset): pending transport blocks are superseded and the next Capture reports
    // a reset. Checksums adopt `precomputed` when given, else rehash every block as MarkAll.
    void Reset(const ResetChecksums* precomputed = nullptr) {
        if (precomputed)
            AdoptChecksums(*precomputed);
        else if (checksums)
            for (size_t block = 0; block < Blocks; ++block)
                if (!checksums->dirty.test(block)) {
                    checksums->pending.push_back(static_cast<uint16_t>(block));
                    checksums->dirty.set(block);
                }
        dirty.reset();
        pending.clear();
        reset = true;
    }
    void Mark(size_t first, size_t count) {
        if (first > WordCount || count > WordCount - first)
            throw std::out_of_range("register delta write range");
        if (!count) return;
        for (size_t block = first / BlockWords; block <= (first + count - 1) / BlockWords; ++block) {
            if (checksums && !checksums->dirty.test(block)) {
                checksums->pending.push_back(static_cast<uint16_t>(block));
                checksums->dirty.set(block);
            }
            if (!dirty.test(block)) {
                pending.push_back(static_cast<uint16_t>(block));
                dirty.set(block);
            }
        }
    }
    template<class Hash>
    static uint64_t FullChecksum(std::span<const uint32_t, WordCount> source, Hash hash) {
        uint64_t result = 0;
        for (size_t block = 0; block < Blocks; ++block) result ^= HashBlock(source, block, hash);
        return result;
    }
    template<class Hash>
    uint64_t Checksum(std::span<const uint32_t, WordCount> source, Hash hash) {
        if (!checksums) {
            checksums = std::make_unique<Checksums>();
            for (size_t block = 0; block < Blocks; ++block) {
                checksums->blocks[block] = HashBlock(source, block, hash);
                checksums->total ^= checksums->blocks[block];
            }
        } else {
            for (auto block : checksums->pending) {
                const auto value = HashBlock(source, block, hash);
                checksums->total ^= checksums->blocks[block] ^ value;
                checksums->blocks[block] = value;
            }
            checksums->pending.clear();
            checksums->dirty.reset();
        }
        return checksums->total;
    }
    // Returns true when the consumer must reset to defaults before applying `packet`.
    bool Capture(std::span<const uint32_t, WordCount> source, Packet& packet) {
        packet.clear();
        packet.reserve(pending.size());
        for (auto index : pending) {
            Block block{index};
            const size_t first = size_t(index) * BlockWords;
            std::copy_n(source.data() + first, std::min(BlockWords, WordCount - first), block.words.data());
            packet.push_back(block);
        }
        dirty.reset();
        pending.clear();
        return std::exchange(reset, false);
    }
    static void Apply(std::span<uint32_t, WordCount> destination, std::span<const Block> packet) {
        for (const auto& block : packet) {
            const size_t first = size_t(block.index) * BlockWords;
            if (first >= WordCount) throw std::out_of_range("register delta block");
            std::copy_n(block.words.data(), std::min(BlockWords, WordCount - first), destination.data() + first);
        }
    }
};
}
