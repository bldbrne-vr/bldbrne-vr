// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/logging/log.h"
#include "core/vr/live_tune.h"

namespace VR::Tune {

namespace {

struct State {
    std::mutex mutex;
    std::string path;
    std::unordered_map<std::string, float> values;
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point checked{};
};

// 0 = not decided, 1 = off, 2 = on.
std::atomic<int> g_mode{0};

State& S() {
    static State* state = new State; // never destroyed (any thread may ask at exit)
    return *state;
}

int Mode() {
    int mode = g_mode.load(std::memory_order_relaxed);
    if (mode == 0) {
        const char* const path = std::getenv("VR_TUNE_FILE");
        mode = path != nullptr && path[0] != 0 ? 2 : 1;
        if (mode == 2) {
            std::scoped_lock lock{S().mutex};
            S().path = path;
        }
        g_mode.store(mode, std::memory_order_relaxed);
        if (mode == 2) {
            LOG_INFO(Core, "VR tune: live values from {}", path);
        }
    }
    return mode;
}

// Under the mutex: the file again when it changed.
void Refresh(State& s) {
    const auto now = std::chrono::steady_clock::now();
    if (now - s.checked < std::chrono::milliseconds(250)) {
        return;
    }
    s.checked = now;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(s.path, ec);
    if (ec || stamp == s.stamp) {
        return;
    }
    s.stamp = stamp;
    std::FILE* const f = std::fopen(s.path.c_str(), "r");
    if (f == nullptr) {
        return;
    }
    std::unordered_map<std::string, float> values;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string text{line};
        if (const size_t hash = text.find('#'); hash != std::string::npos) {
            text.resize(hash);
        }
        const size_t eq = text.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = text.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
            key.erase(key.begin());
        }
        char* end = nullptr;
        const float value = std::strtof(text.c_str() + eq + 1, &end);
        if (!key.empty() && end != text.c_str() + eq + 1) {
            values[key] = value;
        }
    }
    std::fclose(f);
    LOG_INFO(Core, "VR tune: {} values", values.size());
    s.values = std::move(values);
}

} // Anonymous namespace

bool Active() {
    return Mode() == 2;
}

float Get(const char* key, float fallback) {
    if (Mode() != 2) {
        return fallback;
    }
    State& s = S();
    std::scoped_lock lock{s.mutex};
    Refresh(s);
    const auto it = s.values.find(key);
    return it != s.values.end() ? it->second : fallback;
}

} // namespace VR::Tune
