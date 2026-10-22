// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <fmt/std.h>
#include <spdlog/sinks/async_sink.h>
#include <spdlog/sinks/dup_filter_sink.h>
#ifdef _WIN32
#include <Windows.h>
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/logging/log_file_sink.h"
#include "common/types.h"
#include "core/emulator_settings.h"

// return codes above 'standard'
// https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes
enum class ShadPs4ReturnCode : u32 {
    TERMINATE_WITHOUT_EXCEPTION = 20'000,
    TERMINATE_WITH_EXCEPTION = 20'001,
    TERMINATE_WITH_UNKNOWN_EXCEPTION = 20'002,
};

namespace Common::Log {
bool g_should_append = false;
bool BenchmarkEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("BBVR_BENCHMARK");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

namespace {
std::atomic<SinkTimingHook> g_sink_timing_hook{nullptr};

// VR_LOCK_TRACE: times every write of the sink it wraps (the hook adds it to the writing thread's
// queue-lock hold). Installed only when VR_LOCK_TRACE is set.
class TimedSink final : public spdlog::sinks::sink {
public:
    explicit TimedSink(spdlog::sink_ptr inner_) : inner{std::move(inner_)} {}
    void log(const spdlog::details::log_msg& msg) override {
        const SinkTimingHook hook = g_sink_timing_hook.load(std::memory_order_relaxed);
        if (hook == nullptr) {
            inner->log(msg);
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        inner->log(msg);
        hook(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
    }
    void flush() override {
        inner->flush();
    }
    void set_pattern(const std::string& pattern) override {
        inner->set_pattern(pattern);
    }
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
        inner->set_formatter(std::move(formatter));
    }

private:
    spdlog::sink_ptr inner;
};

bool SinkTimingWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("VR_LOCK_TRACE");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return wanted;
}

// DeferScope (log.h): the synchronous logger's sink in front of each logger's sinks. Writes a line
// through to `inner` at once, or, while this thread is inside a DeferScope, keeps a copy (the
// line's text is on the caller's stack) until the outermost scope ends.
class DeferSink;
struct KeptLine {
    std::shared_ptr<DeferSink> sink;
    spdlog::details::async_log_msg msg;
};
thread_local u32 t_defer_depth = 0;
thread_local std::vector<KeptLine> t_kept;
thread_local std::shared_ptr<DeferSink> t_flush_kept; // a flush asked for while deferring
thread_local bool t_background = false;               // WriteThisThreadInBackground

class DeferSink final : public spdlog::sinks::sink,
                        public std::enable_shared_from_this<DeferSink> {
public:
    DeferSink(std::vector<spdlog::sink_ptr> inner_,
              std::shared_ptr<spdlog::sinks::async_sink> background_)
        : inner{std::move(inner_)}, background{std::move(background_)} {}
    void log(const spdlog::details::log_msg& msg) override {
        if (t_background && background) {
            background->log(msg); // queued as an owning copy, written by its thread in order
            // Not inside a DeferScope: there this thread holds the queue lock, and wait_all polls
            // in 5 ms sleeps.
            if (msg.log_level >= spdlog::level::err && t_defer_depth == 0) {
                (void)background->wait_all(std::chrono::milliseconds(200));
            }
            return;
        }
        if (t_defer_depth > 0 && msg.log_level < spdlog::level::err) {
            t_kept.push_back({shared_from_this(), spdlog::details::async_log_msg{
                                                      spdlog::details::async_log_msg::type::log,
                                                      msg}});
            return;
        }
        WriteKept(); // an Error / Critical inside a scope: the lines before it first
        Write(msg);
    }
    void flush() override {
        if (t_background && background) {
            background->flush(); // queued after this thread's lines
            return;
        }
        if (t_defer_depth > 0) {
            t_flush_kept = shared_from_this();
            return;
        }
        FlushInner();
    }
    void set_pattern(const std::string& pattern) override {
        for (auto& sink : inner) {
            sink->set_pattern(pattern);
        }
    }
    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
        for (std::size_t i = 0; i < inner.size(); ++i) {
            inner[i]->set_formatter(i + 1 < inner.size() ? formatter->clone()
                                                         : std::move(formatter));
        }
    }
    // As spdlog::logger writes to its sinks.
    void Write(const spdlog::details::log_msg& msg) {
        for (auto& sink : inner) {
            if (sink->should_log(msg.log_level)) {
                sink->log(msg);
            }
        }
    }
    void FlushInner() {
        for (auto& sink : inner) {
            sink->flush();
        }
    }
    // This thread's kept lines, in order (the scope ended, or an error comes next).
    static void WriteKept() {
        if (t_kept.empty() && !t_flush_kept) {
            return;
        }
        std::vector<KeptLine> lines = std::exchange(t_kept, {});
        const std::shared_ptr<DeferSink> flush = std::exchange(t_flush_kept, nullptr);
        for (const KeptLine& line : lines) {
            line.sink->Write(line.msg);
        }
        if (flush) {
            flush->FlushInner();
        }
    }

private:
    std::vector<spdlog::sink_ptr> inner;
    // WriteThisThreadInBackground's writer: one for every logger, over the same sinks.
    std::shared_ptr<spdlog::sinks::async_sink> background;
};

// The background writer of the current sinks (UpdateSinks; nullptr with the async logger).
std::shared_ptr<spdlog::sinks::async_sink> g_background_sink;
} // Anonymous namespace

