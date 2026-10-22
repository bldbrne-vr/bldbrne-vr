// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include "common/logging/log.h"
#include "common/types.h"

// FAST_DIAG_SKIP_FILE (diagnostic only, off unless set): draws whose vertex or pixel program hash
// (the full hash or its low 32 bits, hex, one per word; '#' starts a comment) is listed in that
// text file are left out, like bit 40's skip rule leaves out a draw whose pipeline compiles. The
// file is re-read when it changes (checked at most every 250 ms), so a running session can
// bisect which draw makes an artifact without a restart.
namespace DiagSkip {

inline const char* const g_path = std::getenv("FAST_DIAG_SKIP_FILE");

inline bool Enabled() {
    return g_path != nullptr;
}

struct State {
    std::mutex mutex;
    std::unordered_set<u64> hashes;
    std::unordered_set<u64> logged;
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point next{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void Reload(State& s) {
    const auto now = std::chrono::steady_clock::now();
    if (now < s.next)
        return;
    s.next = now + std::chrono::milliseconds(250);
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(g_path, error);
    if (error) {
        if (!s.hashes.empty())
            LOG_INFO(Render_Vulkan, "VR diag skip: {} is gone, nothing is left out", g_path);
        s.hashes.clear();
        s.stamp = {};
        return;
    }
    if (stamp == s.stamp)
        return;
    s.stamp = stamp;
    s.hashes.clear();
    s.logged.clear();
    std::ifstream file(g_path);
    std::string word;
    while (file >> word) {
        if (word.starts_with('#')) {
            std::getline(file, word);
            continue;
        }
        try {
            s.hashes.insert(std::stoull(word, nullptr, 16));
        } catch (...) {
        }
    }
    LOG_INFO(Render_Vulkan, "VR diag skip: {} program hashes from {}", s.hashes.size(), g_path);
}

template <typename Stages>
bool Skip(const Stages& stages) {
    if (!Enabled())
        return false;
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    Reload(s);
    if (s.hashes.empty())
        return false;
    for (const auto* stage : stages) {
        if (!stage)
            continue;
        const u64 hash = stage->pgm_hash;
        if (s.hashes.contains(hash) || s.hashes.contains(hash & 0xFFFF'FFFFull)) {
            if (s.logged.insert(hash).second)
                LOG_INFO(Render_Vulkan, "VR diag skip: leaving out the draws of program {:#x}",
                         hash);
            return true;
        }
    }
    return false;
}

} // namespace DiagSkip
