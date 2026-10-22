// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <map>
#include <mutex>

#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/file_override.h"

namespace Core::FileSys {

namespace {

// A read-only file in memory (shared by every open of it).
class MemoryFile final : public IFile {
public:
    explicit MemoryFile(std::shared_ptr<const std::vector<u8>> bytes) : m_bytes{std::move(bytes)} {}

    s64 Read(void* dst, u64 size) override {
        std::scoped_lock lk{m_mutex};
        const u64 total = m_bytes->size();
        if (size == 0 || m_position >= total) {
            return 0;
        }
        const u64 count = std::min(size, total - m_position);
        std::memcpy(dst, m_bytes->data() + m_position, count);
        m_position += count;
        return static_cast<s64>(count);
    }

    s64 Write(const void*, u64) override {
        return -1;
    }

    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override {
        std::scoped_lock lk{m_mutex};
        s64 base = 0;
        switch (origin) {
        case Common::FS::SeekOrigin::SetOrigin:
            base = 0;
            break;
        case Common::FS::SeekOrigin::CurrentPosition:
            base = static_cast<s64>(m_position);
            break;
        case Common::FS::SeekOrigin::End:
            base = static_cast<s64>(m_bytes->size());
            break;
        }
        const s64 target = base + offset;
        if (target < 0) {
            return false;
        }
        m_position = static_cast<u64>(target);
        return true;
    }

    u64 Tell() const override {
        std::scoped_lock lk{m_mutex};
        return m_position;
    }

    u64 Size() const override {
        return m_bytes->size();
    }

    bool Flush() override {
        return true;
    }

    bool IsOpen() const override {
        return true;
    }

    MmapPolicy GetMmapPolicy() const override {
        return MmapPolicy::Copy;
    }

    bool Map(u8* addr, u64 size, u64 offset, u32 raw_prot, const FileMapContext& ctx) override {
        ctx.map_anonymous(addr, size);
        const u64 total = m_bytes->size();
        const u64 count = offset < total ? std::min(size, total - offset) : 0;
        if (count != 0) {
            std::memcpy(addr, m_bytes->data() + offset, count);
        }
        if (count < size) {
            std::memset(addr + count, 0, size - count);
        }
        ctx.protect(addr, size, raw_prot);
        return true;
    }

    bool IsReadOnly() const override {
        return true;
    }

private:
    std::shared_ptr<const std::vector<u8>> m_bytes;
    mutable std::mutex m_mutex;
    u64 m_position{0};
};

using Bytes = std::shared_ptr<const std::vector<u8>>;

struct Entry {
    Bytes bytes;                       // null while `pending` is being built
    std::shared_future<Bytes> pending; // SetFileOverrideDeferred's worker
    std::string tag;
    u32 opens{0};
};

std::mutex g_mutex;
std::map<std::string, Entry> g_files; // key: the lower-case guest path
std::atomic<bool> g_any{false};

} // Anonymous namespace

void SetFileOverride(const std::string& guest_path, std::vector<u8> bytes, std::string tag) {
    std::scoped_lock lk{g_mutex};
    LOG_INFO(Core, "File override: {} is served from memory ({} bytes, {})", guest_path,
             bytes.size(), tag);
    g_files[Common::ToLower(guest_path)] =
        Entry{std::make_shared<const std::vector<u8>>(std::move(bytes)), {}, std::move(tag), 0};
    g_any.store(true, std::memory_order_release);
}

void SetFileOverrideDeferred(const std::string& guest_path,
                             std::function<std::vector<u8>()> build, std::string tag) {
    const auto started = std::chrono::steady_clock::now();
    std::shared_future<Bytes> pending =
        std::async(std::launch::async,
                   [build = std::move(build), path = guest_path, tag, started]() -> Bytes {
                       std::vector<u8> bytes;
                       try {
                           bytes = build();
                       } catch (const std::exception& e) {
                           LOG_WARNING(Core, "File override: building {} failed ({})", path,
                                       e.what());
                           bytes.clear();
                       }
                       const double ms = std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - started)
                                             .count();
                       if (bytes.empty()) {
                           LOG_WARNING(Core,
                                       "File override: {} not built ({}); the game's own file "
                                       "is served",
                                       path, tag);
                           return nullptr;
                       }
                       LOG_INFO(Core, "File override: {} is served from memory ({} bytes, {}; "
                                      "built in {:.0f} ms)",
                                path, bytes.size(), tag, ms);
                       return std::make_shared<const std::vector<u8>>(std::move(bytes));
                   })
            .share();
    std::scoped_lock lk{g_mutex};
    LOG_INFO(Core, "File override: {} will be served from memory ({}; being built)", guest_path,
             tag);
    g_files[Common::ToLower(guest_path)] = Entry{nullptr, std::move(pending), std::move(tag), 0};
    g_any.store(true, std::memory_order_release);
}

void ClearFileOverride(const std::string& guest_path) {
    std::scoped_lock lk{g_mutex};
    g_files.erase(Common::ToLower(guest_path));
    g_any.store(!g_files.empty(), std::memory_order_release);
}

namespace {
std::atomic<s64> g_cutscene_open_ns{0};
} // namespace

void NoteGuestOpen(std::string_view guest_path) {
    // "/app0/dvdroot_ps4/remo/s24_01_0010.remobnd.dcx": a cutscene's data, opened as it starts.
    if (guest_path.find("/remo/") == std::string_view::npos) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    g_cutscene_open_ns.store(now, std::memory_order_release);
}

s64 LastCutsceneOpenNs() {
    return g_cutscene_open_ns.load(std::memory_order_acquire);
}

std::unique_ptr<IFile> OpenFileOverride(std::string_view guest_path) {
    if (!g_any.load(std::memory_order_acquire)) {
        return nullptr;
    }
    const std::string key = Common::ToLower(guest_path);
    std::shared_future<Bytes> pending;
    {
        std::scoped_lock lk{g_mutex};
        const auto it = g_files.find(key);
        if (it == g_files.end()) {
            return nullptr;
        }
        pending = it->second.pending;
    }
    // A deferred file still being built: wait for it without the lock (other opens go on).
    double waited_ms = 0.0;
    if (pending.valid() &&
        pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        const auto t0 = std::chrono::steady_clock::now();
        pending.wait();
        waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
    }
    std::scoped_lock lk{g_mutex};
    const auto it = g_files.find(key);
    if (it == g_files.end()) {
        return nullptr;
    }
    Entry& entry = it->second;
    if (entry.bytes == nullptr && entry.pending.valid()) {
        entry.bytes = entry.pending.get();
        entry.pending = {};
        if (entry.bytes == nullptr) {
            g_files.erase(it);
            g_any.store(!g_files.empty(), std::memory_order_release);
            return nullptr;
        }
    }
    if (entry.bytes == nullptr) {
        return nullptr;
    }
    if (entry.opens < 8) {
        LOG_INFO(Core, "File override: open {} of {} served from memory ({} bytes, {}{})",
                 entry.opens + 1, guest_path, entry.bytes->size(), entry.tag,
                 waited_ms > 0.0 ? fmt::format("; waited {:.0f} ms for it", waited_ms)
                                 : std::string{});
    }
    ++entry.opens;
    return std::make_unique<MemoryFile>(entry.bytes);
}

} // namespace Core::FileSys
