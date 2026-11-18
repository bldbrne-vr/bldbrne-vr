// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "benchmark.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <pugixml.hpp>
#include <regex>
#include <sstream>

namespace Launcher::Benchmark {
// The searches' preference order, not a measured image-quality score. Output size first; one
// upscaler preset step (about a quarter fewer pixels rendered) weighs as much as 10 points of
// output, so Quality at 90% outranks Performance at 100% and native 100% outranks Balanced;
// DLSS, then FSR 3.1.5, then FSR 4; native stereo over DIBR.
int Rank(StereoPreset stereo, int output, int provider, int preset) {
    return output * 10 + (stereo == StereoPreset::Native ? 100 : -100) +
           (provider ? (preset - 2) * 100 + 20 + (provider == 1 ? 10 : provider == 3 ? 0 : -40)
                     : 0);
}
PacingStats EvaluatePacing(const Sample &sample, double target_hz) {
    PacingStats out;
    if (!std::isfinite(target_hz) || target_hz <= 0)
        return out;
    const double budget = 1000.0 / target_hz;
    if (sample.frame_times.empty()) {
        // Compatibility for synthetic/legacy samples without ordered intervals.
        out.acceptable = sample.frame_p99 > 0 && sample.frame_p99 <= budget;
        return out;
    }
    double total = 0, slow_time = 0, run = 0;
    size_t slow = 0;
    for (double ms : sample.frame_times) {
        if (!std::isfinite(ms) || ms <= 0)
            return out;
        total += ms;
        if (ms > budget) {
            ++slow;
            slow_time += ms;
            run += ms;
            out.longest_slow_ms = std::max(out.longest_slow_ms, run);
        } else {
            run = 0;
        }
    }
    out.slow_frame_fraction = double(slow) / sample.frame_times.size();
    out.slow_time_fraction = slow_time / total;
    // Tolerate brief isolated dips, not recurring misses or a perceptible long burst.
    out.acceptable = out.slow_frame_fraction <= .05 && out.slow_time_fraction <= .05 &&
                     out.longest_slow_ms <= 100;
    return out;
}
int TargetRate(int requested, float headset_hz) {
    return requested                               ? requested
           : headset_hz >= MinGameRefreshHz && headset_hz <= MaxGameRefreshHz ? int(headset_hz + .5f)
                                                   : 90;
}
// Each five-second cycle returns the stick to rest and includes one physical attack.
// Repeat the same work across warm-up and measured windows; every run restores the save.
std::string ScanInput() {
    std::string script;
    for(int i=0;i<24;++i)
        script += "move 0.75 0 0.6\nmove 0.75 0 -0.6\nrswing_fore 2.75 6 0 0\n";
    return script;
}
bool ScanActivityObserved(const std::string& log) {
    const auto range=SelectMeasurementRange(log);
    if(!range) return false;
    const auto measured=log.substr(range->begin,range->end-range->begin);
    const std::regex attack(R"(VR melee: #\d+ [^\r\n]* open \([^\r\n]* ok=1\b)");
    return measured.find("class locomotion")!=std::string::npos &&
        std::distance(std::sregex_iterator(measured.begin(),measured.end(),attack),
                      std::sregex_iterator())>=3;
}
// A conservative search seed, not a prediction of FPS or GPU architecture.
int InitialScale(const SystemInfo &system, const Headset &headset, int target) {
    const double pixels=double(std::max(headset.width[0],headset.width[1])) *
                        std::max(headset.height[0],headset.height[1]);
    if(pixels<=0) return 100;
    const double gib=double(system.vram)/(1ull<<30);
    const double budget=(gib<=8 ? 2.0e6 : gib<=12 ? 3.0e6 : 4.0e6) *
                        std::min(1.0,90.0/std::max(30,target));
    return std::clamp(int(std::floor(std::sqrt(budget/pixels)*10))*10,50,100);
}
std::vector<Candidate> Candidates(const SystemInfo &system, bool dlss, bool fsr4, bool fsr31) {
    std::vector<Candidate> out;
    auto add = [&](std::string id, std::string label, StereoPreset stereo, int scale, int provider,
                   int preset, int quality, std::string model = "builtin") {
        auto g = DefaultGraphics;
        g[1] = scale;
        g[2] = provider;
        g[3] = g[4] = preset;
        out.push_back({id, label, stereo, g, model, quality});
    };
    add("native-100", "Native stereo, full resolution", StereoPreset::Native, 0, 0, 2, 100);
    if (dlss && system.nvidia) {
        add("dlss-quality", "Native + DLSS Quality", StereoPreset::Native, 0, 1, 2, 90, "K");
        add("dlss-performance", "Native + DLSS Performance", StereoPreset::Native, 0, 1, 0, 70,
            "K");
    }
    if (fsr4) {
        add("fsr-quality", "Native + FSR 4 Quality", StereoPreset::Native, 0, 2, 2, 90);
        add("fsr-performance", "Native + FSR 4 Performance", StereoPreset::Native, 0, 2, 0, 70);
    }
    if (fsr31) {
        add("fsr31-quality", "Native + FSR 3.1.5 Quality", StereoPreset::Native, 0, 3, 2, 90);
        add("fsr31-performance", "Native + FSR 3.1.5 Performance", StereoPreset::Native, 0, 3, 0, 70);
    }
    add("native-80", "Native stereo, 80% resolution", StereoPreset::Native, -20, 0, 2, 60);
    add("native-60", "Native stereo, 60% resolution", StereoPreset::Native, -40, 0, 2, 40);
    add("dibr-100", "Simplified Rendering, full resolution", StereoPreset::Dibr, 0, 0, 2, 55);
    add("dibr-80", "Simplified Rendering, 80% resolution", StereoPreset::Dibr, -20, 0, 2, 35);
    if (StartupStereo(system) == StereoPreset::Dibr)
        std::rotate(out.begin(), out.end() - 2, out.end());
    // A card without native stereo's multiview (a gray right eye) is never measured, recommended
    // or applied in native stereo: its frame rate there says nothing about a picture that works.
    if (system.multiview_limited)
        std::erase_if(out, [](const Candidate &c) { return c.stereo == StereoPreset::Native; });
    return out;
}
// Long scan explores each supported provider/preset independently. A native
// baseline failure must not prevent testing an upscaler which could make it viable.
static std::optional<Candidate> NextLongCandidate(const SystemInfo& system, bool dlss,
    bool fsr4, const std::vector<Result>& results, int target, bool fsr31, const Headset& headset) {
    if(target<30 || target>240) return std::nullopt;
    const int seed=InitialScale(system,headset,target);
    for(int preset:{2,1,0}) for(int provider:{1,3,2,0,-1}) {
        if((provider==1 && !(dlss && system.nvidia)) || (provider==3 && !fsr31) ||
           (provider==2 && !fsr4) || (provider<=0 && preset!=2)) continue;
        if(system.multiview_limited && provider>=0) continue; // native stereo: DIBR only
        int output=seed;
        for(int step=0;step<6;++step) {
            const bool native=provider>=0;
            const std::string name=provider==1?"DLSS":provider==3?"FSR 3.1.5":provider==2?"FSR 4":native?"Native base":StereoLabel(StereoPreset::Dibr);
            Candidate c;
            c.stereo=native?StereoPreset::Native:StereoPreset::Dibr;
            c.graphics[1]=output-100;c.graphics[2]=std::max(0,provider);
            c.graphics[3]=c.graphics[4]=preset;c.model=provider==1?PresetDlssModel(preset):"builtin";
            c.id=provider>0?"upscale-"+std::to_string(provider)+"-"+std::to_string(preset)+"-"+std::to_string(output)
                           :std::string(native?"base-native-":"base-dibr-")+std::to_string(output);
            c.label=name+(provider>0?(preset==2?" Quality":preset==1?" Balanced":" Performance"):"")+", "+std::to_string(output)+"% output";
            c.quality=Rank(c.stereo,output,std::max(0,provider),preset);
            const auto it=std::find_if(results.begin(),results.end(),[&](const auto& r){return r.candidate.id==c.id;});
            if(it==results.end()) return c;
            if(!it->reliable) break; // Bounded retries belong to the runner; move on afterward.
            int next=output;
            if(it->meets_target) {
                if(output==100 || it->fps < target*ExploreReserve) break;
                const double gain=std::min(it->fps/(target*PredictedReserve),1000.0/(target*PredictedReserve*std::max(.1,it->gpu_ms)));
                next=std::min(100,int(std::floor(output*std::sqrt(gain)/10))*10);
                if(next<=output) next=std::min(100,int(std::floor(output*std::sqrt(gain)/5))*5);
                if(next<=output) break; // No supported increase preserves the predicted reserve.
            }
            else {
                if(output==50) break;
                const double gain=std::min(it->fps/(target*PredictedReserve),1000.0/(target*PredictedReserve*std::max(.1,it->gpu_ms)));
                next=std::clamp(int(std::floor(output*std::sqrt(std::max(.01,gain))/10))*10,std::max(50,output-20),output-10);
            }
            // Stop at a measured boundary rather than oscillate between sizes.
            const auto seen=std::any_of(results.begin(),results.end(),[&](const auto& r){
                return r.candidate.stereo==c.stereo && r.candidate.graphics[2]==c.graphics[2] &&
                    r.candidate.graphics[provider==1?3:4]==preset && r.candidate.graphics[1]==next-100;
            });
            if(seen) break;
            output=next;
        }
    }
    return std::nullopt;
}

// Find a comfortable base first, then spend its remaining budget on reconstruction.
// The launcher currently couples input/output through provider presets, so every
// trial is a real supported configuration, not an invented independent input size.
std::optional<Candidate> NextCandidate(const SystemInfo &system, bool dlss, bool fsr4,
                                       const std::vector<Result> &results, int target, bool fsr31, const Headset *headset) {
    if(headset)
        return NextLongCandidate(system,dlss,fsr4,results,target,fsr31,*headset);
    auto find = [&](const std::string &id) -> const Result * {
        const auto it = std::find_if(results.begin(), results.end(),
                                     [&](const auto &r) { return r.candidate.id == id; });
        return it == results.end() ? nullptr : &*it;
    };
    auto make = [](std::string id, std::string label, StereoPreset stereo, int percent,
                   int provider, int preset, int quality) {
        auto g = DefaultGraphics;
        g[1] = percent - 100;
        g[2] = provider;
        g[3] = g[4] = preset;
        return Candidate{
            id, label, stereo, g, provider == 1 ? PresetDlssModel(preset) : "builtin", quality};
    };
    int native_base = 0;
    auto first = StartupStereo(system);
    // A severe, repeatable native shortfall warrants testing DIBR before
    // spending more runs reducing native resolution. This changes test order,
    // not the recommendation gates or the player's active rendering mode.
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
        if (it->candidate.stereo != StereoPreset::Native ||
            !it->candidate.id.starts_with("base-native-") || !it->reliable ||
            it->fps >= target * (2.0 / 3.0))
            continue;
        first = StereoPreset::Dibr;
        const int percent = 100 + it->candidate.graphics[1];
        const auto id = "base-dibr-" + std::to_string(percent);
        if (!find(id))
            return make(id, "Simplified Rendering, " + std::to_string(percent) + "%", StereoPreset::Dibr,
                        percent, 0, 2, percent * 10 - 100);
        break;
    }
    for (auto stereo :
         {first, first == StereoPreset::Native ? StereoPreset::Dibr : StereoPreset::Native}) {
        if (system.multiview_limited && stereo == StereoPreset::Native)
            continue; // no native base: the reconstruction candidates below are native too
        const std::string mode = stereo == StereoPreset::Native ? "native" : "dibr";
        for (int percent = headset ? InitialScale(system,*headset,target) : 100; percent >= 50; percent -= 10) {
            const auto c = make("base-" + mode + "-" + std::to_string(percent),
                                (stereo == StereoPreset::Native ? "Native base, " : "Simplified Rendering, ") +
                                    std::to_string(percent) + "%",
                                stereo, percent, 0, 2, Rank(stereo, percent, 0, 2));
            const auto *result = find(c.id);
            if (!result)
                return c;
            bool comfortable =
                result->reliable && result->meets_target && result->fps >= target * 1.2;
            for (const auto &sample : result->repeats)
                if (sample.valid)
                    comfortable &= sample.worst_window_gpu_p99 <= 1000.0 / (target * 1.2) &&
                                   sample.frame_p99 <= 1000.0 / (target * 1.1);
            if (comfortable) {
                if (stereo == StereoPreset::Native)
                    native_base = percent;
                break;
            }
        }
    }
    // DIBR remains a measured fallback. Temporal DIBR candidates need their own
    // completion evidence before being admitted to automatic recommendations.
    if (!native_base)
        return std::nullopt;
    for (int provider : {1, 3, 2}) {
        if (provider == 1 && !(dlss && system.nvidia))
            continue;
        if (provider == 2 && !fsr4)
            continue;
        if (provider == 3 && !fsr31)
            continue;
        for (int preset : {2, 1, 0}) {
            const double ratio = preset == 2 ? 2.0 / 3.0 : preset == 1 ? .58 : .5;
            const int top = std::min(100, int(std::floor(native_base / ratio / 10)) * 10);
            for (int percent = top; percent >= std::max(50, native_base); percent -= 10) {
                const std::string name = provider == 1 ? "DLSS" : provider == 3 ? "FSR 3.1.5" : "FSR 4";
                const std::string mode = preset == 2   ? "Quality"
                                         : preset == 1 ? "Balanced"
                                                       : "Performance";
                const auto c = make("upscale-" + std::to_string(provider) + "-" +
                                        std::to_string(preset) + "-" + std::to_string(percent),
                                    name + " " + mode + ", " + std::to_string(percent) + "% output",
                                    StereoPreset::Native, percent, provider, preset,
                                    Rank(StereoPreset::Native, percent, provider, preset));
                const auto *result = find(c.id);
                if (!result)
                    return c;
                if (result->reliable && result->meets_target)
                    break;
            }
        }
    }
    return std::nullopt;
}
std::optional<Candidate> NextQuickCandidate(const SystemInfo &system, bool dlss, bool fsr4,
                                           const std::vector<Result> &results, int target, bool allow_screening, bool fsr31, const Headset *headset) {
    if (target < 30 || target > 240) return std::nullopt;
    size_t attempts = 0;
    for (const auto& r : results) attempts += r.repeats.size();
    if (attempts >= QuickScanMaxRuns) return std::nullopt;
    (void)allow_screening; // Adaptive steps are bounded by the runner's hard time limit.
    auto make = [](StereoPreset stereo, int output, int provider, int preset) {
        Candidate c;
        c.stereo=stereo; c.graphics[1]=output-100; c.graphics[2]=provider;
        c.graphics[3]=c.graphics[4]=preset;
        c.model=provider==1 ? PresetDlssModel(preset) : "builtin";
        const std::string mode=stereo==StereoPreset::Native ? "native" : "dibr";
        const std::string name=provider==1 ? "DLSS" : provider==3 ? "FSR 3.1.5" : "FSR 4";
        const std::string quality=preset==2 ? "Quality" : preset==1 ? "Balanced" : "Performance";
        c.id=provider ? "upscale-"+std::to_string(provider)+"-"+std::to_string(preset)+"-"+std::to_string(output)
                      : "base-"+mode+"-"+std::to_string(output);
        c.label=provider ? name+" "+quality+", "+std::to_string(output)+"% output"
                         : std::string(StereoLabel(stereo))+", "+std::to_string(output)+"% output";
        c.quality=Rank(stereo,output,provider,preset); // preference, not measured quality
        return c;
    };
    // Later candidates keep the first one's stereo mode: DIBR from the start when native stereo's
    // multiview is missing or the card starts in DIBR (an AMD RX 400, RX 500 or RX 5000).
    const int preferred=system.multiview_limited || system.auto_dibr ? 0 : dlss && system.nvidia ? 1 : fsr31 ? 3 : fsr4 ? 2 : 0;
    if(results.empty()) return make(preferred ? StereoPreset::Native : StartupStereo(system),
        headset ? InitialScale(system,*headset,target) : 100,preferred,2);
    auto usable=[](const Result& r)->const Sample* {
        // A noisy confirmation can justify a cheaper trial, never a recommendation
        // or a quality increase. Keep its slower measurement available for recovery.
        const Sample* slowest=nullptr;
        for(const auto& s:r.repeats) if(s.valid && (!slowest || s.fps<slowest->fps)) slowest=&s;
        return slowest;
    };
    auto passes=[&](const Sample& x) {
        return x.fps>=target*.92 && x.gpu_ms<=1000.0/(target*.92) &&
               x.memory_pressure<=.9 && EvaluatePacing(x,target*.92).acceptable;
    };
    auto untested=[&](const Candidate& c) {
        return std::none_of(results.begin(),results.end(),[&](const auto& r){return r.candidate.id==c.id;});
    };
    auto headroom=[&](const Sample& x) {
        return x.fps>=target*RequiredReserve && x.gpu_ms<=1000.0/(target*RequiredReserve) &&
               EvaluatePacing(x,target).acceptable && x.memory_pressure<.85;
    };
    // Aim slightly above the acceptance boundary when predicting the next setting.
    auto short_by=[&](const Sample& x) {
        return std::max(target*PredictedReserve/std::max(1.0,x.fps),x.gpu_ms*target*PredictedReserve/1000.0);
    };
    // A choice with headroom beats one that only passes the 8% tolerance, whatever its rank.
    const Result* best=nullptr;
    bool best_headroom=false;
    for(const auto& r:results) if(const auto* x=usable(r);x && (r.reliable || r.provisional) &&
                               (r.meets_target || r.near_target) && passes(*x)) {
        const bool room=r.meets_target;
        if(!best || room>best_headroom || (room==best_headroom && r.candidate.quality>best->candidate.quality)) {
            best=&r;
            best_headroom=room;
        }
    }
    auto confirm=[&]()->std::optional<Candidate> {
        if(best && !best->reliable && best->repeats.size()<3) return best->candidate;
        return std::nullopt;
    };
    // Up from a passing choice: coarse output steps, with a 5-point refinement when
    // a larger step would spend the reserve, or the next preset. Measure every proposal.
    auto step_up=[&](const Result& r)->std::optional<Candidate> {
        const auto* s=usable(r);
        if(!s || !r.meets_target || !headroom(*s) || s->fps<target*ExploreReserve) return std::nullopt;
        const auto& rc=r.candidate;
        const int out=100+rc.graphics[1], prov=rc.graphics[2];
        const int pre=prov==1 ? rc.graphics[3] : rc.graphics[4];
        if(out<100) {
            // Rendered pixels follow the square of scale. Round down: never force a
            // 10-point step when the estimate says it would spend the required reserve.
            const double gain=std::min(r.fps/(target*PredictedReserve),1000.0/(target*PredictedReserve*r.gpu_ms));
            int raised=std::min(100,int(std::floor(out*std::sqrt(gain)/10))*10);
            if(raised<=out) raised=std::min(100,int(std::floor(out*std::sqrt(gain)/5))*5);
            if(raised>out) {
                auto next=make(rc.stereo,raised,prov,pre);
                if(untested(next)) return next;
            }
        }
        if(prov && pre<2) {
            const double ratios[]{.5,.58,2.0/3.0};
            const double cost=std::pow(ratios[pre+1]/ratios[pre],2);
            if(r.fps/cost>=target*PredictedReserve && r.gpu_ms*cost<=1000.0/(target*PredictedReserve)) {
                auto next=make(rc.stereo,out,prov,pre+1);
                if(untested(next)) return next;
            }
        }
        return std::nullopt;
    };
    // A confirmed choice finishes the scan. Only the final choice is confirmed (its second run):
    // the climb below uses one run per step.
    if(const auto chosen=Recommend(results);chosen && results[*chosen].meets_target) return std::nullopt;
    const auto& last=results.back();
    const auto* x=usable(last);
    // Climb while the last step passes with headroom, one run per step; the screening budget
    // does not stop a climb (the runner gives it the time up to its hard limit).
    if(x && headroom(*x) && attempts<QuickScanMaxRuns-1)
        if(const auto up=step_up(last)) return up;
    // A jump that overshot (a higher step failed above a passing one): one probe in the middle,
    // then the best passing choice is confirmed.
    if(best_headroom && x && !headroom(*x) && attempts<QuickScanMaxRuns-2) {
        const auto& b=best->candidate;
        const int low=100+b.graphics[1], high=100+last.candidate.graphics[1];
        const int b_preset=b.graphics[2]==1 ? b.graphics[3] : b.graphics[4];
        const int l_preset=last.candidate.graphics[2]==1 ? last.candidate.graphics[3] : last.candidate.graphics[4];
        if(b.stereo==last.candidate.stereo && b.graphics[2]==last.candidate.graphics[2] &&
           b_preset==l_preset && high-low>=20) {
            auto middle=make(b.stereo,int(std::lround((low+high)/20.0))*10,b.graphics[2],b_preset);
            if(untested(middle)) return middle;
        }
    }
    // Reserve the final run for confirmation. Expiring the optional screening budget must
    // not strand a slow first result: keep descending until we find headroom or hit the
    // run/time limits. The runner reserves confirmation time within the hard limit.
    if(attempts>=QuickScanMaxRuns-1) return confirm();
    if(!x) {
        if(last.repeats.size()<2) return last.candidate;
        return confirm();
    }
    const auto& c=last.candidate;
    const int output=100+c.graphics[1], provider=c.graphics[2];
    const int preset=provider==1 ? c.graphics[3] : c.graphics[4];
    // A failed climb returns to the comfortable fallback instead of spending its reserve.
    if(best_headroom && !headroom(*x)) return confirm();
    // Close to the target: one step down, a preset (the upscaler renders about a quarter fewer
    // pixels) or, without one, 10 points of output.
    auto one_step=[&]()->std::optional<Candidate> {
        if(provider && preset>0 && x->memory_pressure<.85) return make(c.stereo,output,provider,preset-1);
        if(output>50) return make(c.stereo,output-10,provider,preset);
        return std::nullopt;
    };
    if(last.meets_target) return confirm();
    // Low GPU time is a hint. Lack of a measured throughput gain after a substantial
    // pixel reduction is stronger evidence that further resolution cuts will not help.
    bool ineffective=false;
    const auto internal_ratio=[](int prov,int pre) {
        return prov ? (pre==2?2.0/3.0:pre==1?.58:.5) : 1.0;
    };
    for(const auto& previous:results) {
        const auto* p=usable(previous);
        const auto& pc=previous.candidate;
        if(p && pc.stereo==c.stereo && pc.graphics[2]==provider &&
           std::pow(output*internal_ratio(provider,preset)/
                    ((100+pc.graphics[1])*internal_ratio(provider,pc.graphics[provider==1?3:4])),2)<=.8 &&
           x->fps>=p->fps*.95 && x->fps<=p->fps*1.05 && x->memory_pressure<.85) ineffective=true;
    }
    const double frame_ms=1000.0/std::max(1.0,x->fps);
    const bool cpu_limited=x->gpu_ms<frame_ms*.65 || ineffective;
    if(passes(*x) && !cpu_limited) {
            // Keep reducing cost until the reserve is measured, retaining the limited fallback.
            if(const auto next=one_step();next && untested(*next)) return next;
            // At the floor, continue to a measured stereo fallback below.
    }
    // A limited fallback must not stop the search for the required reserve.
    const double shortfall=short_by(*x);
    const int predicted=int(std::floor(output/std::sqrt(shortfall)/10))*10;
    if(!cpu_limited && shortfall<=1.3) {
        // Close (within 30%): one step down.
        if(const auto next=one_step();next && untested(*next)) return next;
    } else if(!cpu_limited && predicted>=50 && output>50) {
        // Far: straight to the output size the shortfall predicts (the rendered pixels follow
        // its square), keeping the preset: one preset step could not close the gap.
        auto next=make(c.stereo,std::min(predicted,output-10),provider,preset);
        if(untested(next)) return next;
    }
    // Exhaust useful native pixel reductions before switching stereo mode. At the
    // output floor an upscaler's cheaper internal preset can still provide a large gain.
    if(!cpu_limited && provider && preset>0 && (output==50 || predicted<50)) {
        auto next=make(c.stereo,50,provider,preset-1);
        if(untested(next)) return next;
    }
    // Fewer pixels cannot help a CPU-bound frame, nor close a gap even half the output would
    // not: DIBR renders one view.
    if(c.stereo==StereoPreset::Native && (cpu_limited || predicted<50)) {
        // DIBR removes the temporal upscaler too. Account for that extra pixel cost
        // instead of blindly keeping an output that becomes GPU-bound in the new mode.
        const double ratio=internal_ratio(provider,preset);
        const double estimated_gpu=x->gpu_ms*.65/(ratio*ratio);
        const int dibr_output=std::clamp(int(std::floor(output*std::sqrt(
            1000.0/(target*PredictedReserve*estimated_gpu))/10))*10,50,output);
        auto next=make(StereoPreset::Dibr,dibr_output,0,2);
        if(untested(next)) return next;
    }
    if(output>50 && !(ineffective && c.stereo==StereoPreset::Dibr)) {
        auto next=make(c.stereo,output-10,provider,preset);
        if(untested(next)) return next;
    }
    if(c.stereo==StereoPreset::Native) {
        for(int alternative:{3,1,2}) {
            if(alternative==provider || (alternative==1 && !(dlss && system.nvidia)) ||
               (alternative==3 && !fsr31) || (alternative==2 && !fsr4)) continue;
            auto next=make(c.stereo,output,alternative,2);
            if(untested(next)) return next;
        }
        auto next=make(StereoPreset::Dibr,output,0,2);
        if(untested(next)) return next;
    } else if(preferred) {
        // A CPU-limit hint is not proof. If DIBR fails, still measure the cheapest
        // supported native option before settling for a limited-headroom fallback.
        auto next=make(StereoPreset::Native,50,preferred,0);
        if(untested(next)) return next;
    }
    return confirm();
}
std::optional<MeasurementRange> SelectMeasurementRange(const std::string &log) {
    const auto projection = log.find("VR projection: ");
    if (projection == std::string::npos)
        return std::nullopt;
    // Timed windows (the runtime says so once): ten seconds of warm-up whatever the frame rate,
    // and at least five windows so the 5-second throughput lines fall three times inside them.
    const bool timed = log.find("VR gpu frame: windows of ") != std::string::npos;
    const size_t discard = timed ? 2 : 4, minimum = timed ? 5 : 4;
    std::vector<size_t> offsets;
    constexpr std::string_view prefix = "VR gpu frame: ";
    for (size_t pos = projection; (pos = log.find(prefix, pos)) != std::string::npos; ++pos) {
        size_t end = pos + prefix.size();
        while (end < log.size() && std::isdigit(static_cast<unsigned char>(log[end])))
            ++end;
        if (end > pos + prefix.size() && log.compare(end, 13, " game frames:") == 0)
            offsets.push_back(pos);
    }
    // Warm-up is measured after gameplay projection begins, never in fast menus.
    if (offsets.size() < discard + minimum)
        return std::nullopt;
    const auto count = std::min<size_t>(12, offsets.size() - discard);
    return MeasurementRange{offsets[offsets.size() - count], offsets.back(), unsigned(count)};
}
Sample Analyze(const std::string &log, const Headset &headset, const Candidate &c,
               double background_cores) {
    Sample s;
    s.background_cores = background_cores;
    auto reject = [&](const char *reason) {
        s.reason = reason;
        return s;
    };
    for (auto marker : {"VK_ERROR_DEVICE_LOST", "Unhandled Exception", "SYNC-HAZARD", "VUID-",
                        "out of memory", "Benchmark fatal"})
        if (log.find(marker) != std::string::npos)
            return reject("Runtime or GPU error during measurement.");
    std::smatch match;
    const std::regex resolution(
        R"(VR resolution: headset L (\d+)x(\d+) R (\d+)x(\d+), scale ([+-]\d+)%, scene/post/output (\d+)x(\d+))");
    if (!std::regex_search(log, match, resolution))
        return reject("Missing headset resolution evidence.");
    // The controlled runtime uses the larger eye extent for both eyes.
    const auto width = std::max(headset.width[0], headset.width[1]);
    const auto height = std::max(headset.height[0], headset.height[1]);
    if (std::stoul(match[1]) != width || std::stoul(match[3]) != width ||
        std::stoul(match[2]) != height || std::stoul(match[4]) != height ||
        std::stoi(match[5]) != c.graphics[1])
        return reject("Measured headset size or scaling differs from the request.");
    const unsigned output_w =
        std::min(unsigned(std::lround(width * (1 + c.graphics[1] / 100.0) / 8) * 8),
                 headset.max_width / 8 * 8);
    const unsigned output_h =
        std::min(unsigned(std::lround(height * (1 + c.graphics[1] / 100.0) / 8) * 8),
                 headset.max_height / 8 * 8);
    const int preset = c.graphics[c.graphics[2] == 1 ? 3 : 4];
    const float ratio = c.graphics[2] ? (preset == 0   ? .5f
                                         : preset == 1 ? .58f
                                                              : 2.f / 3.f)
                                      : 1.f;
    const unsigned input_w =
        std::min(output_w, std::max(64u, unsigned(std::lround(output_w * double(ratio) / 8)) * 8));
    const unsigned input_h =
        std::min(output_h, std::max(64u, unsigned(std::lround(output_h * double(ratio) / 8)) * 8));
    s.input_width = input_w;
    s.input_height = input_h;
    s.output_width = output_w;
    s.output_height = output_h;
    if (std::stoul(match[6]) != input_w || std::stoul(match[7]) != input_h)
        return reject("Game input resolution differs from the requested configuration.");
    if (log.find("image " + std::to_string(input_w) + "x" + std::to_string(input_h) + ",") ==
        std::string::npos)
        return reject("No in-game projection at the expected resolution; "
                      "menu/loading samples rejected.");
    if (c.graphics[2]) {
        const std::regex temporal(
            R"(VR resolution: S1 \(FAST bit 51\) whole frame (\d+)x(\d+) per eye, reconstructed by (DLSS|FSR 4|FSR 3\.1\.5) preset (\d+).*? to (\d+)x(\d+) after the composite)");
        if (!std::regex_search(log, match, temporal) || std::stoul(match[1]) != input_w ||
            std::stoul(match[2]) != input_h ||
            match[3] != (c.graphics[2] == 1 ? "DLSS" : c.graphics[2] == 3 ? "FSR 3.1.5" : "FSR 4") ||
            std::stoi(match[4]) != preset || std::stoul(match[5]) != output_w ||
            std::stoul(match[6]) != output_h)
            return reject("Requested upscaler was not confirmed active at the "
                          "expected dimensions.");
        const std::regex resolved(
            R"(VR temporal native: frame \d+ both eyes recorded (\d+)x(\d+),.*?raster (\d+)x(\d+) -> (\d+)x(\d+),)");
        bool found = false;
        for (auto it = std::sregex_iterator(log.begin(), log.end(), resolved);
             it != std::sregex_iterator(); ++it) {
            const auto &m = *it;
            found |= std::stoul(m[1]) == output_w && std::stoul(m[2]) == output_h &&
                     std::stoul(m[3]) == input_w && std::stoul(m[4]) == input_h &&
                     std::stoul(m[5]) == output_w && std::stoul(m[6]) == output_h;
        }
        if (!found)
            return reject("No completed stereo reconstruction from the selected upscaler.");
    }
    const std::regex gpu(
        R"(VR gpu frame: \d+ game frames: GPU busy per frame avg ([\d.]+) / p50 ([\d.]+) / p99 ([\d.]+))");
    const std::regex fps(R"(VR perf: game frames ([\d.]+)/s, XR frames ([\d.]+)/s)");
    struct Window {
        size_t pos;
        double gpu, p99;
    };
    std::vector<Window> windows;
    for (auto it = std::sregex_iterator(log.begin(), log.end(), gpu); it != std::sregex_iterator();
         ++it)
        windows.push_back({size_t(it->position()), std::stod((*it)[1]), std::stod((*it)[3])});
    const auto range = SelectMeasurementRange(log);
    if (!range)
        return reject(
            "Not enough warmed-up in-game GPU windows; no recommendation from a short run.");
    std::vector<double> times, rates;
    const auto start =
        size_t(std::lower_bound(windows.begin(), windows.end(), range->begin,
                                [](const Window &w, size_t pos) { return w.pos < pos; }) -
               windows.begin());
    if (start >= windows.size())
        return reject("Missing complete GPU timing windows.");
    for (size_t i = start; i < windows.size(); ++i) {
        times.push_back(windows[i].gpu);
        s.worst_window_gpu_p99 = std::max(s.worst_window_gpu_p99, windows[i].p99);
    }
    for (auto it = std::sregex_iterator(log.begin(), log.end(), fps); it != std::sregex_iterator();
         ++it)
        if (size_t(it->position()) >= windows[start].pos &&
            size_t(it->position()) <= windows.back().pos)
            rates.push_back(std::stod((*it)[1]));
    if (rates.size() < 3)
        return reject("Not enough in-game throughput samples.");
    // Do not let easier sections hide the cost of movement/attacks. Use the slower
    // throughput window and the more expensive GPU window, then confirm in another run.
    s.fps = *std::min_element(rates.begin(),rates.end());
    s.gpu_ms = *std::max_element(times.begin(),times.end());
    s.windows = unsigned(windows.size() - start);
    if (s.fps <= 0 || s.gpu_ms <= 0 || s.fps >= 350)
        return reject("Invalid or benchmark-capped throughput.");
    if ((*std::max_element(times.begin(), times.end()) -
         *std::min_element(times.begin(), times.end())) /
            s.gpu_ms >
        .15)
        return reject("GPU timing did not stabilize; shader compilation or "
                      "background work may still be active.");
    const auto tail = log.substr(windows[start].pos, windows.back().pos - windows[start].pos);
    if (c.stereo == StereoPreset::Native) {
        const std::regex native(R"(VR native stereo: \d+ pairs, (\d+)x(\d+) per-eye source)");
        if (!std::regex_search(tail, match, native) || std::stoul(match[1]) != input_w ||
            std::stoul(match[2]) != input_h)
            return reject("Native stereo was not confirmed active during measurement.");
    } else {
        if (tail.find("VR native stereo:") != std::string::npos)
            return reject("Native stereo was active during a requested Simplified Rendering run.");
        const std::regex dibr(
            R"(VR dibr gpu: \d+ flips, (\d+) DIBR frames, (\d+) warped to both eyes)");
        unsigned long long frames = 0, warped = 0;
        for (auto it = std::sregex_iterator(tail.begin(), tail.end(), dibr);
             it != std::sregex_iterator(); ++it) {
            frames += std::stoull((*it)[1]);
            warped += std::stoull((*it)[2]);
        }
        if (frames < 300 || warped < frames * .99 || warped > frames)
            return reject("Simplified Rendering did not consistently reach both eyes during measurement.");
    }
    if (c.graphics[2]) {
        const std::regex dropout(R"(VR temporal native:[^\r\n]*skipped;[^\r\n]*draws=([1-9]\d*))");
        if (std::regex_search(tail, dropout))
            return reject("Upscaler reconstruction dropped out during measurement.");
        if (tail.find("both eyes recorded " + std::to_string(output_w) + "x" +
                      std::to_string(output_h)) == std::string::npos)
            return reject("No upscaler completion evidence in the selected "
                          "measurement window.");
    }

    std::vector<double> intervals;
    std::istringstream lines(tail);
    std::string line;
    while (std::getline(lines, line)) {
        const auto marker = line.find("VR frame times: ");
        if (marker == std::string::npos)
            continue;
        const auto delimiter = line.find(" intervals (ms):", marker);
        if (delimiter == std::string::npos)
            continue;
        const auto expected = std::stoul(line.substr(marker + 16, delimiter - marker - 16));
        std::istringstream values(line.substr(delimiter + 16));
        double value;
        unsigned count = 0;
        while (values >> value) {
            if (!std::isfinite(value) || value <= 0)
                return reject("Invalid frame interval.");
            intervals.push_back(value);
            ++count;
        }
        if (count != expected)
            return reject("Incomplete frame-interval telemetry.");
    }
    // 300 frames, or (a slow PC) ten seconds of at least 100 frames: the same span of play.
    const double covered_ms = std::accumulate(intervals.begin(), intervals.end(), 0.0);
    if (intervals.size() < 300 && (intervals.size() < 100 || covered_ms < 10000))
        return reject("Not enough published game-frame intervals to judge pacing.");
    s.frame_times = intervals;
    std::sort(intervals.begin(), intervals.end());
    s.frame_intervals = unsigned(intervals.size());
    s.frame_p99 = intervals[size_t(std::ceil(.99 * intervals.size())) - 1];
    s.frame_p999 = intervals[size_t(std::ceil(.999 * intervals.size())) - 1];
    const std::regex gaps(R"(frames \((\d+) gaps of 250 ms\+\))");
    for (auto it = std::sregex_iterator(tail.begin(), tail.end(), gaps);
         it != std::sregex_iterator(); ++it)
        if (std::stoul((*it)[1]))
            return reject("Long gameplay stalls occurred during the selected "
                          "measurement window.");
    const std::regex heaps(R"(VRAM ((?:\d+/\d+ ?)+) MB)");
    const std::regex pair(R"((\d+)/(\d+))");
    for (auto it = std::sregex_iterator(tail.begin(), tail.end(), heaps);
         it != std::sregex_iterator(); ++it) {
        const auto list = (*it)[1].str();
        double usage = 0, budget = 0;
        for (auto heap = std::sregex_iterator(list.begin(), list.end(), pair);
             heap != std::sregex_iterator(); ++heap)
            if (std::stod((*heap)[2]) > budget) {
                usage = std::stod((*heap)[1]);
                budget = std::stod((*heap)[2]);
            }
        if (budget > 0) {
            s.peak_device_heap_mib = std::max(s.peak_device_heap_mib, usage);
            s.device_heap_budget_mib =
                s.device_heap_budget_mib ? std::min(s.device_heap_budget_mib, budget) : budget;
            s.memory_pressure = std::max(s.memory_pressure, usage / budget);
        }
    }
    if (s.device_heap_budget_mib <= 0)
        return reject("Missing device-memory budget evidence.");
    // Background activity is context, not proof that measured performance is invalid.
    // Confirmation still requires repeatable throughput and acceptable measured pacing.
    if (!std::isfinite(background_cores) || background_cores < 0)
        s.warning = "Background CPU activity could not be measured.";
    else if (background_cores > 1.5)
        s.warning = "Elevated background CPU activity; other applications may affect performance.";
    s.valid = true;
    return s;
}
Result Summarize(const Candidate &c, const std::vector<Sample> &samples, int target) {
    Result r;
    r.candidate = c;
    r.target_hz = target;
    r.repeats = samples;
    if (target < 30 || target > 240) {
        r.reason = "Invalid target refresh rate.";
        return r;
    }
    std::vector<double> rates, times;
    for (auto &s : samples)
        if (s.valid) {
            rates.push_back(s.fps);
            times.push_back(s.gpu_ms);
        }
    if (rates.empty()) {
        r.reason = "Two valid repeats are required.";
        for (const auto &sample : samples)
            if (!sample.valid && !sample.reason.empty())
                r.reason = "Not enough valid repeats: " + sample.reason;
        return r;
    }
    r.fps = *std::min_element(rates.begin(), rates.end());
    r.gpu_ms = *std::max_element(times.begin(), times.end());
    const double slowest = *std::min_element(rates.begin(), rates.end());
    if ((*std::max_element(rates.begin(), rates.end()) - slowest) / r.fps > .10) {
        // Disagreeing repeats only matter when they put the target in doubt: when even the
        // slower one clears it with the required reserve, judge by the slower one (2026-10-22: one
        // repeat at 128 FPS beside another at 156, both far above 72 Hz, ended a scan with no
        // recommendation because of background CPU).
        if (slowest < target * RequiredReserve) {
            r.reason = "Repeated throughput differs by more than 10%; rerun with the PC idle.";
            return r;
        }
        r.fps = slowest;
        r.gpu_ms = *std::max_element(times.begin(), times.end());
    }
    r.reliable = rates.size() >= 2;
    r.provisional = rates.size() == 1;
    // Every repeat must preserve the reserve; averaging cannot hide a slower run.
    r.meets_target = r.fps >= target * RequiredReserve;
    constexpr double tolerance = .08;
    const double acceptable_rate = target * (1.0 - tolerance);
    r.near_target = r.fps >= acceptable_rate;
    for (auto &s : samples)
        if (s.valid && (s.gpu_ms > 1000.0 / (target*RequiredReserve) || !EvaluatePacing(s, target).acceptable || s.memory_pressure >= .85))
            r.meets_target = false;
    for (const auto &s : samples)
        if (s.valid && (s.gpu_ms > 1000.0 / acceptable_rate ||
                        !EvaluatePacing(s, acceptable_rate).acceptable ||
                        s.memory_pressure > .9))
            r.near_target = false;
    r.near_target = r.near_target && !r.meets_target;
    if (r.near_target)
        r.reason = "Limited headroom: the required 13.8% reserve was not confirmed. May be up to 8% below target.";
    else if (!r.meets_target)
        r.reason = "Frame pacing, throughput, or available GPU memory misses the 8% tolerance.";
    if (r.provisional)
        r.reason = "Provisional: one usable run; not yet confirmed. " + r.reason;
    return r;
}
std::optional<size_t> Recommend(const std::vector<Result> &results, bool provisional) {
    std::optional<size_t> best;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        if (!(provisional ? r.provisional : r.reliable) || (!r.meets_target && !r.near_target))
            continue;
        if (!best || (r.meets_target && !results[*best].meets_target) ||
            (r.meets_target == results[*best].meets_target &&
             (r.candidate.quality > results[*best].candidate.quality ||
              (r.candidate.quality == results[*best].candidate.quality &&
               r.fps > results[*best].fps * 1.05))))
            best = i;
    }
    return best; // No fabricated winner when every option misses the target.
}
Json ToJson(const Result &r) {
    Json samples = Json::array();
    for (auto &s : r.repeats) {
        const auto pacing = EvaluatePacing(s, r.target_hz * .92);
        samples.push_back({{"valid", s.valid},
                           {"reason", s.reason},
                           {"warning", s.warning},
                           {"fps", s.fps},
                           {"gpu_ms", s.gpu_ms},
                           {"worst_window_gpu_p99", s.worst_window_gpu_p99},
                           {"background_cores", s.background_cores},
                           {"windows", s.windows},
                           {"frame_p99", s.frame_p99},
                           {"frame_p999", s.frame_p999},
                           {"frame_intervals", s.frame_intervals},
                           {"slow_frame_fraction_at_tolerance", pacing.slow_frame_fraction},
                           {"slow_time_fraction_at_tolerance", pacing.slow_time_fraction},
                           {"longest_slow_burst_ms_at_tolerance", pacing.longest_slow_ms},
                           {"ordered_pacing_available", !s.frame_times.empty()},
                           {"peak_logged_device_heap_mib", s.peak_device_heap_mib},
                           {"device_heap_budget_mib", s.device_heap_budget_mib},
                           {"memory_pressure", s.memory_pressure},
                           {"input_width", s.input_width},
                           {"input_height", s.input_height},
                           {"output_width", s.output_width},
                           {"output_height", s.output_height}});
    }
    return {{"id", r.candidate.id},
            {"label", r.candidate.label},
            {"stereo", r.candidate.stereo == StereoPreset::Native ? "native" : "dibr"},
            {"graphics", GraphicsText(r.candidate.graphics)},
            {"upscale_focus", r.candidate.upscale_focus},
            {"dlss_model", r.candidate.model},
            {"quality_rank", r.candidate.quality},
            {"fps", r.fps},
            {"gpu_ms", r.gpu_ms},
            {"reliable", r.reliable},
            {"provisional", r.provisional},
            {"meets_target", r.meets_target},
            {"near_target", r.near_target},
            {"target_tolerance_percent", 8},
            {"required_reserve_percent", RequiredReservePercent},
            {"scan_policy", 2},
            {"target_hz", r.target_hz},
            {"reason", r.reason},
            {"repeats", samples}};
}
Profile ApplySettings(const Paths &paths, const Profile &profile, const Candidate &candidate,
                      int target_hz, const std::string &expected_ini) {
    if (!ValidGameRefreshHz(target_hz))
        throw std::runtime_error("Invalid recommended refresh rate.");
    // The model is the preset's (candidate.model only names it): never pin one in vr.ini.
    std::vector<VR::Ini::Edit> edits{{"graphics_config", GraphicsText(candidate.graphics)},
                                     {"dlss_model", "builtin"},
                                     {"aa_mode", "smaa"},
                                     {"refresh_hz", std::to_string(target_hz)}};
    if (FoveatedUpscaleAvailable(candidate.graphics, candidate.stereo))
        edits.push_back({"upscale_focus", std::to_string(candidate.upscale_focus)});
    if (candidate.stereo == StereoPreset::Dibr)
        edits.push_back({"dibr_eye_taa", "0.25"});
    const auto next_ini = VR::Ini::ApplyEdits(expected_ini, edits);
    auto next = profile;
    next.stereo =
        candidate.stereo == StereoPreset::Native ? StereoChoice::Native : StereoChoice::Dibr;
    next.refresh_hz = target_hz;
    next.follow_headset = false; // Pin the recommendation to the measured target.
    const auto previous_profile = Read(paths.Profile());
    next.Save(paths);
    const auto applied_profile = Read(paths.Profile());
    try {
        Write(paths.Ini(), next_ini, expected_ini);
    } catch (...) {
        // Guard rollback too: never overwrite another writer's intervening profile edit.
        Write(paths.Profile(), previous_profile, applied_profile);
        throw;
    }
    return next;
}
std::string UncapPatch(const std::string &xml) { return PatchFrameRate(xml, 360); }

} // namespace Launcher::Benchmark
