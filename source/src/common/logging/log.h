// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <iostream>
#include <unordered_map>
#include <vector>
#include <spdlog/details/fmt_helper.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/wincolor_sink.h>
using spdlog_stdout = spdlog::sinks::sink;
#else
using spdlog_stdout = spdlog::sinks::stdout_color_sink_mt;
#endif
#include <spdlog/spdlog.h>

#include "common/logging/classes.h"
#include "common/path_util.h"
#include "common/thread.h"

namespace Common::Log {
extern bool g_should_append;
extern std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS;

void Setup(std::string_view shadps4_filename);

void Switch(std::string_view game_filename);

void Shutdown();

void Flush();

void Terminate();

void UpdateSinks();

void UpdateLogLevels(std::string_view log_filter);
void UpdateLogFlushLevel(std::string_view log_flush_level);

// VR_LOCK_TRACE (profiling): called with each log line's write time (ns, on the writing thread)
// once set; the sinks are wrapped for it only when VR_LOCK_TRACE is set at startup.
using SinkTimingHook = void (*)(long long ns);
void SetSinkTimingHook(SinkTimingHook hook);

// With the synchronous logger (log.sync, the default): the lines this thread logs while a
// DeferScope is alive are kept and written, in their order, when its outermost DeferScope ends.
// For the holders of the Vulkan queue lock (Scheduler::submit_mutex): declared BEFORE the lock
// guard, it ends after the lock is released, so a console write (0.5-2 ms a line) no longer holds
// up every thread waiting for the queue. An Error or Critical line is never kept: it writes the
// kept lines and itself at once (a crash's last lines stay in order). Nothing is kept with the
// asynchronous logger, which never blocks.
class DeferScope {
public:
    DeferScope();
    ~DeferScope();
    DeferScope(const DeferScope&) = delete;
    DeferScope& operator=(const DeferScope&) = delete;
};

// With the synchronous logger: this thread's lines are handed to a background writer (in their
// order, to the same sinks) instead of being written on the thread. For a thread with a deadline
// every few milliseconds (the headset's frame thread at 90-120 Hz), which the console write of its
// 5 s report lines (13-23 ms) made miss a refresh. An Error or Critical line waits (200 ms at most)
// until the writer has taken it, except inside a DeferScope (the queue lock is held there). Call it
// on the thread itself; it lasts for the thread's life.
void WriteThisThreadInBackground();

static constexpr std::array level_string_views{"Trace", "Debug",    "Info", "Warning",
                                               "Error", "Critical", "Off"};

[[nodiscard]] static constexpr std::string_view to_string_view(spdlog::level lvl) noexcept {
    return level_string_views.at(level_to_number(lvl));
}

// The play build (CMake BBVR_NO_LOGS): every log line compiles to nothing. Its arguments are still
// type-checked (and count as used) but never evaluated, and log.cpp creates no logger, console
// output or log file.
#ifdef BBVR_NO_LOGS
inline constexpr bool Enabled = false;
#else
inline constexpr bool Enabled = true;
#endif
// Only launcher benchmark children opt into sparse measurement records in release builds.
bool BenchmarkEnabled() noexcept;

template <typename... Args>
constexpr void Discard(std::string_view, const Args&...) noexcept {}
} // namespace Common::Log

// Define the fmt lib macros
#ifdef BBVR_NO_LOGS
#define LOG_GENERIC(log_class, log_level, format, ...)                                             \
    do {                                                                                           \
        if (false) {                                                                               \
            Common::Log::Discard(format, ##__VA_ARGS__);                                           \
        }                                                                                          \
    } while (false)
#else
// The level is checked before the arguments are evaluated: a filtered-out line (LOG_DEBUG at the
// default level) must not pay for GetCurrentThreadName or its format arguments.
#define LOG_GENERIC(log_class, log_level, format, ...)                                             \
    do {                                                                                           \
        if (auto logger = Common::Log::ALL_LOGGERS[log_class];                                     \
            logger && logger->should_log(log_level)) {                                             \
            logger->log(log_level, "[{}] <{}> ({}) {}:{} {}: " format, log_class,                  \
                        Common::Log::to_string_view(log_level), Common::GetCurrentThreadName(),    \
                        spdlog::source_loc::basename(__FILE__), __LINE__,                          \
                        std::string_view(__func__) == "operator()" ? "lambda" : __func__,          \
                        ##__VA_ARGS__);                                                            \
        }                                                                                          \
    } while (false)
#endif

#ifdef NDEBUG
#define LOG_TRACE(log_class, ...) (void(0))
#else
#define LOG_TRACE(log_class, ...)                                                                  \
    LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::trace, __VA_ARGS__)
#endif

#define LOG_DEBUG(log_class, ...)                                                                  \
    LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...)                                                                   \
    LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...)                                                                \
    LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::warn, __VA_ARGS__)
#define LOG_ERROR(log_class, ...)                                                                  \
    LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::err, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...)                                                               \
    LOG_GENERIC(Common::Log::Class::log_class, spdlog::level::critical, __VA_ARGS__)

// These selected evidence records survive BBVR_NO_LOGS. Normal release play neither
// evaluates their arguments nor initializes logging. General logging stays compiled out.
#ifdef BBVR_NO_LOGS
#define LOG_BENCHMARK(log_class, log_level, format, ...) \
    do { \
        if (Common::Log::Enabled || Common::Log::BenchmarkEnabled()) { \
            if (auto logger = Common::Log::ALL_LOGGERS[Common::Log::Class::log_class]; \
                logger && logger->should_log(log_level)) { \
                logger->log(log_level, format, ##__VA_ARGS__); \
            } \
        } \
    } while (false)
#else
#define LOG_BENCHMARK(log_class, log_level, ...) \
    LOG_GENERIC(Common::Log::Class::log_class, log_level, __VA_ARGS__)
#endif
#define LOG_BENCHMARK_INFO(log_class, ...) \
    LOG_BENCHMARK(log_class, spdlog::level::info, __VA_ARGS__)
#define LOG_BENCHMARK_WARNING(log_class, ...) \
    LOG_BENCHMARK(log_class, spdlog::level::warn, __VA_ARGS__)
