// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// SparsePool's arithmetic (sparse_pool.h), without Vulkan: pieces of slabs, block alignment and
// the merging of a view's blocks into as few binds as their pieces allow.
namespace VideoCore::SparseLogic {

inline constexpr std::uint64_t BlockBits = 16; // 64 KB: the sparse block (RTX 5080 probe, bbport)
inline constexpr std::uint64_t BlockSize = std::uint64_t{1} << BlockBits;
inline constexpr std::uint64_t SlabSize = std::uint64_t{32} << 20;
inline constexpr std::uint32_t PiecesPerSlab = static_cast<std::uint32_t>(SlabSize / BlockSize);

// A piece: its slab and its index in the slab.
inline constexpr std::uint32_t Piece(std::uint32_t slab, std::uint32_t index) {
    return slab * PiecesPerSlab + index;
}
inline constexpr std::uint32_t SlabOf(std::uint32_t piece) {
    return piece / PiecesPerSlab;
}
inline constexpr std::uint64_t OffsetIn(std::uint32_t piece) {
    return std::uint64_t{piece % PiecesPerSlab} * BlockSize;
}

inline constexpr std::uint64_t AlignDown(std::uint64_t value) {
    return value & ~(BlockSize - 1);
}
inline constexpr std::uint64_t AlignUp(std::uint64_t value) {
    return (value + BlockSize - 1) & ~(BlockSize - 1);
}

// One bind: `blocks` blocks of the view from its block `first`, to the slab's memory from piece
// `piece` on.
struct Run {
    std::uint64_t first{};
    std::uint64_t blocks{};
    std::uint32_t piece{};
    bool operator==(const Run&) const = default;
};

// The view's pieces, block by block, as runs: consecutive blocks on consecutive pieces of one
// slab make one bind.
inline std::vector<Run> Runs(const std::vector<std::uint32_t>& pieces) {
    std::vector<Run> runs;
    for (std::uint64_t i = 0; i < pieces.size(); ++i) {
        const std::uint32_t piece = pieces[i];
        if (!runs.empty()) {
            Run& last = runs.back();
            const std::uint32_t next = last.piece + static_cast<std::uint32_t>(last.blocks);
            if (last.first + last.blocks == i && piece == next &&
                SlabOf(piece) == SlabOf(last.piece)) {
                ++last.blocks;
                continue;
            }
        }
        runs.push_back({.first = i, .blocks = 1, .piece = piece});
    }
    return runs;
}

// Which pieces the guest blocks have and which buffers bind them (SparsePool's bookkeeping, no
// Vulkan). Views bind a block's left piece; twins bind its right piece where the right eye has
// one, else the left (shared). Every buffer keeps the list of pieces it bound and gives exactly
// that list back, so views and twins may go in any order (their cleanups run on different
// threads): a piece returns to the pool when no buffer binds it any more. A block no view binds
// is forgotten at once, its right piece with it: a later view gets new pieces and never an old
// view's bytes (an old twin keeps its pieces until it goes). New pieces come from `take` (nothing
// when VRAM has no slab); a failed call changes nothing.
class Book {
public:
    static constexpr std::uint32_t None = ~std::uint32_t{0};
    using Take = std::function<std::optional<std::uint32_t>()>;
    using Give = std::function<void(std::uint32_t)>;

    Book(Take take_, Give give_) : take{std::move(take_)}, give{std::move(give_)} {}

    // A view of blocks [first, first + count): each block's left piece (a new one where none).
    std::optional<std::vector<std::uint32_t>> BindView(std::uint64_t first, std::uint64_t count) {
        std::vector<std::uint32_t> pieces(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            auto [chunk, block] = Slot(first + i, true);
            if (block->views == 0) {
                const auto piece = take();
                if (!piece) {
                    // Undo this call: the views counted so far, the pieces taken so far.
                    Tidy(chunk);
                    ReleaseView(first, {pieces.begin(), pieces.begin() + i});
                    return std::nullopt;
                }
                block->left = *piece;
                ++chunk->live;
            }
            ++block->views;
            ++RefSlot(block->left);
            pieces[i] = block->left;
        }
        return pieces;
    }

