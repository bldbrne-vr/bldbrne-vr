// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "core/vr/sever_enemy.h"
#include "core/vr/sever_mesh.h"
#include "core/vr/sever_policy.h"

// Beast Claw dismemberment, the jobs: every
// cut from its lethal hit to its retirement, bounded and safe to abandon at any point.
//  - Reserve, then submit: a lethal hit first reserves a slot by its mixed meshes' count and
//    size alone (cheap, under the lock); only an admitted cut copies its snapshot of the frozen
//    pose (sever_enemy.h), outside the lock, and submits it. A refused kill never copied anything.
//  - A job owns everything it needs (the snapshot, the bone mask, the plane); nothing here reads
//    or keeps a guest pointer. The actor key only names the corpse.
//  - Fixed slots (MaxSlots); one cut per corpse (a remembered ring): a second heavy on the same
//    body is never a second job.
//  - Memory: a reservation holds the cut's peak (EstimateBytes); the result's real size
//    (sized up front by the cutter) replaces it.
//    Every byte is released with its slot; nothing is ever freed while the lock is held (the
//    released buffers go after it, Trash), so the game thread never waits on a big free.
//  - Tickets (slot + serial + world): a slot reused, a corpse despawned or the world reset (lamp
//    travel, a soft-save load, quitting) makes every older ticket stale: a late submit or cut is
//    thrown away, a late Take finds nothing. The world's reset is one counter bump.
//  - The cut runs outside the lock (RunNext); a cut that fails or throws frees its slot.
namespace VR::Sever {

// One corpse: the character (its address as a number, never read through) and its model, so a
// reused character slot holding another enemy is another key.
struct ActorKey {
    std::uint64_t actor{0};
    std::int32_t model{-1};
    friend constexpr bool operator==(const ActorKey&, const ActorKey&) = default;
};

struct Ticket {
    std::uint16_t slot{0xFFFF};
    std::uint32_t serial{0};
    constexpr bool Valid() const {
        return slot != 0xFFFF && serial != 0;
    }
};

enum class Refusal : std::uint8_t { None, Severed, Busy, TooBig, Budget, Invalid, Stale };

constexpr const char* RefusalName(Refusal r) {
    switch (r) {
    case Refusal::None:
        return "admitted";
    case Refusal::Severed:
        return "this corpse was cut (or tried) before";
    case Refusal::Busy:
        return "every cut slot busy";
    case Refusal::TooBig:
        return "the mesh is over an enemy's limits";
    case Refusal::Budget:
        return "over the memory budget";
    case Refusal::Invalid:
        return "no corpse, no limb, or a snapshot over its reservation";
    case Refusal::Stale:
        return "the reservation is gone (despawned, reset or retired)";
    }
    return "?";
}

struct Admit {
    Ticket ticket{};
    Refusal why{Refusal::Invalid};
};

class Jobs {
public:
    static constexpr std::size_t MaxSlots = 4;      // cuts reserved, queued, running, ready, shown
    static constexpr std::size_t Remembered = 64;   // corpses already cut (or tried)
    // Bytes, every slot together: the Scourge Beast (c1000) crosses 11 meshes of fur at the neck,
    // its worst case (EstimateBytes) about 26 MB.
    static constexpr std::uint64_t DefaultBudget = 64ull << 20;

    explicit Jobs(std::uint64_t budget_bytes = DefaultBudget) : budget_{budget_bytes} {}

    // The game thread, at the lethal hit (after Decide said Sever): a slot for this corpse's cut
    // of `parts` mixed meshes this size in all, its peak memory held, or why not. Nothing is
    // copied yet. Admitted, the corpse is remembered: a refusal from here on (a failed submit or
    // cut) is never retried.
    Admit Reserve(const ActorKey& key, Limb limb, std::uint32_t parts, std::uint32_t vertices,
                  std::uint32_t triangles) {
        std::scoped_lock lock{mutex_};
        if (key.actor == 0 || limb == Limb::None) {
            return {{}, Refusal::Invalid};
        }
        if (KnownLocked(key)) {
            return {{}, Refusal::Severed};
        }
        if (parts == 0 || vertices == 0 || triangles == 0) {
            return {{}, Refusal::Invalid};
        }
        if (parts > MaxParts || vertices > MaxCutVertices || triangles > 2 * MaxCutVertices) {
            Remember(key); // the same mesh would be refused again
            return {{}, Refusal::TooBig};
        }
        const auto free = std::find_if(slots_.begin(), slots_.end(),
                                       [](const Slot& s) { return s.state == State::Free; });
        if (free == slots_.end()) {
            return {{}, Refusal::Busy}; // not remembered: a later kill may find a free slot
        }
        const std::uint64_t estimate = EstimateBytes(parts, vertices, triangles);
        if (estimate > Room()) {
            return {{}, Refusal::Budget}; // nor this: the budget frees as pieces retire
        }
        Slot& s = *free;
        s.state = State::Reserved;
        s.serial = NextSerial();
        s.world = world_;
        s.key = key;
        s.limb = limb;
        s.parts = parts;
        s.vertices = vertices;
        s.triangles = triangles;
        s.bytes = estimate;
        used_ += estimate;
        Remember(key);
        return {{static_cast<std::uint16_t>(free - slots_.begin()), s.serial}, Refusal::None};
    }

