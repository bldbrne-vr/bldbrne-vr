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
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/file_override.h"
#include "core/file_sys/file_override_decide.h"

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

// A switched layer (SetFileOverrideLayer) and its build (BuildFileOverrideLayer: at once, on its own
// worker; QueueFileOverrideLayer: queued for the one queue worker, or taken by the open that
// wants it first).
struct LayerBuild {
    Bytes bytes;           // the changed file (null: left as it was, or the build failed)
    bool unchanged{false}; // the build left the file as it was
};
struct Layer {
    FileOverrideLayerOn on{nullptr};
    FileOverrideLayerOpened opened{nullptr};
    std::string tag;
    std::string path;                        // as registered (the logs)
    std::shared_future<LayerBuild> pending;  // BuildFileOverrideLayer's worker
    std::function<std::vector<u8>()> queued; // QueueFileOverrideLayer's build, not started yet
    bool building{false};                    // a queued build running (the queue's worker or an open)
    std::optional<LayerBuild> built;
    u32 opens{0};
    LayerDecisions decisions; // the file each open and stat sees (file_override_decide.h)
    u32 stats{0};
};
std::map<std::string, Layer> g_layers; // key: the lower-case guest path (under g_mutex)
std::atomic<bool> g_any_layer{false};
thread_local u32 t_layer_bypass = 0;
thread_local u32 t_stat_scope = 0;
std::condition_variable g_layer_built; // a queued build done (under g_mutex)
std::deque<std::string> g_layer_queue; // the keys queued, in order (under g_mutex)
bool g_queue_worker_running{false};    // (under g_mutex)
// The queue's worker (destroyed before the state above: at exit it waits for a build still
// running).
std::shared_future<void> g_queue_worker;

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

void SetFileOverrideLayer(const std::string& guest_path, FileOverrideLayerOn on,
                          FileOverrideLayerOpened opened, std::string tag) {
    // (A build still running is let go of outside the lock: the last reference of std::async's
    // state waits for its worker, which may be opening a file. Declared before the lock, so it is
    // destroyed after it is released.)
    std::shared_future<LayerBuild> previous;
    std::scoped_lock lk{g_mutex};
    Layer& layer = g_layers[Common::ToLower(guest_path)];
    previous = std::move(layer.pending);
    layer = Layer{};
    layer.on = on;
    layer.opened = opened;
    layer.tag = std::move(tag);
    layer.path = guest_path;
    g_any_layer.store(true, std::memory_order_release);
}

