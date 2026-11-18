// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include "core/vr/inframe.h"

namespace VR::Sever {
// Bounded worker -> renderer -> pose-hook handoff. No guest/GPU pointers, allocation,
// waits or locks. A token names one cut, not an actor address or a reusable piece slot.
// Models acknowledged here are pinned in each renderer until their first visible use.
class RevealMailbox {
public:
    static constexpr std::size_t Slots = 16;
    static constexpr std::uint32_t MaxModels = 20;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    std::uint32_t Reserve(std::uint32_t models) {
        if (models == 0 || models > MaxModels) return 0;
        const auto serial = next_.fetch_add(1, std::memory_order_relaxed);
        if (serial > 0x0FFFFFFFu) return 0; // never wrap into an older token
        for (std::uint32_t i = 0; i < Slots; ++i) {
            const auto token = static_cast<std::uint32_t>((serial << 4) | i);
            std::uint64_t empty = 0;
            if (slots_[i].compare_exchange_strong(empty, (std::uint64_t(token) << 32) |
                                                            (models << 8),
                                                  std::memory_order_acq_rel)) return token;
        }
        return 0;
    }
    bool Live(std::uint32_t token) const { return Load(token) != 0; }
    bool Complete(std::uint32_t token, std::uint32_t models) const {
        const auto word = Load(token);
        return word != 0 && ((word >> 8) & 255) == models;
    }
    // Still held and not every model in this list: wait. One load: a slot given back meanwhile
    // (the cut drawn, RevealDone) is no wait, never a frame without its pieces (a code review,
    // 2026-11-16: Live, then Complete, with the release between them, skipped the meshes).
    bool Waiting(std::uint32_t token, std::uint32_t models) const {
        const auto word = Load(token);
        return word != 0 && ((word >> 8) & 255) != models;
    }
    bool Ready(std::uint32_t token) const {
        const auto word = Load(token);
        const auto required = (word >> 2) & 3;
        return required != 0 && (word & required) == required;
    }
    bool Visible(std::uint32_t token) const {
        const auto word = Load(token);
        const auto required = (word >> 2) & 3;
        return required != 0 && ((word >> 4) & required) == required;
    }
    // A renderer may not evict this cut's models: staged or revealing, not yet drawn in every
    // active eye. Once visible (or cancelled) they age like any other model, even if a piece
    // flew out of view and is never asked for again (the review, 2026-11-16: pinned for the
    // cut's whole life, culled pieces could fill the model cache). One load.
    bool Pinned(std::uint32_t token) const {
        const auto word = Load(token);
        const auto required = (word >> 2) & 3;
        return word != 0 && !(required != 0 && ((word >> 4) & required) == required);
    }
    // Atomic with cancellation: once claimed, retiring the visual pieces keeps the
    // corpse cut. If retirement wins first, a late pose hook cannot start the hide.
    bool ClaimHide(std::uint32_t token) {
        if (token == 0) return false;
        auto& slot = slots_[token & 15];
        auto word = slot.load(std::memory_order_acquire);
        while ((word >> 32) == token) {
            if ((word & 64) != 0) return true;
            const auto required = (word >> 2) & 3;
            if (required == 0 || (word & required) != required) return false;
            if (slot.compare_exchange_weak(word, word | 64, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    // Only a complete group (all its models) with the draw pipelines ready can ack.
    // Native stereo requires both eyes; mono/DIBR requires the one source renderer.
    void Uploaded(std::uint32_t token, std::uint32_t models,
                  std::uint32_t eye, std::uint32_t required) {
        if (token == 0 || (required != 1 && required != 3) || eye > 1 ||
            (required & (1u << eye)) == 0) return;
        auto& slot = slots_[token & 15];
        auto word = slot.load(std::memory_order_acquire);
        while ((word >> 32) == token && ((word >> 8) & 255) == models) {
            const auto old_required = (word >> 2) & 3;
            const auto eyes = old_required == required ? word & 3 : 0;
            const auto keep = old_required == required ? ~std::uint64_t{15} : ~std::uint64_t{63};
            const auto updated = (word & keep) | (required << 2) |
                                 eyes | (1u << eye);
            if (slot.compare_exchange_weak(word, updated, std::memory_order_acq_rel)) return;
        }
    }
    void Drawn(std::uint32_t token, std::uint32_t models,
               std::uint32_t eye, std::uint32_t required) {
        if (token == 0 || (required != 1 && required != 3) || eye > 1 ||
            (required & (1u << eye)) == 0) return;
        auto& slot = slots_[token & 15];
        auto word = slot.load(std::memory_order_acquire);
        while ((word >> 32) == token && ((word >> 8) & 255) == models &&
               ((word >> 2) & 3) == required && (word & required) == required && (word & 64) != 0) {
            if (slot.compare_exchange_weak(word, word | (std::uint64_t{1} << (eye + 4)),
                                            std::memory_order_acq_rel)) return;
        }
    }
    bool Release(std::uint32_t token) {
        if (token == 0) return false;
        auto& slot = slots_[token & 15];
        auto word = slot.load(std::memory_order_acquire);
        while ((word >> 32) == token) {
            if (slot.compare_exchange_weak(word, 0, std::memory_order_acq_rel)) return (word & 64) != 0;
        }
        return false;
    }
private:
    std::uint64_t Load(std::uint32_t token) const {
        if (token == 0) return 0;
        const auto word = slots_[token & 15].load(std::memory_order_acquire);
        return (word >> 32) == token ? word : 0;
    }
    std::array<std::atomic<std::uint64_t>, Slots> slots_{};
    std::atomic<std::uint64_t> next_{1};
};
inline RevealMailbox g_reveals;

// Oldest pending cuts first, before already-visible effects. New cuts cannot steal
// list space from an older reveal once its hide is committed. Storage slots need not
// be adjacent; the token also keeps both halves together when timestamps coincide.
// Fixed-size insertion sort, only while a reveal is pending; no heap allocation.
template <class Piece, std::size_t N>
std::array<std::size_t, N> RevealOrder(const std::array<Piece, N>& pieces) {
    static_assert(N <= RevealMailbox::Slots);
    std::array<std::size_t, N> order;
    bool pending = false;
    for (std::size_t i = 0; i < N; ++i) {
        order[i] = i;
        pending |= pieces[i].on && !pieces[i].gpu_seen;
    }
    if (!pending) return order;
    const auto before = [&](std::size_t a, std::size_t b) {
        const auto& x = pieces[a];
        const auto& y = pieces[b];
        const bool xp = x.on && !x.gpu_seen, yp = y.on && !y.gpu_seen;
        if (xp != yp) return xp;
        if (!xp) return a < b;
        if (x.born_ns != y.born_ns) return x.born_ns < y.born_ns;
        if (x.reveal != y.reveal) return x.reveal < y.reveal;
        return a < b;
    };
    for (std::size_t i = 1; i < N; ++i) {
        const auto at = order[i];
        std::size_t j = i;
        while (j > 0 && before(at, order[j - 1])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = at;
    }
    return order;
}

// Reserve the same list space as this half's first visible frame, including its initial
// blood burst. Otherwise model-only staging can fit where the actual reveal cannot.
// The placeholder never draws or writes depth; it only participates in list capacity.
inline bool StageHalf(InFrame::List& list, std::span<const std::int32_t> models,
                      std::uint32_t token, bool blood) {
    const auto count = models.size() + (blood ? 1u : 0u);
    if (token == 0 || models.empty() || models.size() > RevealMailbox::MaxModels ||
        list.count > InFrame::MaxItems || count > InFrame::MaxItems - list.count) return false;
    InFrame::Item item{};
    item.group = token;
    item.upload_only = true;
    if (blood) {
        item.mesh = InFrame::MeshId::AugurSpray;
        list.Add(item);
    }
    item.mesh = InFrame::MeshId::Model;
    item.tex = InFrame::Tex::Model;
    item.shade = InFrame::Shade::Model;
    for (const auto id : models) {
        item.tex_key = static_cast<std::uint32_t>(id);
        list.Add(item);
    }
    return true;
}

// One camera record's decision for both halves, even if the pose hook publishes halfway
// through building its list. Fixed scratch storage, only used by pending cuts.
class RevealFrame {
public:
    template <class Read>
    bool Hidden(std::uint32_t token, Read&& read) {
        if (token == 0) return false;
        for (std::size_t i = 0; i < count_; ++i)
            if (entries_[i].token == token) return entries_[i].hidden;
        if (count_ == entries_.size()) return false;
        const bool hidden = read(token);
        entries_[count_++] = {token, hidden};
        return hidden;
    }
private:
    struct Entry { std::uint32_t token{}; bool hidden{}; };
    std::array<Entry, RevealMailbox::Slots> entries_{};
    std::size_t count_{};
};
} // namespace VR::Sever