    // The game thread, its snapshot taken (outside the lock): the reserved cut is queued for the
    // worker. A stale ticket (the corpse gone, the world reset) or a snapshot over what was
    // reserved is refused; the snapshot then stays the caller's, and an oversized one frees the
    // reservation (that corpse is not cut).
    Refusal Submit(Ticket t, Snapshot&& input) {
        Trash trash;
        std::scoped_lock lock{mutex_};
        Slot* s = Find(t);
        if (s == nullptr || s->state != State::Reserved) {
            return Refusal::Stale;
        }
        std::uint64_t vertices = 0, triangles = 0;
        bool fits = !input.parts.empty() && input.parts.size() <= s->parts;
        for (const Part& p : input.parts) {
            vertices += p.vertices.size();
            triangles += p.triangles.size();
            fits = fits && p.vertices.size() <= EnemyLimits.input_vertices &&
                   p.triangles.size() <= EnemyLimits.input_triangles;
        }
        if (!fits || vertices > s->vertices || triangles > s->triangles) {
            ReleaseLocked(*s, trash);
            return Refusal::Invalid;
        }
        // A snapshot built with spare capacity costs more than its counts said.
        const std::uint64_t reserved = SnapshotBytes(s->parts, s->vertices, s->triangles);
        const std::uint64_t need = EstimateBytes(s->parts, s->vertices, s->triangles) +
                                   (Bytes(input) - std::min(Bytes(input), reserved)) +
                                   ClothExtraBytes(input);
        if (need > s->bytes) {
            if (need - s->bytes > Room()) {
                ReleaseLocked(*s, trash);
                return Refusal::Budget;
            }
            used_ += need - s->bytes;
            s->bytes = need;
        }
        s->input = std::move(input);
        s->state = State::Queued;
        return Refusal::None;
    }

    // The worker: the oldest queued job, cut with `cut(snapshot)` (CutEnemy, or a test's)
    // outside the lock. False when there was none. A job that went stale meanwhile (despawned,
    // reset, its slot reused) is thrown away; one that failed, threw or outgrew the budget frees
    // its slot (its corpse stays remembered).
    template <class CutFn>
    bool RunNext(CutFn&& cut) {
        Snapshot input;
        std::uint16_t index = 0xFFFF;
        std::uint32_t serial = 0;
        {
            std::scoped_lock lock{mutex_};
            std::uint32_t oldest = 0;
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                const Slot& s = slots_[i];
                if (s.state == State::Queued && (index == 0xFFFF || s.serial < oldest)) {
                    index = static_cast<std::uint16_t>(i);
                    oldest = s.serial;
                }
            }
            if (index == 0xFFFF) {
                return false;
            }
            Slot& s = slots_[index];
            s.state = State::Cutting;
            serial = s.serial;
            input = std::move(*s.input);
            s.input.reset();
        }
        EnemyCut result{};
        try {
            result = cut(std::as_const(input));
        } catch (...) {
            result = EnemyCut{}; // a cut that throws is a failed cut (Error::InvalidInput)
        }
        input = {}; // the snapshot's memory goes now, outside the lock
        Trash trash; // (destroyed after the lock below: a stale result goes outside it too)
        std::scoped_lock lock{mutex_};
        Slot& s = slots_[index];
        if (s.state != State::Cutting || s.serial != serial || s.world != world_) {
            return true; // stale: the slot was released (and maybe reused) meanwhile
        }
        if (!result) {
            failed_ = result.error;
            ReleaseLocked(s, trash);
            return true;
        }
        const std::uint64_t actual = Bytes(result);
        used_ -= std::min(used_, s.bytes);
        s.bytes = 0;
        if (actual > Room()) {
            failed_ = Error::Budget;
            ReleaseLocked(s, trash);
            return true;
        }
        s.bytes = actual;
        used_ += actual;
        s.output = std::move(result);
        s.state = State::Ready;
        return true;
    }

    // The renderer's adapter: a ready cut's meshes (moved out) to upload; the slot stays
    // accounted (Shown) until Retire. Nothing for a stale, unready or already taken ticket.
    std::optional<EnemyCut> Take(Ticket t) {
        std::scoped_lock lock{mutex_};
        Slot* s = Find(t);
        if (s == nullptr || s->state != State::Ready || !s->output) {
            return std::nullopt;
        }
        std::optional<EnemyCut> out = std::move(s->output);
        s->output.reset();
        s->state = State::Shown;
        return out;
    }

    // Done with this cut, whatever its state: a reservation never submitted (the snapshot could
    // not be taken), a cut abandoned, a shown piece gone (its GPU memory retired). Its slot and
    // memory are free; a stale ticket does nothing.
    void Retire(Ticket t) {
        Trash trash;
        std::scoped_lock lock{mutex_};
        if (Slot* s = Find(t)) {
            ReleaseLocked(*s, trash);
        }
    }