void WriteThisThreadInBackground() {
    t_background = true;
}

void SetSinkTimingHook(SinkTimingHook hook) {
    g_sink_timing_hook.store(hook, std::memory_order_relaxed);
}

DeferScope::DeferScope() {
    ++t_defer_depth;
}

DeferScope::~DeferScope() {
    if (--t_defer_depth == 0) {
        DeferSink::WriteKept();
    }
}

static std::shared_ptr<spdlog_stdout> g_console_sink;
static std::shared_ptr<LogFileSink> g_shad_file_sink;

std::unordered_map<std::string_view, std::shared_ptr<spdlog::logger>> ALL_LOGGERS{
    {Class::Common, nullptr},
    {Class::Common_Filesystem, nullptr},
    {Class::Common_Memory, nullptr},
    {Class::Config, nullptr},
    {Class::Core, nullptr},
    {Class::Core_Devices, nullptr},
    {Class::Core_Linker, nullptr},
    {Class::Debug, nullptr},
    {Class::Frontend, nullptr},
    {Class::IPC, nullptr},
    {Class::ImGui, nullptr},
    {Class::Input, nullptr},
    {Class::Kernel, nullptr},
    {Class::Kernel_Event, nullptr},
    {Class::Kernel_Fs, nullptr},
    {Class::Kernel_Pthread, nullptr},
    {Class::Kernel_Sce, nullptr},
    {Class::Kernel_Vmm, nullptr},
    {Class::KeyManager, nullptr},
    {Class::Lib, nullptr},
    {Class::Lib_Ajm, nullptr},
    {Class::Lib_AppContent, nullptr},
    {Class::Lib_Audio3d, nullptr},
    {Class::Lib_AudioIn, nullptr},
    {Class::Lib_AudioOut, nullptr},
    {Class::Lib_AvPlayer, nullptr},
    {Class::Lib_Camera, nullptr},
    {Class::Lib_CommonDlg, nullptr},
    {Class::Lib_CompanionHttpd, nullptr},
    {Class::Lib_CompanionUtil, nullptr},
    {Class::Lib_ContentExport, nullptr},
    {Class::Lib_DiscMap, nullptr},
    {Class::Lib_ErrorDialog, nullptr},
    {Class::Lib_Fiber, nullptr},
    {Class::Lib_Font, nullptr},
    {Class::Lib_FontFt, nullptr},
    {Class::Lib_GameLiveStreaming, nullptr},
    {Class::Lib_GnmDriver, nullptr},
    {Class::Lib_Hmd, nullptr},
    {Class::Lib_HmdSetupDialog, nullptr},
    {Class::Lib_Http, nullptr},
    {Class::Lib_Http2, nullptr},
    {Class::Lib_Ime, nullptr},
    {Class::Lib_ImeDialog, nullptr},
    {Class::Lib_InvitationDialog, nullptr},
    {Class::Lib_Jpeg, nullptr},
    {Class::Lib_Kernel, nullptr},
    {Class::Lib_LibcInternal, nullptr},
    {Class::Lib_Mouse, nullptr},
    {Class::Lib_Move, nullptr},
    {Class::Lib_MsgDlg, nullptr},
    {Class::Lib_Net, nullptr},
    {Class::Lib_NetCtl, nullptr},
    {Class::Lib_Ngs2, nullptr},
    {Class::Lib_NpAuth, nullptr},
    {Class::Lib_NpCommerce, nullptr},
    {Class::Lib_NpCommon, nullptr},
    {Class::Lib_NpManager, nullptr},
    {Class::Lib_NpMatching2, nullptr},
    {Class::Lib_NpSignaling, nullptr},
    {Class::Lib_NpPartner, nullptr},
    {Class::Lib_NpParty, nullptr},
    {Class::Lib_NpProfileDialog, nullptr},
    {Class::Lib_NpScore, nullptr},
    {Class::Lib_NpSnsFacebookDialog, nullptr},
    {Class::Lib_NpTrophy, nullptr},
    {Class::Lib_NpTus, nullptr},
    {Class::Lib_NpWebApi, nullptr},
    {Class::Lib_NpWebApi2, nullptr},
    {Class::Lib_Pad, nullptr},
    {Class::Lib_PlayGo, nullptr},
    {Class::Lib_PlayGoDialog, nullptr},
    {Class::Lib_Png, nullptr},
    {Class::Lib_Random, nullptr},
    {Class::Lib_RazorCpu, nullptr},
    {Class::Lib_Remoteplay, nullptr},
    {Class::Lib_Rtc, nullptr},
    {Class::Lib_Rudp, nullptr},
    {Class::Lib_SaveData, nullptr},
    {Class::Lib_SaveDataDialog, nullptr},
    {Class::Lib_Screenshot, nullptr},
    {Class::Lib_SharePlay, nullptr},
    {Class::Lib_SigninDialog, nullptr},
    {Class::Lib_Ssl, nullptr},
    {Class::Lib_Ssl2, nullptr},
    {Class::Lib_SysModule, nullptr},
    {Class::Lib_SystemGesture, nullptr},
    {Class::Lib_SystemService, nullptr},
    {Class::Lib_Usbd, nullptr},
    {Class::Lib_UserService, nullptr},
    {Class::Lib_Vdec2, nullptr},
    {Class::Lib_VideoOut, nullptr},
    {Class::Lib_Videodec, nullptr},
    {Class::Lib_VideoRecording, nullptr},
    {Class::Lib_Voice, nullptr},
    {Class::Lib_VrTracker, nullptr},
    {Class::Lib_WebBrowserDialog, nullptr},
    {Class::Lib_Zlib, nullptr},
    {Class::Loader, nullptr},
    {Class::Log, nullptr},
    {Class::NpHandler, nullptr},
    {Class::Render, nullptr},
    {Class::Render_Recompiler, nullptr},
    {Class::Render_Vulkan, nullptr},
    {Class::ShadNet, nullptr},
    {Class::Tty, nullptr},
};

