// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "common/shader_cache_lock.h"
#include "benchmark.h"
#include "benchmark_runtime/file_io.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
using namespace Launcher;
using namespace Launcher::Benchmark;
static int checks = 0;
void Check(bool pass, const char *message) {
    ++checks;
    if (!pass)
        throw std::runtime_error(message);
}
template <class F> void Throws(F f, const char *message) {
    bool yes = false;
    try {
        f();
    } catch (...) {
        yes = true;
    }
    Check(yes, message);
}
int main(int argc, char **argv) {
    // CPU-only fake OpenXR probe: a service it starts must outlive probe cleanup.
    if (argc > 1 && std::string_view(argv[1]) == "--probe-service") { Sleep(60000); return 0; }
    if (argc > 2 && std::string_view(argv[1]) == "--benchmark-headset") {
        wchar_t delayed[2]{};
        if(GetEnvironmentVariableW(L"BBVR_TEST_DELAY_PROBE",delayed,2)) Sleep(60000);
        const auto self = ExecutableDirectory() / "benchmark_tests.exe";
        auto command = Quote(self.wstring()) + L" --probe-service";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION service{};
        if (!CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &service)) return 2;
        CloseHandle(service.hThread); CloseHandle(service.hProcess);
        Write(fs::absolute(fs::path(argv[2])), Json{{"benchmark_telemetry",1}, {"name", std::to_string(service.dwProcessId)},
              {"width",{1000,1000}}, {"height",{1000,1000}}, {"max_width",8192}, {"max_height",8192}, {"refresh_hz",90}}.dump());
        return 0;
    }
    try {
        const auto probe = ScanHeadset(ExecutableDirectory() / "benchmark_tests.exe");
        const auto service = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, std::stoul(probe.name));
        const bool service_alive = service && WaitForSingleObject(service,0) == WAIT_TIMEOUT;
        if (service) { TerminateProcess(service,0); WaitForSingleObject(service,5000); CloseHandle(service); }
        Check(service_alive, "real headset probe cleanup preserves launched runtime services");
        const auto cancelled_probe = [&](bool pre_cancelled) {
            std::atomic_bool cancel{pre_cancelled};
            SetEnvironmentVariableW(L"BBVR_TEST_DELAY_PROBE",L"1");
            std::jthread timer([&] { if(!pre_cancelled) { Sleep(250); cancel=true; } });
            const auto start=std::chrono::steady_clock::now();
            bool cancelled=false;
            try { ScanHeadset(ExecutableDirectory()/"benchmark_tests.exe",&cancel); }
            catch(const std::exception& e) { cancelled=std::string(e.what())=="Headset detection cancelled."; }
            SetEnvironmentVariableW(L"BBVR_TEST_DELAY_PROBE",nullptr);
            Check(cancelled,"probe reports cancellation");
            Check(std::chrono::steady_clock::now()-start<std::chrono::seconds(3),"cancel does not wait for probe timeout");
        };
        cancelled_probe(false);
        cancelled_probe(true);
        Check(TargetRate(0, 120) == 120, "automatic target uses active headset rate");
        Check(TargetRate(0, 0) == 90 && TargetRate(0, std::nanf("")) == 90,
              "missing headset rate falls back to 90");
        Check(TargetRate(72, 120) == 72, "manual target overrides detected rate");
        Check(!UsePresentationStartup(Json::object(),"missing.dll"),"legacy package retains timed startup");
        Throws([&] { UsePresentationStartup(Json{{"startup_navigation","unknown"}},"missing.dll"); },
               "unknown startup protocol rejected");
        Throws([&] { UsePresentationStartup(Json{{"startup_navigation","presentation-v1"}},"missing.dll"); },
               "missing recovery runtime rejected");
        if(argc>=4) {
            const Json declared{{"startup_navigation","presentation-v1"}};
            Check(UsePresentationStartup(declared,Path(argv[2])),"new runtime advertises presentation startup");
            Throws([&] { UsePresentationStartup(declared,Path(argv[3])); },"old runtime cannot claim presentation startup");
            Check(!UsePresentationStartup(Json::object(),Path(argv[3])),"old runtime remains usable with legacy protocol");
        }
        SystemInfo system;
        system.nvidia = true;
        system.vram = 16ull << 30;
        auto candidates = Candidates(system, true, true);
        Check(candidates.size() == 9, "complete provider matrix");
        const auto with_fsr31 = Candidates(system, true, true, true);
        Check(with_fsr31.size() == 11 &&
                  std::count_if(with_fsr31.begin(), with_fsr31.end(),
                                [](const auto &c) { return c.graphics[2] == 3; }) == 2,
              "packaged FSR 3.1.5 adds Quality and Performance candidates");
        Check(candidates.front().id == "native-100", "native control first");
        system.nvidia = false;
        system.vram = 8ull << 30;
        auto low = Candidates(system, true, false);
        Check(low.size() == 5 && low.front().stereo == StereoPreset::Native &&
                  std::none_of(low.begin(), low.end(),
                               [](const auto &c) { return c.graphics[2] == 1; }),
              "8 GB starts in native stereo (DIBR is the player's choice); no DLSS on non-NVIDIA");
        Sample a{true, "", 120, 7, 9, 1, 12};
        a.frame_p99 = 9;
        auto r = Summarize(candidates[0], {a, a}, 90);
        Check(r.reliable && r.meets_target, "repeatable 90 Hz target has headroom");
        Check(!Summarize(candidates[0], {a}, 90).reliable, "one run cannot recommend");
        auto slow = a;
        slow.fps = 90;
        Check(!Summarize(candidates[0], {slow, slow}, 90).meets_target, "headroom required");
        auto variable = a;
        variable.fps = 150;
        Check(!Summarize(candidates[0], {a, variable}, 110).reliable,
              "reject inconsistent repeats that put the target in doubt");
        const auto spread = Summarize(candidates[0], {a, variable}, 90);
        Check(spread.reliable && spread.fps == 120,
              "inconsistent repeats far above the target count at the slower rate");
        auto tail = a;
        tail.worst_window_gpu_p99 = 12;
        Check(Summarize(candidates[0], {tail, tail}, 90).meets_target,
              "isolated GPU tail does not override steady published pacing");
        auto pacing = a;
        pacing.frame_p99 = 15;
        Check(!Summarize(candidates[0], {pacing, pacing}, 90).meets_target,
              "CPU/publish pacing misses target despite fast GPU");
        auto pressure = a;
        pressure.memory_pressure = .95;
        Check(!Summarize(candidates[0], {pressure, pressure}, 90).meets_target,
              "GPU heap budget headroom required");
        auto invalid = a;
        invalid.valid = false;
        Check(!Summarize(candidates[0], {a, invalid}, 90).reliable, "invalid repeats excluded");
        const auto provisional = Summarize(candidates[0], {a}, 90);
        Check(provisional.provisional && !provisional.reliable && provisional.fps == a.fps,
              "single run preserves measurement with provisional confidence");
        Check(!Recommend({provisional}) && Recommend({provisional}, true) == 0,
              "provisional is explicit and never confirmed");
        Check(!Recommend({Summarize(candidates[0], {pacing}, 90)}, true), "provisional requires pacing");
        Check(!Recommend({Summarize(candidates[0], {pressure}, 90)}, true), "provisional requires memory headroom");
        Check(!Recommend({Summarize(candidates[0], {invalid}, 90)}, true), "invalid cannot recommend provisionally");
        Check(!Recommend({Summarize(candidates[0], {a, variable}, 90)}, true), "inconsistent repeats cannot become provisional");
        auto lower = Summarize(candidates[2], {variable, variable}, 90);
        Check(Recommend({lower, r}) == 1,
              "prefer conservative full resolution when it meets target");
        auto near_sample = a;
        near_sample.fps = 82.8;
        near_sample.frame_p99 = 11.9;
        near_sample.worst_window_gpu_p99 = 11.9;
        auto near_result = Summarize(candidates[0], {near_sample, near_sample}, 90);
        Check(near_result.reliable && near_result.near_target && !near_result.meets_target,
              "eight percent boundary is near target without headroom");
        Check(Recommend({near_result}) == 0, "near target can be recommended");
        Check(Recommend({near_result, lower}) == 1 && Recommend({lower, near_result}) == 0,
              "headroom preferred over higher quality near target in either order");
        near_sample.fps = 82.7;
        Check(!Recommend({Summarize(candidates[0], {near_sample, near_sample}, 90)}),
              "below eight percent tolerance cannot recommend");
        near_sample.fps = 86;
        near_sample.frame_p99 = 12.1;
        Check(!Summarize(candidates[0], {near_sample, near_sample}, 90).near_target,
              "near target still requires acceptable pacing");
        near_sample.frame_p99 = 11.9;
        near_sample.memory_pressure = .95;
        Check(!Summarize(candidates[0], {near_sample, near_sample}, 90).near_target,
              "near target retains memory reserve");
        near_sample = a;
        near_sample.fps = 120 * .92;
        near_sample.frame_p99 = 8.8;
        near_sample.worst_window_gpu_p99 = 8.8;
        Check(Summarize(candidates[0], {near_sample, near_sample}, 120).near_target,
              "tolerance scales with headset target");
        Check(!r.near_target && ToJson(near_result).at("target_tolerance_percent") == 8,
              "headroom and near target are distinct and report tolerance");
        auto dips = a;
        dips.frame_times.assign(1000, 10.0);
        for (size_t i = 0; i < 1000; i += 100)
            dips.frame_times[i] = 20;
        Check(EvaluatePacing(dips, 90).acceptable &&
                  Summarize(candidates[0], {dips, dips}, 90).meets_target,
              "occasional isolated small drops are acceptable");
        for (size_t i = 0; i < 1000; i += 10)
            dips.frame_times[i] = 20;
        Check(!EvaluatePacing(dips, 90).acceptable,
              "frequent drops fail despite a good median FPS");
        dips.frame_times.assign(1000, 10.0);
        for (size_t i = 100; i < 106; ++i)
            dips.frame_times[i] = 20;
        Check(!EvaluatePacing(dips, 90).acceptable,
              "sustained 120ms slow burst fails despite low aggregate frequency");
        dips.frame_times.assign(1000, 10.0);
        dips.frame_times[100] = 250;
        Check(!EvaluatePacing(dips, 90).acceptable, "rare freeze is not a small dip");
        dips.frame_times.assign(1000, 11.8);
        Check(!EvaluatePacing(dips, 90).acceptable && EvaluatePacing(dips, 90 * .92).acceptable,
              "pacing uses eight percent tolerance only for near target tier");
        SystemInfo quick_system;
        quick_system.nvidia = true;
        quick_system.vram = 16ull << 30;
        Headset large{"large",{4000,4000},{4000,4000},8192,8192,90};
        Check(InitialScale(quick_system,large,90)==50,"large headset starts conservatively");
        Headset quest{"quest",{1840,1840},{2016,2016},3680,4032,90};
        Check(InitialScale(quick_system,quest,90)==100,"moderate headset fits starting pixel budget");
        auto modest=quick_system; modest.vram=8ull<<30;
        Check(InitialScale(modest,quest,90)==70,"8 GiB seed reduces initial pixel load");
        Check(InitialScale(quick_system,quest,120)<InitialScale(quick_system,quest,90),
              "higher refresh reduces initial work");
        const auto seeded=NextQuickCandidate(modest,false,false,{},90,true,false,&quest);
        Check(seeded && seeded->id=="base-native-70","8 GiB starts native with cautious resolution");
        const auto nv_first=NextQuickCandidate(quick_system,true,true,{},90,true,true,&quest);
        Check(nv_first && nv_first->id=="upscale-1-2-100","NVIDIA scan starts with DLSS Quality");
        auto amd_first_system=modest; amd_first_system.nvidia=false;
        const auto amd_first=NextQuickCandidate(amd_first_system,true,true,{},90,true,true,&quest);
        Check(amd_first && amd_first->id=="upscale-3-2-70","AMD starts with FSR3.1.5 at conservative output");
        {
            // A driver without native stereo's multiview (a gray right eye): DIBR only, everywhere.
            auto limited=quick_system; limited.nvidia=false;
            limited.multiview_known=limited.multiview_limited=true;
            const auto quick_limited=NextQuickCandidate(limited,true,true,{},90,true,true,&quest);
            Check(quick_limited && quick_limited->stereo==StereoPreset::Dibr,
                  "limited multiview: the quick scan starts in DIBR");
            const auto all=Candidates(limited,true,true,true);
            Check(!all.empty() && std::none_of(all.begin(),all.end(),[](const Candidate& c){
                      return c.stereo==StereoPreset::Native;}),
                  "limited multiview: no native-stereo candidates");
            const auto thorough=NextCandidate(limited,true,true,{},90,true,&quest);
            Check(thorough && thorough->stereo==StereoPreset::Dibr,
                  "limited multiview: the thorough scan proposes DIBR");
            const auto plain=NextCandidate(limited,true,true,{},90,true,nullptr);
            Check(!plain || plain->stereo==StereoPreset::Dibr,
                  "limited multiview: the fixed scan never proposes native stereo");
        }
        {
            // An AMD RX 400 to RX 5000 starts in DIBR; native stereo (layers) is still measured.
            auto older=quick_system; older.nvidia=false; older.amd=older.auto_layers=older.auto_dibr=true;
            const auto quick_older=NextQuickCandidate(older,true,true,{},90,true,true,&quest);
            Check(quick_older && quick_older->stereo==StereoPreset::Dibr,
                  "RX 400 to RX 5000: the quick scan starts in DIBR");
            const auto all=Candidates(older,true,true,true);
            Check(!all.empty() && all.front().stereo==StereoPreset::Dibr &&
                      std::any_of(all.begin(),all.end(),[](const Candidate& c){
                          return c.stereo==StereoPreset::Native;}),
                  "RX 400 to RX 5000: DIBR candidates first, native ones kept");
        }
        auto quick = *nv_first;
        auto high=a; high.fps=120; high.gpu_ms=7; high.frame_p99=9;
        auto small=quick; small.graphics[1]=-30; small.id="upscale-1-2-70";
        auto raised=NextQuickCandidate(quick_system,true,true,{Summarize(small,{high},90)},90,true,true);
        Check(raised && raised->id==small.id,
              "120 FPS at 70 percent holds when the next size would spend the reserve");
        auto lots=high; lots.fps=141; lots.gpu_ms=6.69;
        raised=NextQuickCandidate(quick_system,true,true,{Summarize(small,{lots},72)},72,true,true);
        Check(raised && raised->graphics[1]==-20, "recorded 72 Hz headroom predicts a conservative 80 percent output");
        {
            // 2026-10-22, RTX 5080 at 2448x2672 per eye and 72 Hz. Repeats that disagree
            // (background CPU) still count when even the slower one clears the target.
            auto fast=lots; fast.fps=153; fast.gpu_ms=6.0; fast.frame_p99=8;
            auto busy=lots; busy.fps=128; busy.gpu_ms=6.6; busy.frame_p99=10;
            const auto two=Summarize(small,{fast,busy},72);
            Check(two.reliable && two.fps==128 && two.meets_target,
                  "disagreeing repeats far above target are judged by the slower one");
            const auto doubt=Summarize(small,{fast,busy},120);
            Check(!doubt.reliable, "disagreeing repeats near the target stay unreliable");
            // Strong headroom at 70% predicts 90% while keeping the reserve, even
            // after the initial screening budget.
            const auto up=NextQuickCandidate(quick_system,true,true,{Summarize(small,{fast},72)},72,false,true);
            Check(up && up->graphics[1]==-10 && up->graphics[2]==1,
                  "153 FPS at 70% for 72 Hz predicts 90% while preserving the reserve");
            auto at90c=quick; at90c.graphics[1]=-10; at90c.id="upscale-1-2-90";
            auto ninety=lots; ninety.fps=107; ninety.gpu_ms=8.5; ninety.frame_p99=10;
            const auto then=NextQuickCandidate(quick_system,true,true,{Summarize(at90c,{ninety},72)},72,false,true);
            Check(then && then->graphics[1]==0, "90% with headroom tests 100%");
            auto tight=lots; tight.fps=75; tight.gpu_ms=12.5; tight.frame_p99=14;
            const auto done=NextQuickCandidate(quick_system,true,true,
                {Summarize(small,{fast},72),Summarize(at90c,{tight},72)},72,true,true);
            Check(!done || done->graphics[1]!=0, "no headroom at 90%: no 100% probe");
        }
        {
            // The whole quick scan as the runner drives it: one run per step, the runner's stop
            // rule and budget (59 s runs, as measured), the 2026-10-22 RTX 5080 rates.
            Headset quest3{"quest",{2448,2448},{2672,2672},4896,5344,72};
            const auto scan=[&](auto rate, double run=59) {
                std::vector<Candidate> tried;
                std::vector<std::vector<Sample>> runs;
                std::vector<Result> results;
                double elapsed=0;
                size_t total=0;
                for (int step=0; step<10; ++step) {
                    const auto chosen=Recommend(results);
                    if(chosen && results[*chosen].meets_target) break;
                    const bool screen=total==0 || elapsed+2*run*1.1<QuickScanWorkSeconds;
                    auto next=NextQuickCandidate(quick_system,true,true,results,72,screen,true,&quest3);
                    if (!next) break;
                    const bool known=std::any_of(results.begin(),results.end(),
                        [&](const auto& r){return r.candidate.id==next->id && r.provisional;});
                    if (total && !known && elapsed+2*run*1.1>QuickScanHardSeconds)
                        if (const auto best=Recommend(results,true)) next=results[*best].candidate;
                    if (total && elapsed+run*1.1>QuickScanHardSeconds) break;
                    size_t i=0;
                    while (i<tried.size() && tried[i].id!=next->id) ++i;
                    if (i==tried.size()) { tried.push_back(*next); runs.emplace_back(); }
                    const int output=100+next->graphics[1];
                    auto s=lots; s.fps=rate(output); s.gpu_ms=1000.0/s.fps*.9; s.frame_p99=1000.0/s.fps+1;
                    runs[i].push_back(s);
                    elapsed+=run; ++total;
                    results.clear();
                    for (size_t k=0;k<tried.size();++k) results.push_back(Summarize(tried[k],runs[k],72));
                }
                const auto best=Recommend(results);
                return std::make_pair(best ? std::optional<Result>(results[*best]) : std::nullopt, total);
            };
            const auto [full,full_runs]=scan([](int o){ return o<=70 ? 153.0 : o<=90 ? 107.0 : 98.8; });
            Check(full && full->candidate.graphics[1]==0 && full->candidate.graphics[2]==1 && full->reliable,
                  "the scan recommends a confirmed DLSS Quality 100% for the RTX 5080");
            Check(full_runs==4, "70% -> 90% -> 100% -> confirm: four runs");
            // A jump that overshoots (100% too slow): the middle once, then confirm.
            const auto [mid,mid_runs]=scan([](int o){ return o<=70 ? 190.0 : o<=90 ? 90.0 : 70.0; });
            Check(mid && mid->candidate.graphics[1]==-10 && mid->reliable,
                  "an overshooting jump settles on a confirmed 90%");
            Check(mid_runs<=4, "the overshoot costs one run");
            // A slow PC (150 s runs): no time for a climb and its confirmation, so the passing
            // first choice is confirmed instead of ending without an answer.
            const auto [slow,slow_runs]=scan([](int o){ return o<=70 ? 153.0 : 98.8; }, 150);
            Check(slow && slow->candidate.graphics[1]==-30 && slow->reliable && slow_runs==2,
                  "long runs confirm the passing choice within the hard limit");
            // A normal one-minute run already consumes the optional screening budget.
            // A slow first result must still descend, then confirm a usable fallback.
            const auto [down,down_runs]=scan([](int o){ return o<=50 ? 90.0 : o<=60 ? 65.0 : 50.0; });
            Check(down && down->candidate.graphics[1]==-50 && down->reliable && down->meets_target,
                  "slow first run descends to 50% and confirms comfortable performance");
            Check(down_runs==3, "downward recovery confirms the comfortable floor without a needless climb");
            const auto [near_choice,near_runs]=scan([](int o){ return o<=60 ? 90.0 : 68.0; });
            Check(near_choice && near_choice->meets_target && near_choice->candidate.graphics[1]==-50 && near_choice->candidate.stereo==StereoPreset::Dibr,
                  "ineffective preset reduction tests DIBR at a conservative output to find headroom");
            Check(near_runs==4, "detecting ineffective preset reductions saves a measured run");
            const auto [unreachable,unreachable_runs]=scan([](int){ return 30.0; });
            Check(!unreachable && unreachable_runs<=QuickScanMaxRuns,
                  "unreachable target stops within the limits without recommending a slow choice");
        }
        auto at90=quick;at90.graphics[1]=-10;at90.id="upscale-1-2-90";
        auto enormous=high;enormous.fps=300;enormous.gpu_ms=2;enormous.frame_p99=4;
        const auto capped=NextQuickCandidate(quick_system,true,true,{Summarize(at90,{enormous},90)},90,true,true);
        Check(capped && capped->graphics[1]==0,"even extreme headroom cannot exceed headset output");
        auto slow_probe=a; slow_probe.fps=80; slow_probe.gpu_ms=12; slow_probe.frame_p99=13;
        auto at80=quick;at80.graphics[1]=-20;at80.id="upscale-1-2-80";
        auto next_quick=NextQuickCandidate(quick_system,true,true,{Summarize(at80,{slow_probe},90)},90,true,true);
        Check(next_quick && next_quick->graphics[1]<-20,
              "GPU shortfall lowers resolution to build the larger reserve");
        slow_probe.fps=60;slow_probe.gpu_ms=16;
        next_quick=NextQuickCandidate(quick_system,true,true,{Summarize(at80,{slow_probe},90)},90,true,true);
        // Aim at 108 FPS: straight to the predicted output (80/sqrt(1.8) -> 50%),
        // keeping Quality, rather than a preset step that could not close the gap.
        Check(next_quick && next_quick->graphics[3]==2 && next_quick->graphics[1]==-50,
              "large GPU shortfall lowers output straight to the predicted size");
        {
            // Within 8% but short of the 10% reserve: one step down is tried before settling.
            auto near_pass=a; near_pass.fps=88; near_pass.gpu_ms=10.5; near_pass.frame_p99=11;
            auto at100=quick;at100.graphics[1]=0;at100.id="upscale-1-2-100";
            const auto step=NextQuickCandidate(quick_system,true,true,{Summarize(at100,{near_pass},90)},90,true,true);
            Check(step && step->graphics[1]==0 && step->graphics[2]==1 && step->graphics[3]==1,
                  "near-target pass tries one preset step down before confirming");
            // That step passing with headroom is confirmed ahead of the higher-ranked near pass.
            auto roomy=a; roomy.fps=104; roomy.gpu_ms=8.5; roomy.frame_p99=10;
            const auto confirm=NextQuickCandidate(quick_system,true,true,
                {Summarize(at100,{near_pass},90),Summarize(*step,{roomy},90)},90,true,true);
            Check(confirm && confirm->id==step->id,
                  "a step with headroom is confirmed ahead of a higher-ranked near-target pass");
            // If the step fails, the near-target pass stays the fallback to confirm.
            auto failed=a; failed.fps=70; failed.gpu_ms=14; failed.frame_p99=15;
            const auto fallback=NextQuickCandidate(quick_system,true,true,
                {Summarize(at100,{near_pass},90),Summarize(*step,{failed},90)},90,true,true);
            Check(fallback && fallback->id!=at100.id, "a failed step keeps searching beyond the limited-headroom fallback");
            // Ranking: one preset step weighs as much as 10 points of output.
            Check(Rank(StereoPreset::Native,90,1,2)>Rank(StereoPreset::Native,100,1,0) &&
                      Rank(StereoPreset::Native,100,0,2)>Rank(StereoPreset::Native,100,1,1) &&
                      Rank(StereoPreset::Native,100,1,2)>Rank(StereoPreset::Native,100,0,2) &&
                      Rank(StereoPreset::Native,80,1,2)>Rank(StereoPreset::Dibr,100,0,2),
                  "rank: Quality 90% > Performance 100%, native 100% > Balanced, DLSS Quality > native");
        }
        auto perf=at80; perf.graphics[3]=perf.graphics[4]=0; perf.id="upscale-1-0-80";
        next_quick=NextQuickCandidate(quick_system,true,true,{Summarize(perf,{slow_probe},90)},90,true,true);
        Check(next_quick && next_quick->graphics[1]<-20 && next_quick->graphics[3]==0,
              "Performance shortfall reduces output using measured estimate");
        auto cpu=slow_probe;cpu.fps=50;cpu.gpu_ms=5;
        next_quick=NextQuickCandidate(quick_system,true,true,{Summarize(at80,{cpu},90)},90,false,true);
        Check(next_quick && next_quick->stereo==StereoPreset::Dibr && next_quick->graphics[2]==0,
              "severe CPU dominated shortfall tests DIBR without unsupported upscaler");
        std::vector<Result> probes{Summarize(small,{high},90)};
        next_quick=NextQuickCandidate(quick_system,true,true,probes,90,false,true);
        Check(next_quick && next_quick->id==small.id,
              "a quantized increase must preserve predicted reserve even after screening budget");
        auto failed=at80; probes.push_back(Summarize(failed,{slow_probe},90));
        next_quick=NextQuickCandidate(quick_system,true,true,probes,90,true,true);
        Check(next_quick && next_quick->id==small.id,"failed higher output returns to passing fallback");
        probes[0]=Summarize(small,{high,high},90);
        Check(!NextQuickCandidate(quick_system,true,true,probes,90,true,true),"confirmed fallback ends scan");
        probes={Summarize(quick,{high},90)};
        next_quick=NextQuickCandidate(quick_system,true,true,probes,90,true,true);
        Check(next_quick && next_quick->id==quick.id,"full output confirms immediately instead of sweeping providers");
        probes[0]=Summarize(quick,{high,high},90);
        Check(!NextQuickCandidate(quick_system,true,true,probes,90,true,true),"confirmed preferred option finishes");
        probes={Summarize(quick,{invalid},90)};
        next_quick=NextQuickCandidate(quick_system,true,true,probes,90,false,true);
        Check(next_quick && next_quick->id==quick.id,"invalid probe retries without inferring slowness after screening budget");
        probes={Summarize(small,{high,variable},110)};
        next_quick=NextQuickCandidate(quick_system,true,true,probes,110,true,true);
        Check(next_quick && (next_quick->graphics[1]<small.graphics[1] || next_quick->graphics[3]<small.graphics[3]),
              "inconsistent repeats that put the reserve in doubt drive a cheaper trial");
        probes={Summarize(small,{high,variable},90)};
        Check(!NextQuickCandidate(quick_system,true,true,probes,90,true,true),
              "inconsistent repeats far above the target confirm the choice (only a final choice "
              "gets a second run)");
        auto boundary_probe=a;boundary_probe.fps=82.8;boundary_probe.gpu_ms=11.9;boundary_probe.frame_p99=12.0;
        Check(NextQuickCandidate(quick_system,true,true,{Summarize(quick,{boundary_probe},90)},90,false,true).has_value(),"eight percent boundary continues searching for headroom");
        boundary_probe.fps=82.7;
        next_quick=NextQuickCandidate(quick_system,true,true,{Summarize(quick,{boundary_probe},90)},90,false,true);
        Check(next_quick && next_quick->id!=quick.id && (next_quick->graphics[3]<quick.graphics[3] || next_quick->graphics[1]<quick.graphics[1]),
              "below tolerance descends rather than confirming or stopping after screening budget");
        auto pressure_probe=high;pressure_probe.memory_pressure=.95;
        next_quick=NextQuickCandidate(quick_system,true,true,{Summarize(at80,{pressure_probe},90)},90,false,true);
        Check(next_quick && next_quick->graphics[1]<at80.graphics[1],"memory pressure lowers output rather than only internal preset");
        Check(CanStartLongPair(1600,60) && !CanStartLongPair(1700,60),
              "long scan reserves time for two runs");
        Check(!CanStartLongPair(1785,1) && !CanStartLongPair(0,0),
              "long scan cannot start at deadline or with invalid estimate");
        Headset big_long{"large",{2448,2448},{2672,2672},8192,8192,72};
        auto long_first=NextCandidate(quick_system,true,true,{},72,true,&big_long);
        Check(long_first && long_first->id=="upscale-1-2-70","long scan starts with preferred temporal provider");
        auto long_up=NextCandidate(quick_system,true,true,{Summarize(*long_first,{lots,lots},72)},72,true,&big_long);
        Check(long_up && long_up->graphics[1]==-20 && long_up->graphics[2]==1,"long scan raises output conservatively on headroom");
        std::vector<Result> long_history;
        for(int provider:{1,3,2,0,-1}) {
            auto c=*long_first;c.graphics[2]=std::max(0,provider);
            c.stereo=provider<0?StereoPreset::Dibr:StereoPreset::Native;
            c.id=provider>0?"upscale-"+std::to_string(provider)+"-2-70":provider==0?"base-native-70":"base-dibr-70";
            long_history.push_back(Summarize(c,{invalid,invalid},72));
        }
        const auto balanced_long=NextCandidate(quick_system,true,true,long_history,72,true,&big_long);
        Check(balanced_long && balanced_long->graphics[2]==1 && balanced_long->graphics[3]==1,
              "failed Quality and baseline runs do not block Balanced exploration");
        long_history={Summarize(*long_first,{lots,lots},72),Summarize(*long_up,{slow_probe,slow_probe},72)};
        const auto bounded_long=NextCandidate(quick_system,true,true,long_history,72,true,&big_long);
        Check(bounded_long && bounded_long->graphics[2]==3,
              "long scan moves to next provider at measured resolution boundary");
        SystemInfo adaptive_system;
        adaptive_system.nvidia = true;
        adaptive_system.vram = 16ull << 30;
        std::vector<Result> history;
        auto comfortable = a;
        comfortable.worst_window_gpu_p99 = 7;
        auto next = NextCandidate(adaptive_system, true, true, history, 90);
        Check(next && next->id == "base-native-100", "search starts with unscaled base");
        auto critical = a;
        critical.fps = 50;
        std::vector<Result> fallback_history{Summarize(*next, {critical, critical}, 90)};
        auto fallback = NextCandidate(adaptive_system, true, true, fallback_history, 90);
        Check(fallback && fallback->id == "base-dibr-100",
              "severe native shortfall tests DIBR at the same output first");
        fallback_history.push_back(Summarize(*fallback, {slow, slow}, 90));
        fallback = NextCandidate(adaptive_system, true, true, fallback_history, 90);
        Check(fallback && fallback->id == "base-dibr-90",
              "DIBR base search has priority after severe native shortfall");
        fallback_history.back() =
            Summarize(fallback_history.back().candidate, {comfortable, comfortable}, 90);
        fallback = NextCandidate(adaptive_system, true, true, fallback_history, 90);
        Check(fallback && fallback->id == "base-native-90",
              "finding DIBR headroom still allows a measured native comparison");
        fallback_history.resize(1);
        fallback_history[0].reliable = false;
        fallback = NextCandidate(adaptive_system, true, true, fallback_history, 90);
        Check(fallback && fallback->id == "base-native-90",
              "unreliable low FPS does not trigger DIBR priority");
        critical.fps = 75;
        fallback_history = {Summarize(*next, {critical, critical}, 120)};
        fallback = NextCandidate(adaptive_system, true, true, fallback_history, 120);
        Check(fallback && fallback->id == "base-dibr-100",
              "DIBR priority threshold scales with target refresh rate");
        history.push_back(Summarize(*next, {slow, slow}, 90));
        next = NextCandidate(adaptive_system, true, true, history, 90);
        Check(next && next->id == "base-native-90", "base drops when headroom is missing");
        history.push_back(Summarize(*next, {comfortable, comfortable}, 90));
        next = NextCandidate(adaptive_system, true, true, history, 90);
        Check(next && next->id == "base-dibr-100", "separate DIBR base is measured");
        history.push_back(Summarize(*next, {comfortable, comfortable}, 90));
        next = NextCandidate(adaptive_system, true, true, history, 90);
        Check(next && next->id == "upscale-1-2-100", "reconstruction follows measured base budget");
        history.push_back(Summarize(*next, {slow, slow}, 90));
        next = NextCandidate(adaptive_system, true, true, history, 90);
        Check(next && next->id == "upscale-1-2-90",
              "expensive reconstruction backs down output resolution");
        Headset h{"Test", {1816, 1816}, {1968, 1968}, 4096, 4096};
        Check(!Analyze("", h, candidates[0], 0).valid, "empty log rejected");
        Check(!Analyze("", h, candidates[0], std::nan("")).valid, "unavailable load rejected");
        Check(!Analyze("", h, candidates[0], 1.6).valid,
              "background load above clean-run limit rejected");
        {
#ifndef BBVR_BENCHMARK_TEST_ROOT
#define BBVR_BENCHMARK_TEST_ROOT ".."
#endif
            const fs::path root = argc > 1 ? Path(argv[1]) : Path(BBVR_BENCHMARK_TEST_ROOT);
            auto xml = Read(root / "launcher/bbvr_patch.xml");
            Check(UncapPatch(xml) != xml, "audited private deadline transformation");
            const auto fast_patch = PatchFrameRate(xml, 120);
            Check(fast_patch.find("41C74424188988083C") != std::string::npos,
                  "120 FPS deadline is encoded in private patch");
            Check(VR::Ini::ValueOf(LaunchIni("custom_option=keep\n", StereoPreset::Native, 120),
                                   "refresh_hz") == "120",
                  "launch pacing follows selected target");
            Check(Json::parse(LaunchConfig("{}", fs::temp_directory_path(),
                                           120))["GPU"]["vblank_frequency"] == 120,
                  "vblank follows selected target");
            Throws([&] { PatchFrameRate(xml, 0); }, "invalid frame-rate patch rejected");

            Throws([&] { UncapPatch("<Patch/>"); }, "missing deadline rejected");
            Headset quest{"Meta Quest 3", {1840, 1840}, {2016, 2016}, 3680, 4032};
            const auto log = Read(root / "launcher/testdata/benchmark_native.txt");
            // Synthetic slow-game telemetry verifies the parser, not a measured performance claim.
            std::ostringstream slow_log;
            slow_log << "VR resolution: headset L 1840x2016 R 1840x2016, scale +0%, "
                        "scene/post/output 1840x2016\n"
                     << "VR projection: image 1840x2016,\n";
            for (int window = 0; window < 8; ++window) {
                slow_log << "VR gpu frame: 300 game frames: GPU busy per frame avg 25.0 / p50 25.0 "
                            "/ p99 28.0\n"
                         << "VR native stereo: 300 pairs, 1840x2016 per-eye source\n"
                         << "VR perf: game frames 30.0/s, XR frames 90.0/s; VRAM 4000/15000 MB\n"
                         << "VR frame times: 300 intervals (ms):";
                for (int frame = 0; frame < 300; ++frame)
                    slow_log << " 33.3";
                slow_log << '\n';
            }
            const auto slow_measurement = Analyze(slow_log.str(), quest, candidates[0], .2);
            Check(
                slow_measurement.valid && slow_measurement.windows == 4 &&
                    slow_measurement.fps == 30,
                "slow gameplay can be measured after real warmup without twelve trailing windows");
            Check(!Summarize(candidates[0], {slow_measurement, slow_measurement}, 90).meets_target,
                  "valid slow samples fail the target and permit searching a lower base");
            {
                // A 10 FPS PC with the scan's 5-second windows: measured after two warm-up
                // windows from five, as soon as a fast PC would be; 300-frame windows would have
                // needed eight windows of 30 s each.
                std::ostringstream timed_log;
                timed_log << "VR gpu frame: windows of 5.0 s\n"
                          << "VR resolution: headset L 1840x2016 R 1840x2016, scale +0%, "
                             "scene/post/output 1840x2016\n"
                          << "VR projection: image 1840x2016,\n";
                for (int window = 0; window < 7; ++window) {
                    timed_log << "VR gpu frame: 50 game frames: GPU busy per frame avg 80.0 / p50 80.0 "
                                 "/ p99 85.0\n"
                              << "VR native stereo: 50 pairs, 1840x2016 per-eye source\n"
                              << "VR perf: game frames 10.0/s, XR frames 90.0/s; VRAM 4000/15000 MB\n"
                              << "VR frame times: 50 intervals (ms):";
                    for (int frame = 0; frame < 50; ++frame)
                        timed_log << " 100.0";
                    timed_log << '\n';
                }
                const auto timed = Analyze(timed_log.str(), quest, candidates[0], .2);
                Check(timed.valid && timed.windows == 5 && timed.fps == 10,
                      "a very slow PC is measured from timed windows after ten seconds of warm-up");
                std::string six = "VR gpu frame: windows of 5.0 s\nVR projection: image 1840x2016,\n";
                for (int i = 0; i < 6; ++i)
                    six += "VR gpu frame: 61 game frames:\n";
                Check(!SelectMeasurementRange(six), "timed windows need two warm-up and five measured");
                six += "VR gpu frame: 58 game frames:\n";
                const auto seven = SelectMeasurementRange(six);
                Check(seven && seven->windows == 5, "seven timed windows give five measured");
            }
            std::string menu_windows;
            for (int i = 0; i < 20; ++i)
                menu_windows += "VR gpu frame: 300 game frames:\n";
            menu_windows += "VR projection: image 1840x2016,\n";
            for (int i = 0; i < 7; ++i)
                menu_windows += "VR gpu frame: 300 game frames:\n";
            Check(!SelectMeasurementRange(menu_windows),
                  "fast menu windows cannot satisfy gameplay warmup");
            const auto measured = Analyze(log, quest, candidates[0], .285);
            Check(measured.valid, "recorded native run accepted");
            const auto contaminated = Analyze(log, quest, candidates[0], 1.6);
            Check(contaminated.valid && contaminated.fps == measured.fps &&
                      contaminated.gpu_ms == measured.gpu_ms &&
                      contaminated.warning.find("CPU") != std::string::npos,
                  "background CPU activity warns without rejecting usable evidence");
            const auto unknown_cpu = Analyze(log, quest, candidates[0], std::nan(""));
            Check(unknown_cpu.valid && !unknown_cpu.warning.empty(), "missing CPU telemetry warns");
            const auto busy = Summarize(candidates[0], {contaminated, contaminated}, 90);
            Check(busy.reliable && Recommend({busy}).has_value(), "consistent busy runs can confirm");
            Check(ToJson(busy)["repeats"][0]["warning"] == contaminated.warning, "warning exported");
            Check(!Analyze(log + "\nBenchmark fatal exception: startup failure\n", quest,
                           candidates[0], .285)
                       .valid,
                  "release benchmark fatal errors reject otherwise valid evidence");
            Check(measured.input_width == 1840 && measured.input_height == 2016 &&
                      measured.output_width == 1840 && measured.output_height == 2016,
                  "recorded input/output dimensions retained");
            Check(std::abs(measured.fps - 120.2) < .01 && measured.frame_p99 == 9.9,
                  "slowest recorded throughput window and pacing parsed");
            Check(measured.memory_pressure > 0 && measured.memory_pressure < .9,
                  "recorded heap budget parsed");
            quest.width[0] = 2000;
            Check(!Analyze(log, quest, candidates[0], .285).valid, "wrong headset rejected");
            quest.width[0] = 1840;
            Check(!Analyze(log, quest, candidates[1], .285).valid,
                  "missing requested DLSS rejected");
            Check(!Analyze(log, quest, candidates[7], .285).valid,
                  "native run cannot count as DIBR");
            auto without = [&](const std::string &marker) {
                std::istringstream input(log);
                std::string line, out;
                while (std::getline(input, line))
                    if (line.find(marker) == std::string::npos)
                        out += line + "\n";
                return out;
            };
            Check(!Analyze(without("VR native stereo:"), quest, candidates[0], .285).valid,
                  "missing native evidence rejected");
            Check(!Analyze(without("VR frame times:"), quest, candidates[0], .285).valid,
                  "missing pacing evidence rejected");
            const auto temp = fs::temp_directory_path() /
                              ("bbvr-benchmark-tests-" + std::to_string(GetCurrentProcessId()));
            fs::create_directories(temp / "package");
            Check(Common::PrepareShaderCacheDirectory(temp / "new-cache" / "game"),
                  "cache directory can be prepared recursively");
            Check(Common::PrepareShaderCacheDirectory(temp / "new-cache" / "game"),
                  "existing cache directory remains usable");
            Write(temp / "blocked-cache", "file instead of a directory");
            Check(!Common::PrepareShaderCacheDirectory(temp / "blocked-cache" / "game"),
                  "blocked cache directory fails without throwing");
            Common::ShaderCacheLock owner, contender;
            Check(owner.Acquire(temp / "package"), "shader cache owner acquires lock");
            Check(!contender.Acquire(temp / "package"), "concurrent shader cache access refused");
            owner.Release();
            Check(contender.Acquire(temp / "package"), "shader cache available after owner exits");
            contender.Release();
            Check(!owner.Acquire(temp / "missing"), "unavailable cache fails safely");
            Check(owner.Acquire(temp / "package"), "retry owner acquires lock");
            std::jthread closing_owner([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds{75});
                owner.Release();
            });
            Check(contender.Acquire(temp / "package", std::chrono::seconds{2}),
                  "bounded retry acquires cache after previous owner finishes");
            closing_owner.join();
            contender.Release();
            Check(owner.Acquire(temp / "package"), "timeout owner acquires lock");
            const auto wait_start = std::chrono::steady_clock::now();
            Check(!contender.Acquire(temp / "package", std::chrono::milliseconds{100}),
                  "bounded retry refuses live owner");
            Check(std::chrono::steady_clock::now() - wait_start < std::chrono::seconds{2},
                  "busy cache retry remains bounded");
            Check(contender.LastError() == ERROR_SHARING_VIOLATION,
                  "cache reports actionable sharing error");
            owner.Release();
            const auto missing_start = std::chrono::steady_clock::now();
            Check(!owner.Acquire(temp / "missing", std::chrono::seconds{3}),
                  "missing cache fails even with retry enabled");
            Check(std::chrono::steady_clock::now() - missing_start < std::chrono::seconds{1},
                  "permanent cache error does not wait for contention timeout");

            const auto unicode = temp / fs::path(L"Jos\u00e9-\u65e5\u672c");
            fs::create_directories(unicode);
            SetEnvironmentVariableW(L"BBVR_TEST_UNICODE_PATH", unicode.c_str());
            const auto unicode_text = BenchmarkFiles::EnvironmentUtf8(L"BBVR_TEST_UNICODE_PATH");
            Check(Path(unicode_text) == unicode, "MockXR path environment preserves Unicode");
            const auto unicode_file = PathText(unicode / "input.txt");
            auto* unicode_out = BenchmarkFiles::Open(unicode_file, "wb");
            Check(unicode_out != nullptr, "MockXR writes Unicode paths");
            std::fputs("button_a 0.2", unicode_out); std::fclose(unicode_out);
            Check(Read(unicode / "input.txt") == "button_a 0.2", "Unicode output round trip");
            Check(BenchmarkFiles::Remove(unicode_file) == 0, "MockXR removes Unicode input files");
            SetEnvironmentVariableW(L"BBVR_TEST_UNICODE_PATH", nullptr);
            const auto history = temp / "scan-history";
            for (int i = 0; i < 5; ++i) {
                const auto folder = history / std::to_string(i);
                Write(folder / "scan-owner.json", Json{{"kind", "bbvr-perf-scan"}, {"complete", true}}.dump());
                fs::last_write_time(folder / "scan-owner.json", fs::file_time_type::clock::now() + std::chrono::seconds{i});
            }
            Write(history / "active/scan-owner.json", Json{{"kind", "bbvr-perf-scan"}, {"complete", false}}.dump());
            Write(history / "unknown/sentinel", "keep");
            PruneScanHistory(history, 3);
            Check(!fs::exists(history / "0") && !fs::exists(history / "1"), "old owned reports pruned");
            Check(fs::exists(history / "2") && fs::exists(history / "4"), "newest reports retained");
            Check(fs::exists(history / "active/scan-owner.json") && Read(history / "unknown/sentinel") == "keep",
                  "cleanup preserves active and unknown directories");

            Paths profile_paths{temp / "profile"};
            Profile saved;
            saved.refresh_hz = 120;
            saved.Save(profile_paths);
            Check(Profile::Load(profile_paths).refresh_hz == 120,
                  "recommended target survives profile reload");

            for(const int hz : {MinGameRefreshHz,90,120,MaxGameRefreshHz}) {
                saved.refresh_hz=hz; saved.Save(profile_paths);
                Check(Profile::Load(profile_paths).refresh_hz==hz,"supported cap survives reload");
                Check(Json::parse(LaunchConfig("",temp,hz))["GPU"]["vblank_frequency"]==hz &&
                      VR::Ini::ValueOf(LaunchIni("",StereoPreset::Native,hz),"refresh_hz")==std::to_string(hz),
                      "all selectable caps are launchable");
            }
            Check(!Profile::Load(profile_paths).follow_headset,"old fixed-rate profiles remain manual");
            saved.follow_headset=true; saved.refresh_hz=120; saved.Save(profile_paths);
            Check(Profile::Load(profile_paths).follow_headset && Profile::Load(profile_paths).refresh_hz==120,
                  "follow mode persists without losing manual cap");
            for(const float hz : {72.0f,80.0f,89.9f,120.0f,144.0f}) {
                const int resolved=GameRefreshRate(true,120,hz);
                Check(resolved==int(std::lround(hz)),"follow resolves active runtime rate");
                Check(VR::Ini::ValueOf(LaunchIni("refresh_hz=144",StereoPreset::Native,resolved,true),"refresh_hz")=="0" &&
                      Json::parse(LaunchConfig("",temp,resolved))["GPU"]["vblank_frequency"]==resolved,
                      "follow keeps display rate while capping gameplay at detected rate");
            }
            for(const float hz : {0.0f,29.0f,300.0f,std::nanf(""),std::numeric_limits<float>::infinity()})
                Check(GameRefreshRate(true,120,hz)==90,"unavailable or unsupported refresh falls back to 90");
            Check(GameRefreshRate(false,120,72)==120,"manual mode ignores detected display rate");
            const auto valid_profile=Read(profile_paths.Profile());
            for(const int hz : {0,MinGameRefreshHz-1,MaxGameRefreshHz+1,360}) {
                saved.refresh_hz=hz;
                Throws([&] { saved.Save(profile_paths); },"invalid cap cannot be saved");
                Check(Read(profile_paths.Profile())==valid_profile,"invalid cap preserves profile");
                Throws([&] { LaunchConfig("",temp,hz); },"invalid cap rejected by game config");
                Throws([&] { LaunchIni("",StereoPreset::Native,hz); },"invalid cap rejected by VR config");
            }

            Paths apply_paths{temp / "apply"};
            Profile original_profile;
            original_profile.follow_headset=true;
            original_profile.game = "untouched-game";
            original_profile.saves = "untouched-saves";
            original_profile.Save(apply_paths);
            const std::string original_ini =
                "custom_setting=keep\nrefresh_hz=90\ndibr_eye_taa=0\nupscale_focus=3\ndynamic_resolution=1\n";
            Write(apply_paths.Ini(), original_ini);
            auto focused = candidates[1];
            focused.upscale_focus = ScanFoveatedUpscale;
            const auto applied =
                ApplySettings(apply_paths, original_profile, focused, 120, original_ini);
            const auto applied_ini = Read(apply_paths.Ini());
            Check(applied.stereo == StereoChoice::Native && applied.refresh_hz == 120 && !applied.follow_headset &&
                      VR::Ini::ValueOf(applied_ini, "graphics_config") ==
                          GraphicsText(focused.graphics) &&
                      VR::Ini::ValueOf(applied_ini, "dlss_model") == "builtin" &&
                      VR::Ini::ValueOf(applied_ini, "refresh_hz") == "120" &&
                      !VR::Ini::ValueOf(applied_ini, "aa_mode"),
                  "apply persists measured DLSS and 120 Hz, and leaves the player's anti-aliasing");
            Check(VR::Ini::ValueOf(applied_ini, "upscale_focus") == "2" &&
                      ToJson(Summarize(focused, {}, 120)).at("upscale_focus") == 2,
                  "native recommendations record and apply the measured Medium preset");
            Check(applied.game == original_profile.game &&
                      applied.saves == original_profile.saves &&
                      VR::Ini::ValueOf(applied_ini, "custom_setting") == "keep" &&
                      VR::Ini::ValueOf(applied_ini, "dynamic_resolution") == "1" &&
                      VR::Ini::ValueOf(applied_ini, "dibr_eye_taa") == "0",
                  "native apply preserves unrelated settings and DIBR TAA");
            // The scan window (2026-11-25, the user: after Apply it only asked to apply again): a
            // suggestion is in use once the saved settings hold it, and its list names each setting
            // as the launcher does, with what it was before.
            Check(!SettingsInUse(original_profile, original_ini, focused, 120) &&
                      SettingsInUse(applied, applied_ini, focused, 120) &&
                      SettingsInUse(applied, applied_ini + "custom_other=1\n", focused, 120) &&
                      SettingsInUse(applied,
                                    VR::Ini::ApplyEdits(applied_ini, {{"aa_mode", "fxaa"},
                                                                      {"light_shadow_detail", "high"},
                                                                      {"dibr_eye_taa", "0.5"}}),
                                    focused, 120),
                  "a suggestion is in use once applied, whatever unrelated or picture lines say");
            for (const VR::Ini::Edit &change :
                 {VR::Ini::Edit{"upscale_focus", "1"}, VR::Ini::Edit{"dlss_model", "K"},
                  VR::Ini::Edit{"refresh_hz", "90"},
                  VR::Ini::Edit{"graphics_config", GraphicsText(candidates[0].graphics)}})
                Check(!SettingsInUse(applied, VR::Ini::ApplyEdits(applied_ini, {change}), focused, 120),
                      "a setting the scan set, changed again, is no longer in use");
            {
                auto following = applied;
                following.follow_headset = true;
                auto simplified = applied;
                simplified.stereo = StereoChoice::Dibr;
                Check(!SettingsInUse(following, applied_ini, focused, 120) &&
                          !SettingsInUse(simplified, applied_ini, focused, 120) &&
                          !SettingsInUse(applied, applied_ini, focused, 90),
                      "another stereo or frame-rate cap is no longer in use");
            }
            Check(SettingsInUse(applied,
                                VR::Ini::ApplyEdits(applied_ini, {{"upscale_focus", std::nullopt},
                                                                  {"dlss_model", std::nullopt},
                                                                  {"aa_mode", std::nullopt},
                                                                  {"refresh_hz", "120.0"}}),
                                focused, 120),
                  "lines the file lacks are the game's defaults, and numbers match as numbers");
            const auto row = [](const std::vector<AppliedSetting> &rows, std::string_view name) {
                for (const auto &r : rows)
                    if (r.name == name)
                        return r;
                return AppliedSetting{"", "absent", "absent"};
            };
            const auto listed = AppliedSettings(original_profile, original_ini, focused, 120);
            Check(row(listed, "Stereo rendering").value == "Native stereo" &&
                      row(listed, "Stereo rendering").before == "Auto" &&
                      row(listed, "Upscaler").value == "DLSS Quality" &&
                      row(listed, "Upscaler").before == "Off" &&
                      row(listed, "Output scale").value == "100%" &&
                      row(listed, "Output scale").before.empty() &&
                      row(listed, "Foveated upscale").value == "Medium" &&
                      row(listed, "Foveated upscale").before == "Performance" &&
                      row(listed, "Game frame-rate cap").value == "120 FPS" &&
                      row(listed, "Game frame-rate cap").before == "Follow headset",
                  "the applied list names each setting, its value, and what changed");
            Check(listed.size() == 5 && row(listed, "DLSS model").name.empty() &&
                      row(listed, "DLSS 5").name.empty(),
                  "an unpinned model and an unused DLSS 5 are not listed, nor any picture setting");
            bool unchanged = true;
            for (const auto &r : AppliedSettings(applied, applied_ini, focused, 120))
                unchanged &= r.before.empty();
            Check(unchanged, "settings that already hold the suggestion list nothing as changed");
            {
                auto dlss5 = focused.graphics;
                dlss5[5] = 1;
                const auto was = VR::Ini::ApplyEdits(applied_ini, {{"graphics_config", GraphicsText(dlss5)},
                                                                   {"dlss_model", "K"}});
                const auto rows = AppliedSettings(applied, was, focused, 120);
                Check(row(rows, "DLSS 5").value == "Off" && row(rows, "DLSS 5").before == "On" &&
                          row(rows, "Upscaler").before.empty() &&
                          row(rows, "DLSS model").value == "The preset's own" &&
                          row(rows, "DLSS model").before == "K",
                      "turning DLSS 5 off and a pinned model's replacement are listed");
                const auto native = AppliedSettings(applied, applied_ini, candidates[0], 120);
                Check(row(native, "Upscaler").value == "Off" &&
                          row(native, "Upscaler").before == "DLSS Quality" &&
                          row(native, "Anti-aliasing").name.empty(),
                      "without an upscaler, the player's own anti-aliasing stays theirs, unlisted");
            }
            const auto to_dibr = AppliedSettings(applied, applied_ini, candidates[7], 90);
            Check(row(to_dibr, "Stereo rendering").value == "Simplified Rendering" &&
                      row(to_dibr, "Stereo rendering").before == "Native stereo" &&
                      row(to_dibr, "Foveated upscale").name.empty() &&
                      row(to_dibr, "Eye TAA").name.empty() &&
                      row(to_dibr, "Game frame-rate cap").before == "120 FPS",
                  "a Simplified Rendering suggestion lists neither the native-only upscale nor eye TAA");
            const auto dibr = ApplySettings(apply_paths, applied, candidates[7], 90, applied_ini);
            Check(dibr.stereo == StereoChoice::Dibr && dibr.refresh_hz == 90 &&
                      VR::Ini::ValueOf(Read(apply_paths.Ini()), "dibr_eye_taa") == "0" &&
                      VR::Ini::ValueOf(Read(apply_paths.Ini()), "upscale_focus") == "2",
                  "DIBR apply keeps the player's eye TAA and the native-only preset");
            // A scan measures with the player's own picture settings (2026-11-25, the user: "for
            // smaller settings like shadows, ao, etc - we should use what the launcher already
            // has"): the Picture tab's, the launcher's Graphics rows' and the new graphics' without
            // a row; never what the scan sets or holds still, nor the player's other settings.
            {
                const std::string player =
                    "light_shadows=1\nlight_shadow_detail=high\nfog_banks=0\nmoon_shafts=1\n"
                    "ambient_occlusion=1\nbounce_light=0\nwater_reflections=1\nwet_streets=0\n"
                    "fog_light=0\nmoon_shafts_quality=0\naa_mode=fxaa\nlod_bias=-1\nfoveation=2\n"
                    "shadows_far=reduced\ncanted_eyes=0\ndibr_eye_taa=0\n"
                    "graphics_config=1,10,1,2,2,0,1,100,80,85\nupscale_focus=3\ndynamic_resolution=1\n"
                    "dlss_model=K\nrefresh_hz=120\nframe_cap=auto\ngame_fps=120\nframe_hooks=0\n"
                    "body_mode=no_head\nsnap_turn_deg=45\nleft_handed=1\nmove_with=right\n";
                const auto picture = PictureSettings(player);
                const auto has = [&](const char *key, const char *value) {
                    return std::find(picture.begin(), picture.end(),
                                     std::pair<std::string, std::string>{key, value}) != picture.end();
                };
                const auto mentions = [&](const char *key) {
                    return std::any_of(picture.begin(), picture.end(),
                                       [&](const auto &line) { return line.first == key; });
                };
                Check(has("light_shadows", "1") && has("light_shadow_detail", "high") &&
                          has("fog_banks", "0") && has("moon_shafts", "1") &&
                          has("ambient_occlusion", "1") && has("bounce_light", "0") &&
                          has("water_reflections", "1") && has("wet_streets", "0") &&
                          has("fog_light", "0") && has("moon_shafts_quality", "0") &&
                          has("aa_mode", "fxaa") && has("lod_bias", "-1") && has("foveation", "2") &&
                          has("shadows_far", "reduced") && has("canted_eyes", "0") &&
                          has("dibr_eye_taa", "0"),
                      "a scan carries the player's shadows, fog, AO, reflections, AA, detail and foveation");
                for (const char *key : {"graphics_config", "upscale_focus", "dynamic_resolution",
                                        "dlss_model", "refresh_hz", "frame_cap", "game_fps",
                                        "frame_hooks", "body_mode", "snap_turn_deg", "left_handed",
                                        "move_with"})
                    Check(!mentions(key), "never what the scan sets or holds still, nor play settings");
                Check(PictureSettings("").empty() && PictureSettings("custom_setting=keep\n").empty(),
                      "a picture line the file lacks stays the game's default");
            }
            const auto before_conflict = Read(apply_paths.Profile());
            const auto actual_ini = Read(apply_paths.Ini());
            Throws([&] { ApplySettings(apply_paths, dibr, candidates[0], 120, original_ini); },
                   "apply refuses stale settings");
            Check(Read(apply_paths.Profile()) == before_conflict &&
                      Read(apply_paths.Ini()) == actual_ini,
                  "stale settings roll back the profile without changing the INI");
            for (const int provider : {1, 2, 3}) {
                for (const int preset : {0, 1, 2}) {
                    auto choice = candidates[1];
                    choice.graphics[1] = -20;
                    choice.graphics[2] = provider;
                    choice.graphics[provider == 1 ? 3 : 4] = preset;
                    const auto before = Read(apply_paths.Ini());
                    const auto selected = ApplySettings(apply_paths, Profile::Load(apply_paths),
                                                        choice, 90, before);
                    const auto launched = LaunchIni(Read(apply_paths.Ini()), choice.stereo,
                                                    selected.refresh_hz);
                    Check(ParseGraphics(VR::Ini::ValueOf(launched, "graphics_config")) == choice.graphics,
                          "normal launch retains applied provider, preset and output scale");
                    Check(VR::Ini::ValueOf(launched, "upscale_focus") == "0",
                          "a native recommendation from an older report applies its measured Off preset");
                    Check(VR::Ini::ValueOf(launched, "frame_cap") == "auto" &&
                              VR::Ini::ValueOf(launched, "refresh_hz") == "90",
                          "normal launch restores cap instead of benchmark uncapping");
                }
            }
            Write(temp / "package/manifest.json",
                  R"({"schema":1,"files":[{"path":"../outside","sha256":"bad"}]})");
            Throws([&] { CopyFixture(temp / "package", temp / "copy"); },
                   "fixture traversal rejected");
            Write(
                temp / "package/manifest.json",
                R"({"schema":1,"files":[{"path":"1000/savedata/CUSA00207/SPRJ0005/userdata0000","sha256":"bad"}]})");
            Throws([&] { CopyFixture(temp / "package", temp / "copy"); },
                   "missing/hash-invalid fixture rejected");
            const auto valid_package = temp / "valid";
            const std::string savepath = "1000/savedata/CUSA00207/SPRJ0005/userdata0000";
            Write(valid_package / "home" / savepath, "fixture-test");
            Json manifest{
                {"schema", 1},
                {"files",
                 Json::array({{{"path", savepath},
                               {"sha256", FileHash(valid_package / "home" / savepath)}}})}};
            Write(valid_package / "manifest.json", manifest.dump());
            CopyFixture(valid_package, temp / "valid-copy");
            Check(Read(temp / "valid-copy" / savepath) == "fixture-test",
                  "valid fixture copied to a fresh isolated home");
            Throws([&] { CopyFixture(valid_package, temp / "valid-copy"); },
                   "existing destination cannot be overwritten");
            // The save goes into the folder the selected game reads (SaveDataFolder): the fixture
            // was made under CUSA00207; CUSA00900 reads CUSA00900.
            CopyFixture(valid_package, temp / "us-copy", "CUSA00900");
            Check(Read(temp / "us-copy" / "1000/savedata/CUSA00900/SPRJ0005/userdata0000") ==
                          "fixture-test" &&
                      !fs::exists(temp / "us-copy" / savepath),
                  "fixture copied into the selected game's save folder");
            Throws([&] { CopyFixture(valid_package, temp / "bad-copy", "../evil00"); },
                   "a save folder that is not a title id is refused");
            Throws([&] { CopyFixture(valid_package, temp / "bad-copy", "CUSA"); },
                   "a short save folder is refused");
            Check(!fs::exists(temp / "bad-copy"), "a refused copy writes nothing");
        }
        std::cout << checks << " benchmark checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