    // A view of blocks from `first` is gone: its blocks lose it; a block no view binds is
    // forgotten (its right piece too); then its pieces lose a binding.
    void ReleaseView(std::uint64_t first, const std::vector<std::uint32_t>& pieces) {
        for (std::uint64_t i = 0; i < pieces.size(); ++i) {
            auto [chunk, block] = Slot(first + i, false);
            if (block == nullptr || block->views == 0) {
                continue; // never in a well-formed sequence
            }
            if (--block->views == 0) {
                const std::uint32_t right = block->right;
                *block = Block{};
                --chunk->live;
                Tidy(chunk);
                if (right != None) {
                    // The block is gone: its right piece belongs to no block any more (a twin
                    // still binding it gives it back through Release).
                    ForgetOwner(right);
                    if (Refs(right) == 0) {
                        give(right); // made for a twin that never bound it
                    }
                }
            }
        }
        Release(pieces);
    }

    // A twin of a view of blocks [first, first + count): the right piece where a block has one,
    // else its left piece (shared). Bit i of `own` set where block i binds a right piece.
    std::vector<std::uint32_t> BindTwin(std::uint64_t first, std::uint64_t count,
                                        std::vector<std::uint64_t>& own) {
        own.assign((count + 63) / 64, 0);
        std::vector<std::uint32_t> pieces(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            const Block& block = Live(first + i);
            if (block.right != None) {
                own[i / 64] |= std::uint64_t{1} << (i % 64);
                pieces[i] = block.right;
            } else {
                pieces[i] = block.left;
            }
            ++RefSlot(pieces[i]);
        }
        return pieces;
    }

    // Right pieces for the blocks of [first, first + count) without one; their numbers to
    // `fresh`. All or nothing: false, and nothing changed, when a piece could not be taken.
    bool Privatize(std::uint64_t first, std::uint64_t count, std::vector<std::uint64_t>& fresh) {
        const std::size_t before = fresh.size();
        for (std::uint64_t b = first; b < first + count; ++b) {
            Block& block = Live(b);
            if (block.right != None) {
                continue;
            }
            const auto piece = take();
            if (!piece) {
                Unprivatize({fresh.begin() + before, fresh.end()});
                fresh.resize(before);
                return false;
            }
            block.right = *piece;
            SetOwner(*piece, b);
            fresh.push_back(b);
        }
        return true;
    }

    // Undo Privatize's new right pieces (no twin could be made to bind them).
    void Unprivatize(const std::vector<std::uint64_t>& fresh) {
        for (const std::uint64_t b : fresh) {
            Block* const block = Slot(b, false).second;
            if (block == nullptr || block->views == 0 || block->right == None) {
                continue;
            }
            ForgetOwner(block->right);
            if (Refs(block->right) == 0) {
                give(block->right);
            }
            block->right = None;
        }
    }

    // A twin is gone (or any buffer's pieces): each piece loses a binding.
    void Release(const std::vector<std::uint32_t>& pieces) {
        for (const std::uint32_t piece : pieces) {
            if (Refs(piece) == 0) {
                continue; // never in a well-formed sequence
            }
            if (--refs[piece] == 0) {
                // The last twin binding a block's right piece went (a twin released, as at a
                // transition): the right eye's own bytes go with it, the block shares again.
                if (piece < owner.size() && owner[piece] != NoBlock) {
                    Block* const block = Slot(owner[piece], false).second;
                    if (block != nullptr && block->views != 0 && block->right == piece) {
                        block->right = None;
                    }
                    ForgetOwner(piece);
                }
                give(piece);
            }
        }
    }

