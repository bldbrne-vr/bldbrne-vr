// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// vr.ini edits that keep the player's file as it is (header-only, no emulator dependencies; unit
// tested in tests/test_vr_page.cpp):
//  - ApplyEdits: only the changed keys' lines change; comments, blank lines, unknown keys and the
//    order stay; `key = value` lines (spaces around) are matched by their trimmed key; a key the
//    file has twice gets the value on every line (the loader keeps the last); a removed key's
//    lines go (the code default applies again); a new key is appended. Every line keeps its own
//    line ending (CRLF or LF); a new line takes the file's first one.
//  - WriteFile: read, edit, write `<file>.tmp`, then rename over the file, so a crash or a full
//    disk never leaves it empty or half written; under `<file>.lock` (created exclusively; a
//    lock older than LockStaleMs is a crashed writer's and is taken over), so two emulator
//    instances never write at the same time and each one's keys survive the other's write (the
//    file is read again under the lock). A file that exists but cannot be read (another program
//    holds it, its permissions) is never replaced: the write fails instead of losing its lines.
//  - Writer: a worker thread that collects edits and writes them DebounceMs after the last one,
//    or at once on Flush (the VR page closing); never on the caller's (frame) thread. A failed
//    write is tried again RetryMs later (MaxAttempts in all), its edits ahead of newer ones.
namespace VR::Ini {

inline std::string_view Trim(std::string_view s) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// A setting line's trimmed key and value; nullopt for comments, blank lines and lines without
// '=' (the loader skips those too).
struct KeyValue {
    std::string key;
    std::string value;
};
inline std::optional<KeyValue> ParseLine(std::string_view line) {
    const std::string_view t = Trim(line);
    if (t.empty() || t.front() == '#' || t.front() == ';') {
        return std::nullopt;
    }
    const auto eq = t.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view key = Trim(t.substr(0, eq));
    if (key.empty()) {
        return std::nullopt;
    }
    return KeyValue{std::string{key}, std::string{Trim(t.substr(eq + 1))}};
}

// One change: set `key` to `value`, or remove it (nullopt).
struct Edit {
    std::string key;
    std::optional<std::string> value;
};

inline std::string ApplyEdits(std::string_view text, const std::vector<Edit>& edits) {
    // The lines with their own endings ("\r\n", "\n", or none: the file's unterminated last line).
    struct Line {
        std::string text;
        std::string eol;
    };
    std::vector<Line> lines;
    std::string first_eol;
    {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            Line line;
            if (end == std::string_view::npos) {
                end = text.size();
                line.text = std::string{text.substr(at, end - at)};
            } else {
                line.text = std::string{text.substr(at, end - at)};
                line.eol = "\n";
                if (!line.text.empty() && line.text.back() == '\r') {
                    line.text.pop_back();
                    line.eol = "\r\n";
                }
                if (first_eol.empty()) {
                    first_eol = line.eol;
                }
            }
            lines.push_back(std::move(line));
            at = end + 1;
        }
    }
    const std::string eol = first_eol.empty() ? std::string{"\n"} : first_eol; // for new lines
    // The last edit of a key wins.
    std::map<std::string, std::optional<std::string>> wanted;
    std::vector<std::string> order;
    for (const Edit& e : edits) {
        const std::string key{Trim(e.key)};
        if (key.empty()) {
            continue;
        }
        if (!wanted.contains(key)) {
            order.push_back(key);
        }
        wanted[key] = e.value;
    }
    std::map<std::string, bool> seen;
    std::vector<Line> out;
    out.reserve(lines.size() + order.size());
    for (Line& line : lines) {
        const auto kv = ParseLine(line.text);
        const auto it = kv ? wanted.find(kv->key) : wanted.end();
        if (it == wanted.end()) {
            out.push_back(std::move(line));
            continue;
        }
        seen[it->first] = true;
        if (it->second) {
            out.push_back({it->first + "=" + *it->second, line.eol}); // its own line ending
        } // removed: the line goes
    }
    // New keys go before the file's trailing blank lines.
    size_t insert_at = out.size();
    while (insert_at > 0 && Trim(out[insert_at - 1].text).empty()) {
        --insert_at;
    }
    std::vector<Line> added;
    for (const std::string& key : order) {
        const auto& value = wanted[key];
        if (value && !seen[key]) {
            added.push_back({key + "=" + *value, eol});
        }
    }
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), added.begin(), added.end());
    std::string result;
    for (size_t i = 0; i < out.size(); ++i) {
        result += out[i].text;
        // A line that is no longer the last (the file's unterminated last line with new keys
        // after it) needs an ending.
        result += out[i].eol.empty() && i + 1 < out.size() ? eol : out[i].eol;
    }
    return result;
}