template <typename T>
static auto UpdateColorLevels(T sink) {
#ifdef _WIN32
    using LogColor = std::uint16_t;

    const auto Grey = FOREGROUND_INTENSITY;
    const auto Cyan = FOREGROUND_GREEN | FOREGROUND_BLUE;
    const auto Bright_gray = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    const auto Bright_yellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const auto Bright_red = FOREGROUND_RED | FOREGROUND_INTENSITY;
    const auto Bright_magenta = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
#else
    using LogColor = std::string_view;

#define ESC "\x1b"
    const auto Grey = ESC "[1;30m";
    const auto Cyan = ESC "[0;36m";
    const auto Bright_gray = ESC "[0;37m";
    const auto Bright_yellow = ESC "[1;33m";
    const auto Bright_red = ESC "[1;31m";
    const auto Bright_magenta = ESC "[1;35m";
#undef ESC
#endif

    const std::unordered_map<spdlog::level, LogColor> colors{
        {spdlog::level::trace, Grey},       {spdlog::level::debug, Cyan},
        {spdlog::level::info, Bright_gray}, {spdlog::level::warn, Bright_yellow},
        {spdlog::level::err, Bright_red},   {spdlog::level::critical, Bright_magenta}};

    for (const auto& [level, color] : colors) {
        sink->set_color(level, color);
    }

    return sink;
}