namespace {
// A layer's build, run here; never throws (a failure: not built, logged).
LayerBuild RunLayerBuild(const std::function<std::vector<u8>()>& build, const std::string& path,
                         const std::string& tag, std::chrono::steady_clock::time_point started) {
    LayerBuild result;
    try {
        std::vector<u8> bytes = build();
        result.unchanged = bytes.empty();
        if (!bytes.empty()) {
            result.bytes = std::make_shared<const std::vector<u8>>(std::move(bytes));
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();
        LOG_INFO(Core, "File override: the layer of {} {} ({}; built in {:.0f} ms)", path,
                 result.bytes != nullptr
                     ? fmt::format("is served while on ({} bytes)", result.bytes->size())
                     : std::string{"leaves the file as it was"},
                 tag, ms);
    } catch (const std::exception& e) {
        result = LayerBuild{};
        try {
            LOG_WARNING(Core,
                        "File override: the layer of {} not built ({}; {}): the file under it is "
                        "served",
                        path, e.what(), tag);
        } catch (...) {
        }
    } catch (...) {
        result = LayerBuild{};
    }
    return result;
}

// The queue's worker: one queued layer after another until none is left (QueueFileOverrideLayer).
// At the thread's own priority: an open may be waiting for the layer it builds.
void RunLayerQueue() {
    for (;;) {
        std::function<std::vector<u8>()> build;
        std::string key, path, tag;
        bool taken = false;
        try {
            std::scoped_lock lk{g_mutex};
            while (!g_layer_queue.empty()) {
                key = std::move(g_layer_queue.front());
                g_layer_queue.pop_front();
                const auto it = g_layers.find(key);
                if (it != g_layers.end() && it->second.queued) {
                    // (Its names copied first: a copy that throws leaves the layer queued.)
                    path = it->second.path;
                    tag = it->second.tag;
                    build = std::move(it->second.queued);
                    it->second.queued = nullptr;
                    it->second.building = true;
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                g_queue_worker_running = false;
                return;
            }
        } catch (...) {
            // (Out of memory: the worker stops; each layer left queued is built by its open.)
            std::scoped_lock lk{g_mutex};
            g_queue_worker_running = false;
            return;
        }
        LayerBuild result = RunLayerBuild(build, path, tag, std::chrono::steady_clock::now());
        {
            std::scoped_lock lk{g_mutex};
            if (const auto it = g_layers.find(key); it != g_layers.end()) {
                it->second.built = std::move(result);
                it->second.building = false;
            }
        }
        g_layer_built.notify_all();
    }
}
} // Anonymous namespace

void BuildFileOverrideLayer(const std::string& guest_path, std::function<std::vector<u8>()> build) {
    std::scoped_lock lk{g_mutex};
    const auto it = g_layers.find(Common::ToLower(guest_path));
    if (it == g_layers.end() || it->second.built || it->second.pending.valid() ||
        it->second.queued || it->second.building) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    try {
        it->second.pending = std::async(std::launch::async,
                                        [build = std::move(build), path = it->second.path,
                                         tag = it->second.tag, started]() -> LayerBuild {
                                            return RunLayerBuild(build, path, tag, started);
                                        })
                                 .share();
    } catch (...) {
        // (No worker: not built; the file under it is served.)
        it->second.built = LayerBuild{};
    }
}

void QueueFileOverrideLayer(const std::string& guest_path, std::function<std::vector<u8>()> build) {
    try {
        const std::string key = Common::ToLower(guest_path);
        std::scoped_lock lk{g_mutex};
        const auto it = g_layers.find(key);
        if (it == g_layers.end() || it->second.built || it->second.pending.valid() ||
            it->second.queued || it->second.building) {
            return;
        }
        g_layer_queue.push_back(key); // (first: if it throws, nothing changed)
        it->second.queued = std::move(build);
        if (!g_queue_worker_running) {
            // The worker made under the lock (the one before it has let go of the lock for good
            // once it said it is not running: its future's last reference waits for it here
            // without needing the lock; the new one waits for the lock to start).
            try {
                g_queue_worker = std::async(std::launch::async, RunLayerQueue).share();
                g_queue_worker_running = true;
            } catch (...) {
                // (No worker: each queued layer is built by the open that wants it.)
            }
        }
    } catch (...) {
        // (Out of memory: not queued; its open finds it neither built nor queued and serves the
        // file under it, telling the layer it did not apply.)
    }
}

FileOverrideLayerBypass::FileOverrideLayerBypass() {
    ++t_layer_bypass;
}

FileOverrideLayerBypass::~FileOverrideLayerBypass() {
    --t_layer_bypass;
}

FileOverrideStatScope::FileOverrideStatScope() {
    ++t_stat_scope;
}

FileOverrideStatScope::~FileOverrideStatScope() {
    --t_stat_scope;
}

namespace {
// The layer's part of an open: its file when it applies with a changed one; null otherwise (the
// path served as if it had no layer, or as it was). Tells the layer's `opened`, unless a stat.
std::unique_ptr<IFile> OpenLayer(const std::string& key, std::string_view guest_path) {
    const bool stat = t_stat_scope != 0;
    std::chrono::steady_clock::time_point now{};
    FileOverrideLayerOpened opened = nullptr;
    FileOverrideLayerOn is_on = nullptr;
    std::optional<bool> decided;
    {
        std::scoped_lock lk{g_mutex};
        const auto it = g_layers.find(key);
        if (it == g_layers.end()) {
            return nullptr;
        }
        now = std::chrono::steady_clock::now(); // (a layered path's open only)
        Layer& layer = it->second;
        opened = layer.opened;
        is_on = layer.on;
        decided = layer.decisions.Decide(now, stat, std::nullopt);
    }
    // Else the switch, asked outside the lock (it may queue the builds), then decided under it
    // (a decision made meanwhile on another thread wins: the same burst's).
    if (!decided) {
        bool fresh = false;
        try {
            fresh = is_on != nullptr && is_on();
        } catch (...) {
            fresh = false;
        }
        std::scoped_lock lk{g_mutex};
        const auto it = g_layers.find(key);
        if (it == g_layers.end()) {
            return nullptr;
        }
        decided = it->second.decisions.Decide(std::chrono::steady_clock::now(), stat, fresh);
    }
    const bool on = decided.value_or(false);
    std::function<std::vector<u8>()> mine; // a queued build this open takes
    std::string path, tag;
    std::shared_future<LayerBuild> pending;
    {
        std::unique_lock lk{g_mutex};
        const auto it = g_layers.find(key);
        if (it == g_layers.end()) {
            return nullptr;
        }
        Layer& layer = it->second;
        if (stat && layer.stats++ < 8) {
            LOG_INFO(Core, "File override: stat {} of {}: its layer {}{}", layer.stats, guest_path,
                     on ? "on" : "off", layer.decisions.stat_answer ? " (the next open's)" : "");
        }
        if (on && !layer.built) {
            if (layer.pending.valid()) {
                pending = layer.pending;
            } else if (layer.queued) {
                // Not started: built here and now, at this thread's own priority (it waits
                // behind no other layer's build). Its names copied first: a copy that throws
                // leaves it queued.
                path = layer.path;
                tag = layer.tag;
                mine = std::move(layer.queued);
                layer.queued = nullptr;
                layer.building = true;
            } else if (layer.building) {
                // Being built (the queue's worker or another open): that one build.
                g_layer_built.wait(lk, [&layer] { return !layer.building; });
            }
        }
    }
    double waited_ms = 0.0;
    if (mine) {
        const auto t0 = std::chrono::steady_clock::now();
        LayerBuild result = RunLayerBuild(mine, path, tag, t0);
        {
            std::scoped_lock lk{g_mutex};
            if (const auto it = g_layers.find(key); it != g_layers.end()) {
                it->second.built = std::move(result);
                it->second.building = false;
            }
        }
        g_layer_built.notify_all();
        waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
    } else if (pending.valid() &&
               pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        // Its own build still running: waited for without the lock (other opens go on).
        const auto t0 = std::chrono::steady_clock::now();
        pending.wait();
        waited_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
    }
    std::unique_ptr<IFile> file;
    bool applied = false;
    {
        std::scoped_lock lk{g_mutex};
        const auto it = g_layers.find(key);
        if (on && it != g_layers.end()) {
            Layer& layer = it->second;
            if (!layer.built && layer.pending.valid() &&
                layer.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try {
                    layer.built = layer.pending.get();
                } catch (...) {
                    layer.built = LayerBuild{};
                }
                layer.pending = {};
            }
            if (layer.built) {
                applied = layer.built->bytes != nullptr || layer.built->unchanged;
                if (layer.built->bytes != nullptr) {
                    file = std::make_unique<MemoryFile>(layer.built->bytes);
                }
                if (applied && !stat && layer.opens < 8) {
                    LOG_INFO(Core, "File override: open {} of {} with its layer ({}{}{})",
                             layer.opens + 1, guest_path, layer.tag,
                             file ? "" : "; the file as it was",
                             waited_ms > 0.0 ? fmt::format("; waited {:.0f} ms for it", waited_ms)
                                             : std::string{});
                }
                layer.opens += applied && !stat ? 1 : 0;
            }
        }
    }
    if (opened != nullptr && !stat) {
        opened(guest_path, applied);
    }
    return file;
}
} // Anonymous namespace

namespace {
std::atomic<s64> g_cutscene_open_ns{0};
std::atomic<CutsceneOpenObserver> g_cutscene_observer{nullptr};
} // namespace

void SetCutsceneOpenObserver(CutsceneOpenObserver observer) {
    g_cutscene_observer.store(observer, std::memory_order_release);
}

void NoteGuestOpen(std::string_view guest_path) {
    // "/app0/dvdroot_ps4/remo/s24_01_0010.remobnd.dcx": a cutscene's data, opened as it starts.
    if (guest_path.find("/remo/") == std::string_view::npos) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    g_cutscene_open_ns.store(now, std::memory_order_release);
    if (const auto observer = g_cutscene_observer.load(std::memory_order_acquire)) {
        observer(guest_path);
    }
}

s64 LastCutsceneOpenNs() {
    return g_cutscene_open_ns.load(std::memory_order_acquire);
}

std::unique_ptr<IFile> OpenFileOverride(std::string_view guest_path) {
    const bool layers = g_any_layer.load(std::memory_order_acquire) && t_layer_bypass == 0;
    if (!g_any.load(std::memory_order_acquire) && !layers) {
        return nullptr;
    }
    const std::string key = Common::ToLower(guest_path);
    // A switched layer first (SetFileOverrideLayer): its file while it is on; else the path's
    // plain override or the game's own, as without it.
    if (layers) {
        if (auto file = OpenLayer(key, guest_path)) {
            return file;
        }
        if (!g_any.load(std::memory_order_acquire)) {
            return nullptr;
        }
    }
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