// The value of `key` in `text` (the last line wins, as the loader), or nullopt.
inline std::optional<std::string> ValueOf(std::string_view text, std::string_view key) {
    std::optional<std::string> result;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (const auto kv = ParseLine(text.substr(at, end - at)); kv && kv->key == Trim(key)) {
            result = kv->value;
        }
        if (end == text.size()) {
            break;
        }
        at = end + 1;
    }
    return result;
}

inline constexpr int LockStaleMs = 10'000;
inline constexpr int LockWaitMs = 3'000;

// `path` with `suffix` appended to its file name, and a file name for messages (UTF-8), without the
// narrow code-page conversion of path::string() (which throws for characters outside it).
inline std::filesystem::path WithSuffix(const std::filesystem::path& path, const char* suffix) {
    std::filesystem::path out = path;
    out += suffix;
    return out;
}
inline std::string NameOf(const std::filesystem::path& path) {
    const auto u8 = path.filename().u8string();
    return std::string(u8.begin(), u8.end());
}

// Exclusive lock file next to `path` (created with "wx": fails while it exists). Returns false
// when another writer held it for LockWaitMs (and it is not stale).
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) : lock_path{WithSuffix(path, ".lock")} {
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(LockWaitMs);
        for (;;) {
#ifdef _WIN32
            std::FILE* f = _wfopen(lock_path.c_str(), L"wx");
#else
            std::FILE* f = std::fopen(lock_path.c_str(), "wx");
#endif
            if (f != nullptr) {
                std::fprintf(f, "vr.ini writer\n");
                std::fclose(f);
                held = true;
                return;
            }
            std::error_code ec;
            const auto when = std::filesystem::last_write_time(lock_path, ec);
            if (!ec) {
                const auto age = std::filesystem::file_time_type::clock::now() - when;
                if (age > std::chrono::milliseconds(LockStaleMs)) {
                    std::filesystem::remove(lock_path, ec); // a crashed writer's lock
                    stale = true;
                    continue;
                }
            }
            if (std::chrono::steady_clock::now() >= give_up) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ~FileLock() {
        if (held) {
            std::error_code ec;
            std::filesystem::remove(lock_path, ec);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    [[nodiscard]] bool Held() const {
        return held;
    }
    [[nodiscard]] bool TookStale() const {
        return stale;
    }

private:
    std::filesystem::path lock_path;
    bool held{false};
    bool stale{false};
};

inline bool ReadText(const std::filesystem::path& path, std::string& out) {
    out.clear();
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Applies `edits` to the file at `path` (created when missing): under its lock, read, edit, write
// `<path>.tmp`, flush, rename over the file. `why` says what failed. The file is not touched when
// the edits change nothing.
inline bool WriteFile(const std::filesystem::path& path, const std::vector<Edit>& edits,
                      std::string* why = nullptr, bool* changed = nullptr) {
    if (changed != nullptr) {
        *changed = false;
    }
    const auto fail = [why](std::string what) {
        if (why != nullptr) {
            *why = std::move(what);
        }
        return false;
    };
    FileLock lock{path};
    if (!lock.Held()) {
        return fail("another writer holds " + NameOf(path) + ".lock");
    }
    std::string before;
    if (!ReadText(path, before)) {
        // A missing file starts empty; one that exists but cannot be read (another program holds
        // it, its permissions) is left as it is: replacing it would lose every line we could not
        // read.
        std::error_code ec;
        const bool missing = !std::filesystem::exists(path, ec) && !ec;
        if (!missing) {
            return fail("cannot read " + NameOf(path) + "; it is left as it is");
        }
        before.clear();
    }
    const std::string after = ApplyEdits(before, edits);
    if (after == before) {
        return true;
    }
    const std::filesystem::path tmp = WithSuffix(path, ".tmp");
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return fail("cannot create " + NameOf(tmp));
        }
        out.write(after.data(), static_cast<std::streamsize>(after.size()));
        out.flush();
        if (!out) {
            std::error_code ec;
            out.close();
            std::filesystem::remove(tmp, ec);
            return fail("cannot write " + NameOf(tmp));
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec); // replaces the file in one step
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return fail("cannot replace " + NameOf(path) + ": " + ec.message());
    }
    if (changed != nullptr) {
        *changed = true;
    }
    return true;
}

// The background writer. Set / Remove from any thread (cheap: a map insert and a notify); the
// worker writes DebounceMs after the last change, or at once after Flush. `log` (optional) gets
// one line per write. The destructor writes what is pending.
class Writer {
public:
    using PathFn = std::function<std::filesystem::path()>;
    using LogFn = std::function<void(const std::string&)>;

    static constexpr int RetryMs = 1'000;
    static constexpr int MaxAttempts = 5;

    explicit Writer(PathFn path_fn, LogFn log_fn = {}, int debounce_ms = 500,
                    int retry_ms = RetryMs)
        : path_of{std::move(path_fn)}, log{std::move(log_fn)}, debounce{debounce_ms},
          retry{retry_ms} {}
    ~Writer() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
            flush = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void Set(const std::string& key, const std::string& value) {
        Queue(Edit{key, value});
    }
    void Remove(const std::string& key) {
        Queue(Edit{key, std::nullopt});
    }
    // Write now (the page closed): returns at once; the worker does it.
    void Flush() {
        {
            std::scoped_lock lk{mutex};
            flush = true;
        }
        cv.notify_all();
    }
    // Blocks until nothing is pending (tests; shutdown paths). False on timeout.
    bool WaitIdle(int timeout_ms = 5000) {
        std::unique_lock lk{mutex};
        return idle_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [this] { return pending.empty() && !writing; });
    }
    struct Stats {
        unsigned writes{0};     // files written
        unsigned unchanged{0};  // writes that changed nothing
        unsigned failures{0};   // failed attempts (each tried again, MaxAttempts in all)
        unsigned dropped{0};    // batches given up after MaxAttempts
        unsigned edits{0};      // edits queued
    };
    Stats GetStats() {
        std::scoped_lock lk{mutex};
        return stats;
    }

private:
    void Queue(Edit e) {
        {
            std::scoped_lock lk{mutex};
            pending.push_back(std::move(e));
            ++stats.edits;
            last_change = std::chrono::steady_clock::now();
            if (!worker.joinable()) {
                worker = std::thread([this] { Run(); });
            }
        }
        cv.notify_all();
    }

    void Run() {
        std::unique_lock lk{mutex};
        for (;;) {
            cv.wait(lk, [this] { return stop || flush || !pending.empty(); });
            if (pending.empty()) {
                flush = false;
                if (stop) {
                    return;
                }
                continue;
            }
            // Debounce: wait until DebounceMs passed since the last change (or a flush); after a
            // failed attempt, until RetryMs passed (stopping: at once).
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                auto due = flush ? now : last_change + std::chrono::milliseconds(debounce);
                if (attempts > 0 && retry_at > due) {
                    due = retry_at;
                }
                if (stop || now >= due) {
                    break;
                }
                cv.wait_until(lk, due);
            }
            std::vector<Edit> batch;
            batch.swap(pending);
            flush = false;
            writing = true;
            lk.unlock();
            std::string why;
            bool changed = false;
            const std::filesystem::path path = path_of();
            const bool ok = WriteFile(path, batch, &why, &changed);
            std::string keys;
            for (const Edit& e : batch) {
                keys += (keys.empty() ? "" : ", ") + e.key +
                        (e.value ? "=" + *e.value : std::string{" removed"});
            }
            lk.lock();
            writing = false;
            bool give_up = false;
            int attempt = 0;
            if (!ok) {
                ++stats.failures;
                attempt = ++attempts;
                give_up = attempts >= MaxAttempts || stop;
                if (give_up) {
                    attempts = 0;
                    ++stats.dropped;
                } else {
                    // Tried again later, ahead of the edits queued meanwhile (the last one wins).
                    pending.insert(pending.begin(), std::make_move_iterator(batch.begin()),
                                   std::make_move_iterator(batch.end()));
                    retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(retry);
                }
            } else {
                attempts = 0;
                if (changed) {
                    ++stats.writes;
                } else {
                    ++stats.unchanged;
                }
            }
            if (log) {
                std::string line =
                    ok ? fmt_line(changed ? "wrote" : "unchanged", path, keys)
                    : give_up
                        ? fmt_line("NOT written (" + why + ") after " + std::to_string(attempt) +
                                       (attempt == 1 ? " try" : " tries") +
                                       "; the change stays for this session only",
                                   path, keys)
                        : fmt_line("NOT written yet (" + why + "; try " +
                                       std::to_string(attempt) + " of " +
                                       std::to_string(MaxAttempts) + ", again in " +
                                       std::to_string(retry) + " ms)",
                                   path, keys);
                lk.unlock();
                log(line);
                lk.lock();
            }
            if (pending.empty()) {
                idle_cv.notify_all();
            }
            if (stop && pending.empty()) {
                return;
            }
        }
    }

    static std::string fmt_line(const std::string& what, const std::filesystem::path& path,
                                const std::string& keys) {
        return "VR settings: " + what + " " + NameOf(path) + ": " + keys;
    }

    PathFn path_of;
    LogFn log;
    int debounce;
    int retry;
    int attempts{0}; // failed attempts of the edits at the front of `pending`
    std::chrono::steady_clock::time_point retry_at{};
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::vector<Edit> pending;
    std::chrono::steady_clock::time_point last_change{};
    bool flush{false};
    bool stop{false};
    bool writing{false};
    Stats stats{};
    std::thread worker;
};

} // namespace VR::Ini
