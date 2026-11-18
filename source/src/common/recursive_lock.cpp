// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <unordered_map>
#include "common/assert.h"
#include "common/recursive_lock.h"

namespace Common::Detail {

struct RecursiveLockState {
    RecursiveLockType type;
    int count;
};

// The recursive locks this thread holds: a few at a time (a mutex held, its count above 0). Since
// 2026-11-17 a small table of its own before the map: the map's node was allocated at every
// outermost lock and freed at its release (Rasterizer::IsMapped, every draw of the temporal motion:
// ~0.4% of stage B in the Forbidden Woods). The map takes a mutex only while the table is full.
// The answers are the same: a mutex is in the table or the map, never both, and an entry leaves
// either when its count reaches 0.
struct RecursiveLockEntry {
    void* mutex; // nullptr: a free entry
    RecursiveLockState state;
};
constexpr size_t RecursiveLockSlots = 8;
struct RecursiveLocks {
    std::array<RecursiveLockEntry, RecursiveLockSlots> table{};
    std::unordered_map<void*, RecursiveLockState> overflow;
};
thread_local RecursiveLocks g_recursive_locks;

bool IncrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto& locks = g_recursive_locks;
    RecursiveLockEntry* free_entry = nullptr;
    for (auto& entry : locks.table) {
        if (entry.mutex == mutex) {
            ASSERT(entry.state.type == type);
            return entry.state.count++ == 0;
        }
        if (entry.mutex == nullptr && free_entry == nullptr) {
            free_entry = &entry;
        }
    }
    if (locks.overflow.empty() || !locks.overflow.contains(mutex)) {
        if (free_entry != nullptr) {
            *free_entry = {mutex, {type, 1}};
            return true;
        }
    }
    auto& state = locks.overflow[mutex];
    if (state.count == 0) {
        ASSERT(state.type == RecursiveLockType::None);
        state.type = type;
    }
    ASSERT(state.type == type);
    return state.count++ == 0;
}

bool DecrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto& locks = g_recursive_locks;
    for (auto& entry : locks.table) {
        if (entry.mutex == mutex) {
            ASSERT(entry.state.type == type && entry.state.count > 0);
            if (--entry.state.count == 0) {
                entry = {};
                return true;
            }
            return false;
        }
    }
    auto& state = locks.overflow[mutex];
    ASSERT(state.type == type && state.count > 0);
    if (--state.count == 0) {
        locks.overflow.erase(mutex);
        return true;
    }
    return false;
}

} // namespace Common::Detail
