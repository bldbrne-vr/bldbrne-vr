// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core.h"
#include <atomic>
#include <functional>

namespace Launcher::Benchmark {
inline constexpr int QuickScanWorkSeconds = 175;
inline constexpr int QuickScanBudgetSeconds = 180;
// A slow PC's confirmation of a passing screen may run past the 3-minute budget, up to this:
// new candidates still have to fit the budget.
inline constexpr int QuickScanHardSeconds = 360;
// The scan's look-around (the mock runtime's MOCKXR_HEAD: left and right, dipping down and up)
// and the runtime's GPU timing windows (VR_GPU_TIMING_WINDOW_S) share one period, so every
// window holds one whole sweep and moving the head is not mistaken for drift.
inline constexpr int ScanSweepSeconds = 5;
inline constexpr const wchar_t *ScanHead = L"swing:25,nod:6,nodrate:2,period:5";
// Gameplay every run needs, the same on a slow PC as on a fast one: two warm-up windows, five
// measured windows, and slack for the last line. A run extends past Request::seconds to give a
// late start this much, never past ScanRunMaxSeconds.
inline constexpr unsigned ScanGameplaySeconds = 7 * ScanSweepSeconds + 8;
inline constexpr unsigned ScanRunMaxSeconds = 150;
inline constexpr int LongScanBudgetSeconds = 1800;
inline constexpr int LongScanWorkSeconds = 1785;
inline bool CanStartLongPair(double elapsed, double estimated_run) {
    return elapsed >= 0 && estimated_run > 0 && elapsed + 2 * estimated_run * 1.1 < LongScanWorkSeconds;
}
struct Headset {
    std::string name;
    std::array<unsigned, 2> width{}, height{};
    unsigned max_width{}, max_height{};
    float refresh_hz{}; // Zero when the runtime cannot report its active rate.
};
struct Candidate {
    std::string id, label;
    StereoPreset stereo = StereoPreset::Native;
    GraphicsFields graphics = DefaultGraphics;
    std::string model = "builtin";
    int quality = 0; // Conservative ordering, not a measured image-quality score.
};
struct Sample {
    bool valid = false;
    std::string reason;
    double fps{}, gpu_ms{}, worst_window_gpu_p99{}, background_cores{};
    unsigned windows{};
    double frame_p99{}, frame_p999{}; // Published game-frame intervals, rounded
                                      // to 0.1 ms by runtime.
    unsigned frame_intervals{};
    double peak_device_heap_mib{}, device_heap_budget_mib{}, memory_pressure{};
    unsigned input_width{}, input_height{}, output_width{}, output_height{};
    std::string warning;
    std::vector<double> frame_times; // Ordered, selected gameplay intervals; raw log is durable source.
};
struct Result {
    Candidate candidate;
    std::vector<Sample> repeats;
    double fps{}, gpu_ms{};
    bool reliable{}, meets_target{}, near_target{}, provisional{};
    int target_hz = 90;
    std::string reason;
};
struct PacingStats {
    double slow_frame_fraction{}, slow_time_fraction{}, longest_slow_ms{};
    bool acceptable{};
};
PacingStats EvaluatePacing(const Sample &sample, double target_hz);
int TargetRate(int requested, float headset_hz);
int InitialScale(const SystemInfo &system, const Headset &headset, int target_hz);
Headset ScanHeadset(const fs::path &runtime, const std::atomic_bool* cancel = nullptr);
// Retain newest owned, completed reports. Unknown/active directories are never removed.
void PruneScanHistory(const fs::path& root, unsigned keep = 3);
// Validate the declared startup protocol and inspect runtime capability without executing it.
bool UsePresentationStartup(const Json &manifest, const fs::path &runtime);
// The searches' preference order (Candidate::quality), not a measured image-quality score.
int Rank(StereoPreset stereo, int output, int provider, int preset);
std::vector<Candidate> Candidates(const SystemInfo &system, bool dlss, bool fsr4, bool fsr31 = false);
std::optional<Candidate> NextCandidate(const SystemInfo &system, bool dlss, bool fsr4,
                                       const std::vector<Result> &results, int target_hz, bool fsr31 = false, const Headset *headset = nullptr);
// Quick screening never grants recommendation eligibility; confirmation uses Summarize.
std::optional<Candidate> NextQuickCandidate(const SystemInfo &system, bool dlss, bool fsr4,
                                          const std::vector<Result> &results, int target_hz,
                                          bool allow_screening = true, bool fsr31 = false, const Headset *headset = nullptr);
struct MeasurementRange {
    size_t begin{}, end{};
    unsigned windows{};
};
// The scan's timed windows (each one head sweep): discard two in-game GPU windows, then use five
// to twelve. Older 300-frame windows: discard four, then use four to twelve.
std::optional<MeasurementRange> SelectMeasurementRange(const std::string &log);
Sample Analyze(const std::string &log, const Headset &headset, const Candidate &candidate,
               double background_cores);
Result Summarize(const Candidate &candidate, const std::vector<Sample> &repeats, int target_hz);
std::optional<size_t> Recommend(const std::vector<Result> &results, bool provisional = false);
Json ToJson(const Result &result);
// Caller verifies report/runtime/headset freshness and that gameplay is stopped.
// Persist only the measured graphics choice; return the new profile after both writes succeed.
Profile ApplySettings(const Paths &paths, const Profile &profile, const Candidate &candidate,
                      int target_hz, const std::string &expected_ini);
std::string UncapPatch(const std::string &xml);
std::string FileHash(const fs::path &path);
// Only explicit, hash-verified savedata files are copied. No user's live home
// is read.
void CopyFixture(const fs::path &package, const fs::path &destination);
struct Request {
    Profile profile;
    Paths paths;
    fs::path package;
    Headset headset;
    SystemInfo system;
    int target_hz = 0; // Auto: active headset rate, otherwise 90.
    unsigned seconds = 150;
    bool adaptive = true;
    bool quick = true; // Bounded screening plus confirmation; thorough research is opt-in.
    unsigned display_hz = 0; // Auto: target rate. Research overrides do not cap game rendering.
    std::vector<std::string> candidate_ids; // Optional explicit subset for automated validation.
};
struct Report {
    int target_hz = 90;
    fs::path folder;
    std::vector<Result> results;
    std::optional<size_t> recommended;
    std::optional<size_t> provisional;
    bool cancelled{}, time_limited{};
    double elapsed_seconds{};
    std::string error;
};
using Progress = std::function<void(const std::string &)>;
Report Run(const Request &request, std::atomic_bool &cancel, const Progress &progress);
} // namespace Launcher::Benchmark
