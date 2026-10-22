// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "benchmark.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace Launcher::Benchmark {
namespace {
struct Handle {
    HANDLE h{};
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle &) = delete;
};
struct Child {
    Handle process, job;
    explicit Child(const LaunchPlan &p, bool isolate_tree = true) {
        if (isolate_tree) {
            job.h = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job.h || !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits,
                                                   sizeof(limits)))
                throw std::runtime_error("Cannot create isolated benchmark process group.");
        }
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        auto command = p.command;
        if (!CreateProcessW(p.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                            p.environment.empty() ? nullptr : (void *)p.environment.data(),
                            p.directory.c_str(), &startup, &info))
            throw std::runtime_error("Cannot launch benchmark runtime.");
        process.h = info.hProcess;
        Handle thread(info.hThread);
        if (job.h && !AssignProcessToJobObject(job.h, process.h)) {
            TerminateProcess(process.h, 1);
            throw std::runtime_error("Cannot isolate benchmark process; it was stopped.");
        }
        if (ResumeThread(thread.h) == DWORD(-1)) {
            TerminateProcess(process.h, 1);
            throw std::runtime_error("Cannot resume benchmark runtime; it was stopped.");
        }
    }
    ~Child() {
        // Real OpenXR can start SteamVR services; own only the probe itself.
        if (!job.h && process.h && !Finished()) {
            TerminateProcess(process.h, 1);
            WaitForSingleObject(process.h, 1000);
        }
    }
    bool Finished() const { return WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0; }
    void Stop() {
        if (Finished())
            return;
        const DWORD pid = GetProcessId(process.h);
        EnumWindows(
            [](HWND w, LPARAM p) -> BOOL {
                DWORD owner{};
                GetWindowThreadProcessId(w, &owner);
                if (owner == DWORD(p))
                    PostMessageW(w, WM_CLOSE, 0, 0);
                return TRUE;
            },
            LPARAM(pid));
        if (WaitForSingleObject(process.h, 5000) != WAIT_OBJECT_0) {
            if (job.h) TerminateJobObject(job.h, 1);
            else TerminateProcess(process.h, 1);
            WaitForSingleObject(process.h, 5000);
        }
    }
};
void Environment(LaunchPlan &p, const std::map<std::wstring, std::wstring> &extra) {
    std::map<std::wstring, std::wstring> entries;
    for (const wchar_t *at = p.environment.data(); at && *at; at += wcslen(at) + 1) {
        std::wstring e(at);
        auto n = e.find(L'=', e.starts_with(L"=") ? 1 : 0);
        if (n != std::wstring::npos)
            entries[e.substr(0, n)] = e.substr(n + 1);
    }
    for (auto &[k, v] : extra) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (_wcsicmp(it->first.c_str(), k.c_str()) == 0)
                it = entries.erase(it);
            else
                ++it;
        }
        entries[k] = v;
    }
    p.environment.clear();
    for (auto &[k, v] : entries) {
        auto e = k + L"=" + v;
        p.environment.insert(p.environment.end(), e.begin(), e.end());
        p.environment.push_back(0);
    }
    p.environment.push_back(0);
}
fs::path SafeRelative(const std::string &text) {
    auto p = fs::path(Wide(text));
    if (p.empty() || p.is_absolute() || p.has_root_name() || text.find(':') != std::string::npos)
        throw std::runtime_error("Invalid benchmark package path.");
    for (auto &part : p)
        if (part == L".." || part == L".")
            throw std::runtime_error("Invalid benchmark package path.");
    return p;
}
void NoLinks(const fs::path &root, const fs::path &relative) {
    auto current = root;
    const auto root_attributes = GetFileAttributesW(root.c_str());
    if (root_attributes != INVALID_FILE_ATTRIBUTES &&
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Benchmark package root is redirected.");
    for (auto &part : relative) {
        current /= part;
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error(
                "Benchmark package or destination contains a redirected path.");
    }
}
double Seconds(FILETIME f) {
    ULARGE_INTEGER v{};
    v.LowPart = f.dwLowDateTime;
    v.HighPart = f.dwHighDateTime;
    return v.QuadPart / 1e7;
}
struct Cpu {
    double process{}, system{};
    size_t log_bytes{};
    std::chrono::steady_clock::time_point time;
    bool valid{};
};
Cpu ReadCpu(HANDLE process, size_t bytes) {
    FILETIME create{}, exit{}, kernel{}, user{}, idle{}, syskernel{}, sysuser{};
    Cpu s;
    s.time = std::chrono::steady_clock::now();
    s.log_bytes = bytes;
    s.valid = GetProcessTimes(process, &create, &exit, &kernel, &user) &&
              GetSystemTimes(&idle, &syskernel, &sysuser);
    s.process = Seconds(kernel) + Seconds(user);
    s.system = Seconds(syskernel) + Seconds(sysuser) - Seconds(idle);
    return s;
}
void CopyCache(const fs::path &from, const fs::path &to) {
    if (!fs::is_directory(from))
        return;
    for (auto &e : fs::recursive_directory_iterator(from)) {
        auto rel = e.path().lexically_relative(from);
        NoLinks(from, rel);
        if (e.is_regular_file()) {
            fs::create_directories((to / rel).parent_path());
            fs::copy_file(e.path(), to / rel, fs::copy_options::overwrite_existing);
        }
    }
}
} // namespace
std::string FileHash(const fs::path &p) { return FileSha256(p); }
bool UsePresentationStartup(const Json &manifest, const fs::path &runtime) {
    const auto startup=manifest.value("startup_navigation",std::string("timed"));
        if(startup!="timed" && startup!="presentation-v1")
            throw std::runtime_error("Unsupported benchmark startup protocol. Update the benchmark package.");
        const bool presentation_startup=startup=="presentation-v1";
        if(presentation_startup) {
            // Inspect exports only: do not run DllMain or load dependencies into the launcher.
            const auto module=LoadLibraryExW(runtime.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
            if(!module) throw std::runtime_error("Cannot inspect benchmark startup capability.");
            const bool supported=GetProcAddress(module,"BbvrBenchmarkStartupV1")!=nullptr;
            FreeLibrary(module);
            if(!supported) throw std::runtime_error("Benchmark package requests startup recovery but its runtime does not support it.");
        }
    return presentation_startup;
}
Headset ScanHeadset(const fs::path &runtime, const std::atomic_bool* cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Headset detection cancelled.");
    const auto folder =
        fs::temp_directory_path() / (L"bbvr-headset-" + std::to_wstring(GetCurrentProcessId()) +
                                     L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(folder);
    const auto output = folder / L"headset.json";
    struct ScanFiles {
        fs::path output, folder;
        ~ScanFiles() {
            std::error_code ec;
            fs::remove(output, ec);
            fs::remove(folder, ec);
        }
    } cleanup{output, folder};
    LaunchPlan plan{runtime, folder};
    plan.command =
        Quote(ArgumentPath(runtime)) + L" --benchmark-headset headset.json";
    Child child(plan, false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        // Unwinding stops only the probe root, never services started by OpenXR.
        if (cancel && cancel->load()) throw std::runtime_error("Headset detection cancelled.");
        const auto wait = WaitForSingleObject(child.process.h, 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) throw std::runtime_error("Cannot wait for headset detection.");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Headset scan timed out. Check the active OpenXR runtime.");
    }
    DWORD exit{};
    GetExitCodeProcess(child.process.h, &exit);
    if (exit || !fs::exists(output))
        throw std::runtime_error("Connect the headset and use a runtime build with "
                                 "benchmark scanning support.");
    auto j = Json::parse(Read(output));
    if (j.value("benchmark_telemetry", 0) != 1)
        throw std::runtime_error("This runtime lacks release benchmark telemetry. "
                                 "Update the bundled runtime first.");
    Headset h{j.at("name"), j.at("width"), j.at("height"), j.at("max_width"), j.at("max_height")};
    for (unsigned i = 0; i < 2; ++i)
        if (h.width[i] < 64 || h.height[i] < 64 || h.width[i] > 8192 || h.height[i] > 8192 ||
            h.width[i] > h.max_width || h.height[i] > h.max_height)
            throw std::runtime_error("Headset reported unsupported dimensions.");
    h.refresh_hz = j.value("refresh_hz", 0.0f);
    return h;
}
void CopyFixture(const fs::path &package, const fs::path &destination) {
    const auto manifest = Json::parse(Read(package / "manifest.json"));
    if (manifest.at("schema") != 1 || !manifest.at("files").is_array() ||
        manifest.at("files").empty())
        throw std::runtime_error("Unsupported or empty benchmark fixture.");
    if (fs::exists(destination))
        throw std::runtime_error("Benchmark save destination must be new.");
    for (auto &f : manifest.at("files")) {
        const auto rel = SafeRelative(f.at("path").get<std::string>());
        const auto generic = rel.generic_string();
        if (!generic.starts_with("1000/savedata/") ||
            generic.find("/SPRJ0005/") == std::string::npos)
            throw std::runtime_error("Fixture manifest contains files outside the benchmark save.");
        NoLinks(package / "home", rel);
        if (FileHash(package / "home" / rel) != f.at("sha256").get<std::string>())
            throw std::runtime_error("Benchmark save failed integrity verification.");
    }
    fs::create_directories(destination);
    for (auto &f : manifest.at("files")) {
        const auto rel = SafeRelative(f.at("path").get<std::string>());
        NoLinks(destination, rel);
        fs::create_directories((destination / rel).parent_path());
        fs::copy_file(package / "home" / rel, destination / rel);
    }
}
// Remove only owned paths beneath the scan root, never junction targets or player data.
static void RemoveScanTree(const fs::path& root, const fs::path& relative) {
    const auto base = fs::absolute(root).lexically_normal();
    const auto rel = SafeRelative(PathText(relative));
    const auto target = (base / rel).lexically_normal();
    if (target.lexically_relative(base) != rel) throw std::runtime_error("Invalid scan cleanup path.");
    NoLinks(base, rel);
    if (!fs::exists(target)) return;
    for (const auto& entry : fs::recursive_directory_iterator(target))
        NoLinks(base, entry.path().lexically_relative(base));
    fs::remove_all(target);
}
void PruneScanHistory(const fs::path& root, unsigned keep) {
    if (!fs::is_directory(root)) return;
    NoLinks(root, {});
    std::vector<fs::directory_entry> completed;
    for (const auto& entry : fs::directory_iterator(root)) {
        try {
            NoLinks(root, entry.path().filename());
            if (!entry.is_directory()) continue;
            NoLinks(root, entry.path().filename() / "scan-owner.json");
            const auto text = Read(entry.path() / "scan-owner.json");
            if (text.empty()) continue;
            const auto owner = Json::parse(text);
            if (owner.value("kind", "") == "bbvr-perf-scan" && owner.value("complete", false))
                completed.push_back(entry);
        } catch (...) { /* Unknown/redirected directories are never ours to delete. */ }
    }
    std::sort(completed.begin(), completed.end(), [](const auto& a, const auto& b) {
        return fs::last_write_time(a.path() / "scan-owner.json") > fs::last_write_time(b.path() / "scan-owner.json");
    });
    for (size_t i = keep; i < completed.size(); ++i)
        RemoveScanTree(root, completed[i].path().filename());
}
Report Run(const Request &request, std::atomic_bool &cancel, const Progress &progress) {
    const auto scan_start = std::chrono::steady_clock::now();
    Request q = request;
    std::vector<fs::path> run_folders;
    const bool adaptive = q.adaptive && q.candidate_ids.empty();
    const bool quick = q.quick && adaptive;
    // Both launcher scans collect the same evidence per run. Thorough spends
    // its larger budget on more configurations, not longer samples.
    if (adaptive)
        q.seconds = 65;
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - scan_start).count();
    };
    q.target_hz = TargetRate(q.target_hz, q.headset.refresh_hz);
    if (!q.display_hz)
        q.display_hz = q.target_hz;
    const double work_limit=quick ? QuickScanWorkSeconds : LongScanWorkSeconds;
    Report report;
    report.target_hz = q.target_hz;
    try {
        Handle mutex(CreateMutexW(nullptr, TRUE, L"Local\\BldbrneVRBenchmark"));
        if (!mutex.h || GetLastError() == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Another benchmark is already running.");
        if (RuntimeRunning())
            throw std::runtime_error("Close the game before starting the benchmark.");
        if (!ValidGameRefreshHz(q.target_hz) || q.seconds < (adaptive ? 30u : 90u) || q.seconds > 300 ||
            q.display_hz < 30 || q.display_hz > 360)
            throw std::runtime_error("Invalid benchmark duration or target refresh rate.");
        progress("Target: " + std::to_string(q.target_hz) + " FPS (8% tolerance). Checking benchmark save and runtime.");
        const auto manifest = Json::parse(Read(q.package / "manifest.json"));
        const auto runtime_json = q.package / "runtime.json";
        const auto mock = q.package / "benchmark_xr.dll";
        if (FileHash(mock) != manifest.at("runtime_sha256").get<std::string>())
            throw std::runtime_error("Benchmark test runtime failed integrity verification.");
        const auto startup=manifest.value("startup_navigation",std::string("timed"));
        const bool presentation_startup=UsePresentationStartup(manifest,mock);
        const auto runtime_manifest = Json::parse(Read(runtime_json));
        if (runtime_manifest.at("runtime").at("library_path") != "./benchmark_xr.dll")
            throw std::runtime_error("Benchmark runtime manifest contains an unexpected library.");
        const auto game = GameExecutable(Path(q.profile.game));
        const auto support = GameSupportInfo(game);
        const auto version = CheckGameBuild(game);
        if (std::find(manifest.at("game_versions").begin(), manifest.at("game_versions").end(),
                      Json(version)) == manifest.at("game_versions").end())
            throw std::runtime_error("Benchmark save is not validated for this game version.");
        report.folder =
            q.paths.root / "benchmarks" /
            (std::to_string(GetTickCount64()) + "-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(report.folder);
        Write(report.folder / "scan-owner.json", Json{{"kind", "bbvr-perf-scan"}, {"complete", false}}.dump());
        auto candidates = Candidates(q.system, DlssIncluded(Path(q.profile.runtime)),
                                     Fsr4Included(Path(q.profile.runtime)), Fsr31Included(Path(q.profile.runtime)));
        if (!q.candidate_ids.empty()) {
            std::erase_if(candidates, [&](auto &c) {
                return std::find(q.candidate_ids.begin(), q.candidate_ids.end(), c.id) ==
                       q.candidate_ids.end();
            });
            if (candidates.size() != q.candidate_ids.size())
                throw std::runtime_error("Unsupported benchmark candidate requested.");
        }
        if (adaptive)
            candidates.clear();
        Json metadata{{"schema", 1},
                      {"gpu", q.system.gpu},
                      {"driver", q.system.driver},
                      {"dedicated_vram_bytes", q.system.vram},
                      {"system_ram_bytes", q.system.ram},
                      {"logical_processors", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)},
                      {"headset", q.headset.name},
                      {"eye_width", q.headset.width},
                      {"eye_height", q.headset.height},
                      {"max_eye_width", q.headset.max_width},
                      {"max_eye_height", q.headset.max_height},
                      {"target_hz", q.target_hz},
                      {"controlled_display_hz", q.display_hz},
                      {"headset_refresh_hz", q.headset.refresh_hz},
                      {"target_source", request.target_hz          ? "manual"
                                        : q.headset.refresh_hz > 0 ? "headset"
                                                                   : "90-fps-fallback"},
                      {"search", quick ? "quick-temporal-first" : adaptive ? "temporal-first-adaptive" : "fixed-matrix"},
                      {"scan_budget_seconds", quick ? QuickScanBudgetSeconds : LongScanBudgetSeconds},
                      {"runtime_sha256", FileHash(Path(q.profile.runtime))},
                      {"fixture_manifest", manifest},
                      {"startup_navigation",startup},
                      {"game", support},
                      {"scope", "Controlled static-scene rendering estimate; fixed test FOV, "
                                "not headset/compositor or motion-quality certification"}};
        metadata["upscaler_files"] = Json::object();
        for (const auto *file : {"dlss/nvngx_dlss.dll", "dlss/vr_dlss_stereo.dll",
                                 "fsr4/vr_fsr4_stereo.dll", "fsr4/package.json",
                                 "fsr31/vr_fsr31_stereo.dll", "fsr31/package.json"}) {
            const auto path = Path(q.profile.runtime).parent_path() / file;
            if (fs::is_regular_file(path))
                metadata["upscaler_files"][file] = FileHash(path);
        }
        const auto fsr_assets = Path(q.profile.runtime).parent_path() / "fsr4/fsr4_shaders";
        if (fs::is_directory(fsr_assets)) {
            for (const auto &entry : fs::recursive_directory_iterator(fsr_assets)) {
                const auto rel = entry.path().lexically_relative(fsr_assets);
                NoLinks(fsr_assets, rel);
                if (entry.is_regular_file())
                    metadata["upscaler_files"]["fsr4/fsr4_shaders/" + rel.generic_string()] =
                        FileHash(entry.path());
            }
        }
        // Reuse the actual game cache, never merge independently numbered shader
        // permutations. Older/custom runtimes retain private-cache behavior.
        const auto cache_module = LoadLibraryExW(Path(q.profile.runtime).c_str(), nullptr,
                                                 DONT_RESOLVE_DLL_REFERENCES);
        bool shared_cache = cache_module && GetProcAddress(cache_module, "BbvrSharedShaderCacheV1");
        if (cache_module) FreeLibrary(cache_module);
        const auto normal_text = Read(q.paths.Config());
        const auto normal_config = normal_text.empty() ? Json::object() : Json::parse(normal_text);
        if (normal_config.contains("Vulkan") &&
            normal_config["Vulkan"].value("pipeline_cache_archived", false)) shared_cache = false;
        const auto shared_cache_root = fs::absolute(q.paths.User() / "cache");
        metadata["shared_shader_cache"] = shared_cache;
        progress(shared_cache
            ? "Reusing the normal game shader cache; new shaders will also be available to gameplay and future scans."
            : "Using a private shader cache (runtime or archived cache does not support sharing).");
        Write(report.folder / "hardware.json", metadata.dump(2));
        progress("Verified bundled save. GPU: " + q.system.gpu + ".");
        std::vector<std::vector<Sample>> all(candidates.size());
        fs::path cache;
        // The limit the current run is measured against: the budget, or for a quick scan's
        // confirmation of a passing screen, QuickScanHardSeconds.
        double run_limit = work_limit;
        while (!cancel) {
            // A confirmed quick recommendation completes the search normally.
            // Do not relabel it as budget-limited merely because another run
            // would not fit in the remaining time.
            if (quick && Recommend(report.results)) break;
            if (elapsed() >= (quick ? QuickScanHardSeconds : work_limit)) {
                report.time_limited = true;
                break;
            }
            size_t begin = adaptive ? candidates.size() : 0;
            if (adaptive) {
                size_t completed_runs = 0;
                for (const auto &samples : all)
                    completed_runs += samples.size();
                // Reserve a confirmation run before spending time on another screen.
                // A screened candidate already has its first measurement.
                const double estimated_run = completed_runs ? elapsed() / completed_runs : 65.0;
                if(!quick && completed_runs && !CanStartLongPair(elapsed(),estimated_run)) {
                    report.time_limited=true;
                    break; // Do not start a new pair which is unlikely to fit.
                }
                const bool screen = completed_runs == 0 ||
                                    elapsed() + 2 * estimated_run * 1.1 < QuickScanWorkSeconds;
                auto next = quick ? NextQuickCandidate(q.system, DlssIncluded(Path(q.profile.runtime)),
                                    Fsr4Included(Path(q.profile.runtime)), report.results, q.target_hz, screen, Fsr31Included(Path(q.profile.runtime)), &q.headset)
                                  : NextCandidate(q.system, DlssIncluded(Path(q.profile.runtime)),
                                    Fsr4Included(Path(q.profile.runtime)), report.results, q.target_hz, Fsr31Included(Path(q.profile.runtime)), &q.headset);
                if (!next)
                    break;
                if (quick && completed_runs) {
                    // A new screen must start inside the budget (and `screen` already left room
                    // for its confirmation). Confirming a passing screen may run on to the hard
                    // limit: a slow PC's runs are long, and it should still get a confirmed answer.
                    const bool confirmation = std::any_of(report.results.begin(), report.results.end(),
                        [&](const auto &r) { return r.candidate.id == next->id && r.provisional; });
                    if (confirmation ? elapsed() + estimated_run * 1.1 > QuickScanHardSeconds
                                     : QuickScanWorkSeconds - elapsed() < estimated_run * .9) {
                        report.time_limited = true;
                        break;
                    }
                    run_limit = confirmation ? QuickScanHardSeconds : work_limit;
                }
                if (!report.results.empty() && next->id != report.results.back().candidate.id) {
                    const auto& previous = report.results.back().candidate;
                    if (next->stereo != previous.stereo)
                        progress("Trying a different stereo mode to compare rendering cost.");
                    else if (next->graphics[2] != previous.graphics[2])
                        progress("Comparing another upscaler/rendering option.");
                    else if (next->graphics[next->graphics[2]==1?3:4] != previous.graphics[previous.graphics[2]==1?3:4])
                        progress("Adjusting internal render resolution with the upscaler preset.");
                    else if (next->graphics[1] > previous.graphics[1])
                        progress("Trying higher resolution to check available performance headroom.");
                    else if (next->graphics[1] < previous.graphics[1])
                        progress("Trying lower resolution to reduce rendering cost.");
                }
                const auto existing = std::find_if(candidates.begin(), candidates.end(),
                    [&](const auto &c) { return c.id == next->id; });
                if (quick && existing != candidates.end())
                    begin = size_t(existing - candidates.begin());
                else {
                    candidates.push_back(*next);
                    all.resize(candidates.size());
                }
            }
            for (unsigned repeat = 0;
                 !cancel && !report.time_limited && (repeat < (quick ? 1u : 2u) || (!quick && adaptive && repeat < 4 &&
                                            std::count_if(
                                                all.back().begin(), all.back().end(),
                                                [](const auto &s) { return s.valid; }) < 2));
                 ++repeat) {
                // Reverse the second pass to reduce systematic ordering bias.
                for (size_t index = begin; index < (quick ? begin + 1 : candidates.size()) && !cancel; ++index) {
                    const size_t i = repeat ? candidates.size() - 1 - (index - begin) : index;
                    const auto &c = candidates[i];
                    const unsigned run_number = quick ? unsigned(all[i].size()) + 1 : repeat + 1;
                    progress(quick ? (run_number == 1 ? "Screening: " : "Confirming: ") + c.label : repeat < 2 ? "Pass " + std::to_string(repeat + 1) + " of 2: " + c.label
                                        : "Retry after an invalid sample: " + c.label);
                    if (fs::space(report.folder).available < 1024ull * 1024 * 1024)
                        throw std::runtime_error("At least 1 GiB of free space is required "
                                                 "to continue benchmarking.");
                    if (FileHash(Path(q.profile.runtime)) !=
                        metadata.at("runtime_sha256").get<std::string>())
                        throw std::runtime_error(
                            "Runtime changed during the benchmark. Start a fresh scan.");
                    const auto dir = report.folder / (c.id + "-" + std::to_string(run_number));
                    run_folders.push_back(dir.filename());
                    fs::create_directory(dir);
                    Paths isolated{dir};
                    Profile p = q.profile;
                    p.refresh_hz = 90; // Start from the audited template before private uncapping.
                    p.follow_headset = false; // The scan owns its controlled display cadence.
                    p.saves = PathText(isolated.User() / "home");
                    p.fullscreen = false;
                    fs::create_directories(Path(p.saves));
                    Write(isolated.Ini(),
                          VR::Ini::ApplyEdits("", {{"graphics_config", GraphicsText(c.graphics)},
                                                   {"dlss_model", c.model},
                                                   {"aa_mode", "smaa"},
                                                   {"dibr_eye_taa", "0.25"}}));
                    // A fresh portable user profile must never invoke migration of the
                    // player's saves.
                    Json users = Json::array();
                    for (int user = 0; user < 4; ++user)
                        users.push_back({{"user_id", 1000 + user},
                                         {"player_index", 1 + user},
                                         {"user_color", 1 + user},
                                         {"user_name", "Benchmark"},
                                         {"shadnet_enabled", false}});
                    Write(isolated.User() / "users.json",
                          Json{{"Users", {{"user", users}, {"commit_hash", ""}}}}.dump(2));
                    auto plan = Prepare(p, isolated, c.stereo);
                    // Prepare validates a new empty home, so no redundant save backup is
                    // needed. Remove only that empty directory, then populate it from the
                    // verified fixture.
                    if (!fs::remove(Path(p.saves)))
                        throw std::runtime_error(
                            "Benchmark home was unexpectedly populated before fixture copy.");
                    CopyFixture(q.package, Path(p.saves));
                    auto config = Json::parse(Read(isolated.Config()));
                    config["GPU"]["vblank_frequency"] = 360;
                    config["Log"] = {
                        {"enable", true}, {"filter", ""}, {"sync", true}, {"append", false}};
                    config["General"]["shad_net_enabled"] = false;
                    config["General"]["connected_to_network"] = false;
                    config["General"]["volume_slider"] = 0;
                    config["Input"]["background_controller_input"] = true;
                    // Firmware/font assets remain read-only in their normal locations.
                    const auto original_config = Read(q.paths.Config());
                    const auto original =
                        original_config.empty() ? Json::object() : Json::parse(original_config);
                    for (const auto *key : {"sys_modules_dir", "font_dir"}) {
                        auto asset = fs::absolute(q.paths.User()) /
                                     (std::string(key) == "font_dir" ? "fonts" : "sys_modules");
                        if (original.contains("General") && original["General"].contains(key) &&
                            original["General"][key].is_string() &&
                            !original["General"][key].get<std::string>().empty()) {
                            asset = fs::path(Wide(original["General"][key].get<std::string>()));
                            // Relative settings belong to the normal launch working directory,
                            // never to the benchmark child's new private directory.
                            if (asset.is_relative())
                                asset = fs::absolute(q.paths.root / asset);
                        }
                        config["General"][key] = PathText(asset.lexically_normal());
                    }
                    Write(isolated.Config(), config.dump(2));
                    Write(isolated.Ini(),
                          VR::Ini::ApplyEdits(Read(isolated.Ini()),
                                              {{"frame_cap", "off"},
                                               {"refresh_hz", std::to_string(q.display_hz)}}));
                    const auto patch = isolated.User() / "patches/bbvr_patch.xml";
                    Write(patch, UncapPatch(Read(patch)));
                    if (!shared_cache && !cache.empty())
                        CopyCache(cache, isolated.User() / "cache");
                    if(!presentation_startup)
                        Write(dir / "input.txt", "wait 20\nbutton_a 0.2\nwait 3.5\nbutton_a 0.2\n");
                    const auto w = std::max(q.headset.width[0], q.headset.width[1]);
                    const auto h = std::max(q.headset.height[0], q.headset.height[1]);
                    const auto size = std::to_wstring(w) + L"x" + std::to_wstring(h);
                    Environment(
                        plan,
                        {{L"XR_RUNTIME_JSON", runtime_json.wstring()},
                         {L"MOCKXR_BENCHMARK_STARTUP",presentation_startup?L"1":L"0"},
                         {L"MOCKXR_QUEST_FOV", L"1"},
                         {L"MOCKXR_HEAD", ScanHead}, // look left and right, dipping down and up
                         {L"MOCKXR_REC", size},
                         {L"MOCKXR_MAXRECT", std::to_wstring(q.headset.max_width) + L"x" +
                                                 std::to_wstring(q.headset.max_height)},
                         {L"MOCKXR_MAX_SWAPCHAIN", std::to_wstring(q.headset.max_width) + L"x" +
                                                       std::to_wstring(q.headset.max_height)},
                         {L"MOCKXR_DUMP_DIR", dir.wstring()},
                         {L"MOCKXR_DUMPS", L"0"},
                         {L"MOCKXR_RATES", std::to_wstring(q.display_hz)},
                         {L"BBVR_BENCHMARK", L"1"},
                         {L"BBVR_BENCHMARK_INPUT_LOCK", L"1"},
                         {L"VR_GPU_TIMING", L"1"},
                         {L"VR_GPU_TIMING_WINDOW_S", std::to_wstring(ScanSweepSeconds)},
                         {L"VR_FRAME_TIMES", L"1"},
                         {L"VR_LATENCY_GPU", L"1"}});
                    if (shared_cache)
                        Environment(plan, {{L"BBVR_SHADER_CACHE_ROOT", shared_cache_root.wstring()}});
                    Json evidence{{"candidate", c.id},
                                  {"repeat", run_number},
                                  {"exe_sha256", metadata["runtime_sha256"]},
                                  {"config_sha256", FileHash(isolated.Config())},
                                  {"ini_sha256", FileHash(isolated.Ini())},
                                  {"patch_sha256", FileHash(patch)}};
                    Write(dir / "run.json", evidence.dump(2));
                    if (RuntimeRunning())
                        throw std::runtime_error(
                            "Another game appeared before a benchmark run; stopped.");
                    if (elapsed() >= run_limit) {
                        report.time_limited = true;
                        break;
                    }
                    progress("Loading the benchmark save automatically; warming up before measurement.");
                    Child child(plan);
                    std::vector<Cpu> cpu;
                    const auto begin = std::chrono::steady_clock::now();
                    unsigned deadline_seconds = q.seconds;
                    // When gameplay began (the first projection): a slow PC loads later, and its
                    // run is extended so it still gets ScanGameplaySeconds of play to measure.
                    std::optional<std::chrono::steady_clock::time_point> gameplay;
                    while (!cancel && !child.Finished()) {
                        if (elapsed() >= run_limit) {
                            report.time_limited = true;
                            break;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (adaptive && !gameplay &&
                            Read(LogFile(isolated)).find("VR projection: ") != std::string::npos)
                            gameplay = now;
                        if (now - begin >= std::chrono::seconds(deadline_seconds)) {
                            const unsigned wanted = gameplay
                                ? unsigned(std::ceil(std::chrono::duration<double>(*gameplay - begin).count())) +
                                      ScanGameplaySeconds
                                : 0;
                            if (wanted > deadline_seconds && deadline_seconds < ScanRunMaxSeconds) {
                                deadline_seconds = std::min(wanted, ScanRunMaxSeconds);
                                progress("Slow start: extending this run to " +
                                         std::to_string(deadline_seconds) +
                                         " s so it measures as much play as any other.");
                            } else {
                                break; // A slow/unstable candidate cannot consume five minutes.
                            }
                        }
                        if (std::chrono::steady_clock::now() - begin >= std::chrono::seconds(adaptive ? 40 : 60)) {
                            const auto partial = Read(LogFile(isolated));
                            // Keep all warmup, sample-count, pacing and provider checks.
                            // Background CPU telemetry is evaluated on the completed range below.
                            if (Analyze(partial, q.headset, c, 0).valid) {
                                progress("Enough warmed-up data collected; evaluating " + c.label);
                                break;
                            }
                        }
                        const auto log = LogFile(isolated);
                        std::error_code ec;
                        const auto bytes = fs::file_size(log, ec);
                        cpu.push_back(ReadCpu(child.process.h, ec ? 0 : size_t(bytes)));
                        for (int tick = 0; tick < 20 && !cancel && !child.Finished(); ++tick)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    evidence["measured_seconds"] =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
                            .count();
                    evidence["extended_for_slow_gameplay"] = deadline_seconds > q.seconds;
                    Write(dir / "run.json", evidence.dump(2));
                    const bool early = child.Finished();
                    const auto text = Read(
                        LogFile(isolated)); // Before shutdown/capture, never include their stalls.
                    child.Stop();
                    if (cancel) {
                        report.cancelled = true;
                        break;
                    }
                    double other = std::numeric_limits<double>::quiet_NaN();
                    if (const auto range = SelectMeasurementRange(text)) {
                        std::vector<Cpu> tail;
                        for (auto &sample : cpu)
                            if (sample.valid && sample.log_bytes >= range->begin &&
                                sample.log_bytes <= range->end)
                                tail.push_back(sample);
                        if (tail.size() > 1) {
                            const auto &a = tail.front();
                            const auto &b = tail.back();
                            const double dt =
                                std::chrono::duration<double>(b.time - a.time).count();
                            if (dt > 0)
                                other = std::max(
                                    0.0, (b.system - a.system - b.process + a.process) / dt);
                        }
                    }
                    auto sample = Analyze(text, q.headset, c, other);
                    if (text.find("GPU_Model: " + q.system.gpu) == std::string::npos) {
                        sample.valid = false;
                        sample.reason = "Measured Vulkan GPU differs from the scanned adapter.";
                    }
                    if (early) {
                        sample.valid = false;
                        sample.reason = "Game exited before the measurement completed.";
                    }
                    if (Read(dir / "mockxr.log").find("ERROR") != std::string::npos) {
                        sample.valid = false;
                        sample.reason = "Controlled runtime reported an error.";
                    }
                    all[i].push_back(sample);
                    cache = isolated.User() / "cache";
                    Write(dir / "measured-log.txt", text);
                    auto result = Summarize(c, all[i], q.target_hz);
                    Write(dir / "result.json", ToJson(result).dump(2));
                    std::ostringstream measured;
                    measured.setf(std::ios::fixed);
                    measured.precision(1);
                    measured << c.label << ": ";
                    if(sample.fps>0) measured << sample.fps << " FPS, " << sample.gpu_ms << " ms GPU. ";
                    else measured << "No usable FPS measurement. ";
                    if(sample.input_width && sample.output_width)
                        measured << "Per eye: internal " << sample.input_width << "x" << sample.input_height
                                 << " -> output " << sample.output_width << "x" << sample.output_height << ". ";
                    if(!sample.valid) measured << "Excluded: " << sample.reason;
                    else if(!result.reliable) measured << "Valid sample; confirmation still required. " << result.reason;
                    else if(result.meets_target) measured << "Confirmed with performance headroom.";
                    else measured << result.reason;
                    progress(measured.str());
                    if (!sample.warning.empty()) progress("Warning: " + sample.warning);
                    // Each completed sample remains recoverable even if a later run fails.
                    report.results.clear();
                    for (size_t n = 0; n < candidates.size(); ++n)
                        report.results.push_back(Summarize(candidates[n], all[n], q.target_hz));
                    Json results = Json::array();
                    for (auto &r : report.results)
                        results.push_back(ToJson(r));
                    Write(report.folder / "results.json", results.dump(2));
                }
            }
            if (!quick && adaptive && !cancel && !report.results.empty() && !report.results.back().reliable)
                progress("Could not confirm this configuration after bounded retries; keeping earlier results and moving on.");
            if (!adaptive || report.time_limited)
                break;
        }
        report.cancelled = cancel.load();
        if (!report.cancelled) {
            report.recommended = Recommend(report.results);
            if (!report.recommended) report.provisional = Recommend(report.results, true);
        }
    } catch (const std::exception &e) {
        report.error = e.what();
    }
    if(report.time_limited) progress("Scan time budget reached; evaluating completed measurements.");
    report.elapsed_seconds = elapsed();
    if (!report.folder.empty()) {
        try {
            Json summary{{"schema", 1},
                         {"cancelled", report.cancelled},
                         {"time_limited", report.time_limited},
                         {"elapsed_seconds", report.elapsed_seconds},
                         {"error", report.error},
                         {"target_hz", q.target_hz},
                         {"recommended", nullptr}, {"provisional", nullptr}};
            if (report.recommended)
                summary["recommended"] = report.results[*report.recommended].candidate.id;
            if (report.provisional)
                summary["provisional"] = report.results[*report.provisional].candidate.id;
            Write(report.folder / "summary.json", summary.dump(2));
            // Keep report evidence; discard private saves, duplicated caches and raw logs.
            // CopyCache has finished using every private cache by this point.
            try {
                for (const auto& run : run_folders) RemoveScanTree(report.folder, run / "user");
                Write(report.folder / "scan-owner.json", Json{{"kind", "bbvr-perf-scan"}, {"complete", true}}.dump());
                PruneScanHistory(q.paths.root / "benchmarks", 3);
            } catch (const std::exception& e) {
                progress("Could not fully clean temporary scan files: " + std::string(e.what()));
            }
        } catch (const std::exception &e) {
            report.recommended.reset();
            report.error = "Could not save the benchmark summary: " + std::string(e.what());
        }
    }
    return report;
}
} // namespace Launcher::Benchmark