    [[nodiscard]] std::uint32_t Refs(std::uint32_t piece) const {
        return piece < refs.size() ? refs[piece] : 0;
    }
    [[nodiscard]] std::size_t RightPieces() const {
        return right_pieces;
    }
    [[nodiscard]] bool HasBlock(std::uint64_t block) const {
        const Block* const b = Find(block);
        return b != nullptr && b->views != 0;
    }
    [[nodiscard]] std::uint32_t RightOf(std::uint64_t block) const {
        const Block* const b = Find(block);
        return b == nullptr || b->views == 0 ? None : b->right;
    }

private:
    // A block exists while a view binds it (views != 0); a free slot is Block{}.
    struct Block {
        std::uint32_t left{None};
        std::uint32_t right{None};
        std::uint32_t views{};
    };
    // Blocks in chunks of 4096 (256 MiB of guest memory), made when a view first reaches one and
    // freed when its last block goes; one hash lookup per chunk, the last chunk cached.
    static constexpr std::uint64_t ChunkBits = 12;
    static constexpr std::uint64_t ChunkMask = (std::uint64_t{1} << ChunkBits) - 1;
    struct Chunk {
        std::array<Block, std::size_t{1} << ChunkBits> blocks{};
        std::uint32_t live{}; // its blocks with a view
    };
    static constexpr std::uint64_t NoBlock = ~std::uint64_t{0};

    std::pair<Chunk*, Block*> Slot(std::uint64_t b, bool make) {
        const std::uint64_t key = b >> ChunkBits;
        if (cached == nullptr || cached_key != key) {
            auto it = chunks.find(key);
            if (it == chunks.end()) {
                if (!make) {
                    return {nullptr, nullptr};
                }
                it = chunks.emplace(key, std::make_unique<Chunk>()).first;
            }
            cached = it->second.get();
            cached_key = key;
        }
        return {cached, &cached->blocks[b & ChunkMask]};
    }
    [[nodiscard]] const Block* Find(std::uint64_t b) const {
        const std::uint64_t key = b >> ChunkBits;
        if (cached != nullptr && cached_key == key) {
            return &cached->blocks[b & ChunkMask];
        }
        const auto it = chunks.find(key);
        return it == chunks.end() ? nullptr : &it->second->blocks[b & ChunkMask];
    }
    // As unordered_map::at: a block no view binds is an error.
    Block& Live(std::uint64_t b) {
        Block* const block = Slot(b, false).second;
        if (block == nullptr || block->views == 0) {
            throw std::out_of_range("SparseLogic::Book: a block no view binds");
        }
        return *block;
    }
    // A chunk with no live block goes (the cache with it).
    void Tidy(Chunk* chunk) {
        if (chunk->live != 0) {
            return;
        }
        std::uint64_t key = cached_key;
        if (cached == chunk) {
            cached = nullptr;
        } else {
            for (const auto& [k, value] : chunks) { // rare: a chunk other than the last one used
                if (value.get() == chunk) {
                    key = k;
                    break;
                }
            }
        }
        chunks.erase(key);
    }
    std::uint32_t& RefSlot(std::uint32_t piece) {
        if (piece >= refs.size()) {
            refs.resize(std::max<std::size_t>(piece + 1, refs.size() * 2), 0);
        }
        return refs[piece];
    }
    void SetOwner(std::uint32_t piece, std::uint64_t block) {
        if (piece >= owner.size()) {
            owner.resize(std::max<std::size_t>(piece + 1, owner.size() * 2), NoBlock);
        }
        if (owner[piece] == NoBlock) {
            ++right_pieces;
        }
        owner[piece] = block;
    }
    void ForgetOwner(std::uint32_t piece) {
        if (piece < owner.size() && owner[piece] != NoBlock) {
            owner[piece] = NoBlock;
            --right_pieces;
        }
    }

    Take take;
    Give give;
    std::unordered_map<std::uint64_t, std::unique_ptr<Chunk>> chunks;
    Chunk* cached{};
    std::uint64_t cached_key{};
    std::vector<std::uint32_t> refs;  // by piece: the buffers binding it
    std::vector<std::uint64_t> owner; // by piece: the block a right piece belongs to, else NoBlock
    std::size_t right_pieces{};
};

} // namespace VideoCore::SparseLogic