void Setup(std::string_view shadps4_filename) {
    static std::once_flag already_registered;

    std::call_once(already_registered, []() {
        std::atexit(Shutdown);
        std::at_quick_exit(Flush);
        std::set_terminate(Terminate);
    });
    if (!Enabled && !BenchmarkEnabled()) {
        return; // the play build: no loggers, console output or log file (log.h)
    }

    for (auto& [name, logger] : ALL_LOGGERS) {
        logger = std::make_shared<spdlog::logger>(std::string(name));
        logger->set_level(spdlog::level::trace);
    }

    // Setup console

#ifdef _WIN32
    if (EmulatorSettings.GetLogType() == "wincolor") {
        g_console_sink = std::make_shared<spdlog::sinks::wincolor_stdout_sink_mt>();
    } else {
        g_console_sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
    }

#else
    g_console_sink = UpdateColorLevels(std::make_shared<spdlog_stdout>(spdlog::color_mode::always));
#endif

    g_console_sink->set_pattern("%^%v%$");

    // Setup file

    g_shad_file_sink = std::make_shared<LogFileSink>(
        (GetUserPath(Common::FS::PathType::LogDir) / shadps4_filename).string(), false,
        EmulatorSettings.GetLogSizeLimit());
    g_shad_file_sink->set_pattern("%^%v%$");

    UpdateSinks();
}

void Switch(std::string_view game_filename) {
    if (!Enabled && !BenchmarkEnabled()) {
        return;
    }
    UpdateSinks();
    UpdateLogLevels(EmulatorSettings.GetLogFilter());
    UpdateLogFlushLevel(EmulatorSettings.GetLogFlushLevel());

    g_shad_file_sink->_size_limit = EmulatorSettings.GetLogSizeLimit();
    g_shad_file_sink->session_file_helper_.open(
        (GetUserPath(Common::FS::PathType::LogDir) / game_filename).string(), !g_should_append);
}

void Shutdown() {
    for (auto& logger : ALL_LOGGERS | std::views::values) {
        logger.reset();
    }

    g_background_sink.reset(); // the last reference: writes what it holds, then its thread ends
    g_shad_file_sink.reset();
    g_console_sink.reset();
}

void Flush() {
    if (const auto background = g_background_sink) {
        background->flush();
        (void)background->wait_all(std::chrono::milliseconds(500));
    }

    if (g_shad_file_sink != nullptr) {
        g_shad_file_sink->flush();
    }

    if (g_console_sink != nullptr) {
        g_console_sink->flush();
    }
}

void Terminate() {
    try {
        if (std::exception_ptr eptr{std::current_exception()}) {
            std::rethrow_exception(eptr);
        }

        LOG_CRITICAL(Debug, "Exiting without exception");
        LOG_BENCHMARK_WARNING(Debug, "Benchmark fatal: exiting without exception");

        std::quick_exit(std::to_underlying(ShadPs4ReturnCode::TERMINATE_WITHOUT_EXCEPTION));
    } catch (const std::exception& exception) {
        LOG_CRITICAL(Debug, "Exception: {}", exception);
        LOG_BENCHMARK_WARNING(Debug, "Benchmark fatal exception: {}", exception.what());

        std::quick_exit(std::to_underlying(ShadPs4ReturnCode::TERMINATE_WITH_EXCEPTION));
    } catch (...) {
        LOG_CRITICAL(Debug, "Unknown exception caught");
        LOG_BENCHMARK_WARNING(Debug, "Benchmark fatal: unknown exception");

        std::quick_exit(std::to_underlying(ShadPs4ReturnCode::TERMINATE_WITH_UNKNOWN_EXCEPTION));
    }
}