    // The corpse is gone (despawned, its death cleaned up): its job, wherever it was, ends.
    // Still remembered: a reused character slot is a different key only by its model.
    void Despawn(const ActorKey& key) {
        Trash trash;
        std::scoped_lock lock{mutex_};
        for (Slot& s : slots_) {
            if (s.state != State::Free && s.key == key) {
                ReleaseLocked(s, trash);
            }
        }
    }

    // A cut corpse's character alive again under its key (a lamp's rest brought the enemy back in
    // the same character slot): it can be cut again. Its jobs are untouched.
    void Forget(const ActorKey& key) {
        std::scoped_lock lock{mutex_};
        std::replace(remembered_.begin(), remembered_.end(), key, ActorKey{});
    }

    // The world went away (lamp travel, a soft-save load, a death's reload, quitting): every job
    // and every memory of a cut corpse ends; every older ticket is stale.
    void Reset() {
        Trash trash;
        std::scoped_lock lock{mutex_};
        ++world_;
        for (Slot& s : slots_) {
            ReleaseLocked(s, trash);
        }
        remembered_.fill({});
        remembered_next_ = 0;
    }

    struct Stats {
        std::uint64_t used_bytes{0}, budget_bytes{0};
        std::size_t reserved{0}, queued{0}, cutting{0}, ready{0}, shown{0};
        Error last_failure{Error::None};
    };
    Stats StatsNow() const {
        std::scoped_lock lock{mutex_};
        Stats st{used_, budget_};
        for (const Slot& s : slots_) {
            st.reserved += s.state == State::Reserved;
            st.queued += s.state == State::Queued;
            st.cutting += s.state == State::Cutting;
            st.ready += s.state == State::Ready;
            st.shown += s.state == State::Shown;
        }
        st.last_failure = failed_;
        return st;
    }

    bool Known(const ActorKey& key) const {
        std::scoped_lock lock{mutex_};
        return KnownLocked(key);
    }

    // A cut of this corpse in flight: reserved, queued, being cut or its result waiting.
    bool Busy(const ActorKey& key) const {
        std::scoped_lock lock{mutex_};
        return std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) {
            return s.state != State::Free && s.key == key;
        });
    }

private:
    enum class State : std::uint8_t { Free, Reserved, Queued, Cutting, Ready, Shown };
    struct Slot {
        State state{State::Free};
        std::uint32_t serial{0};
        std::uint32_t world{0};
        ActorKey key{};
        Limb limb{Limb::None};
        std::uint32_t parts{0}, vertices{0}, triangles{0}; // as reserved
        std::uint64_t bytes{0};
        std::optional<VR::Sever::Snapshot> input;
        std::optional<EnemyCut> output;
    };
    // What releases free, held until the caller's lock is let go (declared before the lock, so
    // destroyed after it): no big free ever happens under the lock.
    struct Trash {
        std::array<std::optional<VR::Sever::Snapshot>, MaxSlots> inputs;
        std::array<std::optional<EnemyCut>, MaxSlots> outputs;
        std::size_t count{0};
    };

    static std::uint64_t SnapshotBytes(std::uint64_t parts, std::uint64_t vertices,
                                       std::uint64_t triangles) {
        return parts * sizeof(Part) + vertices * sizeof(Vertex) + triangles * sizeof(Triangle) +
               MaxBones;
    }
    std::uint64_t Room() const {
        return budget_ - std::min(budget_, used_);
    }
    std::uint32_t NextSerial() {
        serial_ = serial_ == 0xFFFFFFFFu ? 1 : serial_ + 1;
        return serial_;
    }
    Slot* Find(Ticket t) {
        if (!t.Valid() || t.slot >= slots_.size()) {
            return nullptr;
        }
        Slot& s = slots_[t.slot];
        return s.state != State::Free && s.serial == t.serial && s.world == world_ ? &s : nullptr;
    }
    // (A Cutting slot's snapshot is the worker's: it finds the slot released and drops its cut,
    // outside the lock. Until then that cut is outside the budget: one job's peak at most.)
    void ReleaseLocked(Slot& s, Trash& trash) {
        used_ -= std::min(used_, s.bytes);
        if (trash.count < trash.inputs.size()) {
            trash.inputs[trash.count] = std::move(s.input);
            trash.outputs[trash.count] = std::move(s.output);
            ++trash.count;
        }
        s = Slot{};
    }
    bool KnownLocked(const ActorKey& key) const {
        return std::find(remembered_.begin(), remembered_.end(), key) != remembered_.end();
    }
    void Remember(const ActorKey& key) {
        if (!KnownLocked(key)) {
            remembered_[remembered_next_] = key;
            remembered_next_ = (remembered_next_ + 1) % remembered_.size();
        }
    }

    mutable std::mutex mutex_;
    std::array<Slot, MaxSlots> slots_{};
    std::array<ActorKey, Remembered> remembered_{};
    std::size_t remembered_next_{0};
    std::uint64_t budget_;
    std::uint64_t used_{0};
    std::uint32_t serial_{0};
    std::uint32_t world_{0};
    Error failed_{Error::None};
};

} // namespace VR::Sever