void UpdateSinks() {
    if (!Enabled && !BenchmarkEnabled()) {
        return;
    }
    std::initializer_list<spdlog::sink_ptr> sinks{g_console_sink, g_shad_file_sink};

    std::initializer_list<spdlog::sink_ptr> async_sink{std::make_shared<spdlog::sinks::async_sink>(
        spdlog::sinks::async_sink::config{.sinks = sinks})};

    std::initializer_list<spdlog::sink_ptr> dup_filter{
        std::make_shared<spdlog::sinks::dup_filter_sink_mt>(
            std::chrono::milliseconds(EmulatorSettings.GetLogMaxSkipDuration()),
            EmulatorSettings.IsLogSync() ? sinks : async_sink)};

    // WriteThisThreadInBackground (log.h): with the synchronous logger, a writer thread over the
    // same sinks as every logger's (the duplicate filter, or the console and file).
    std::shared_ptr<spdlog::sinks::async_sink> background;
    if (EmulatorSettings.IsLogSync()) {
        background = std::make_shared<spdlog::sinks::async_sink>(spdlog::sinks::async_sink::config{
            .sinks = EmulatorSettings.IsLogSkipDuplicate() ? dup_filter : sinks,
            .on_thread_start = [] { Common::SetCurrentThreadName("shadPS4:LogWriter"); },
        });
    }

    for (auto& [name, logger] : ALL_LOGGERS) {
        logger->sinks() = EmulatorSettings.IsLogSkipDuplicate()
                              ? dup_filter
                              : (EmulatorSettings.IsLogSync() ? sinks : async_sink);
        if (SinkTimingWanted()) {
            for (auto& sink : logger->sinks()) {
                sink = std::make_shared<TimedSink>(sink);
            }
        }
        if (EmulatorSettings.IsLogSync()) {
            // DeferScope (log.h): in front of the sinks above; the level filtering is unchanged.
            logger->sinks() = {std::make_shared<DeferSink>(logger->sinks(), background)};
        }
    }
    g_background_sink = std::move(background);
}

void UpdateLogLevels(std::string_view log_filter) {
    if (!Enabled && !BenchmarkEnabled()) {
        return;
    }
    spdlog::level default_log_level = spdlog::level::info;
    std::unordered_map<std::string, spdlog::level> log_level_per_class;

    if (EmulatorSettings.IsLogEnable()) {
        for (const auto class_level : std::views::split(log_filter, ' ')) {
            const auto class_level_pair =
                std::views::split(class_level, ':') | std::ranges::to<std::vector<std::string>>();

            if (class_level_pair.size() != 2) {
                LOG_ERROR(Config, "bad log filter provided");
                continue;
            }

            if (class_level_pair.front()[0] == '*') {
                default_log_level = spdlog::level_from_str(class_level_pair.back() |
                                                           std::ranges::to<std::string>());
            } else {
                log_level_per_class[class_level_pair.front() | std::ranges::to<std::string>()] =
                    spdlog::level_from_str(class_level_pair.back() |
                                           std::ranges::to<std::string>());
            }
        }
    }

    for (auto& [name, logger] : ALL_LOGGERS) {
        if (EmulatorSettings.IsLogEnable()) {
            const auto level_it = log_level_per_class.find(std::string(name));

            logger->set_level(level_it != log_level_per_class.end() ? level_it->second
                                                                    : default_log_level);
        } else {
            logger->set_level(spdlog::level::off);
        }
    }
}

void UpdateLogFlushLevel(std::string_view log_flush_level) {
    if (!Enabled && !BenchmarkEnabled()) {
        return;
    }
    if (!log_flush_level.empty()) {
        for (auto& [name, logger] : ALL_LOGGERS) {
            logger->flush_on(spdlog::level_from_str(log_flush_level.data()));
        }
    }
}
} // namespace Common::Log
