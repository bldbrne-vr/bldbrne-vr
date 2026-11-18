#include "benchmark.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace Launcher;
using namespace Launcher::Benchmark;
namespace {
unsigned checks=0;
void Check(bool value, const char* message) {
    ++checks;
    if(!value) throw std::runtime_error(message);
}
Sample Measurement(double fps, double gpu, double pressure=.5) {
    Sample s;
    s.valid=true;s.fps=fps;s.gpu_ms=gpu;s.frame_p99=1000.0/fps*1.03;
    s.memory_pressure=pressure;s.windows=5;
    return s;
}
Candidate Configuration(int scale, int preset=2, StereoPreset stereo=StereoPreset::Native) {
    Candidate c;c.stereo=stereo;c.graphics[1]=scale-100;
    c.graphics[2]=stereo==StereoPreset::Native?1:0;
    c.graphics[3]=c.graphics[4]=preset;
    c.id=c.graphics[2]?"upscale-1-"+std::to_string(preset)+"-"+std::to_string(scale):"base-dibr-"+std::to_string(scale);
    c.quality=Rank(stereo,scale,c.graphics[2],preset);
    return c;
}
}
int main() {
    try {
        SystemInfo system;system.nvidia=true;system.vram=16ull<<30;
        Headset headset{"test",{2448,2448},{2672,2672},8192,8192,90};
        const auto base=Configuration(70);
        auto next=[&](std::vector<Result> history, int target=90) {
            return NextQuickCandidate(system,true,true,history,target,false,true,&headset);
        };
        for(int target:{72,80,90,120,144}) {
            const double minimum = target * RequiredReserve;
            const double gpu_budget = 1000.0 / minimum;
            const auto boundary = Measurement(minimum, gpu_budget);
            const auto confirmed = Summarize(base,{boundary,boundary},target);
            Check(confirmed.meets_target,"exactly 13.8% FPS and GPU reserve is accepted");
            Check(ToJson(confirmed).at("required_reserve_percent").get<double>()==RequiredReservePercent,
                  "report preserves fractional reserve without rounding to an integer");
            const auto weak_fps = Measurement(minimum-.001,gpu_budget);
            Check(!Summarize(base,{boundary,weak_fps},target).meets_target,
                  "one repeat just below 13.8% FPS reserve is not comfortable");
            const auto weak_gpu = Measurement(minimum,gpu_budget+.001);
            Check(!Summarize(base,{boundary,weak_gpu},target).meets_target,
                  "one repeat just over the GPU reserve budget is not comfortable");
            for(double reserve:{RequiredReserve,1.14,1.15,1.20,1.29}) {
                const auto s=Measurement(target*reserve,1000.0/(target*reserve)*.9);
                const auto c=next({Summarize(base,{s},target)},target);
                Check(c && c->id==base.id,"comfortable 13.8-29% band confirms instead of spending reserve");
            }
            const auto insufficient=Measurement(target*(RequiredReserve-.0001),1000.0/(target*(RequiredReserve-.0001))*.9);
            const auto lower=next({Summarize(base,{insufficient},target)},target);
            Check(lower && lower->id!=base.id,"less than 13.8% reserve continues downward");
            const auto fast=Measurement(target*2.0,1000.0/(target*2.0)*.9);
            const auto higher=next({Summarize(base,{fast},target)},target);
            Check(higher && higher->graphics[1]>base.graphics[1],"large excess raises output");
        }
        const auto comfortable=Measurement(108,8);
        const auto fine=next({Summarize(Configuration(50),{Measurement(140,6.8)},90)});
        Check(fine && fine->graphics[1]==-45,
              "strong headroom permits a smaller quality increase when a coarse step costs too much");
        Sample unsupported;unsupported.reason="Controlled runtime rejected swapchain dimensions.";
        const auto safe=Configuration(50);
        const auto bad=Configuration(55);
        const auto recovered=next({Summarize(safe,{Measurement(140,6.8)},90),
                                    Summarize(bad,{unsupported,unsupported},90)});
        Check(recovered && recovered->id==safe.id,
              "unsupported higher resolution returns to the measured fallback after bounded retries");
        Check(!Recommend({Summarize(bad,{unsupported,unsupported},90)}),
              "unsupported dimensions cannot become a recommendation");
        auto bad_pacing=Measurement(112,8);bad_pacing.frame_times.assign(1000,20);
        const auto eligible=next({Summarize(safe,{Measurement(100,9)},90),
            Summarize(base,{Measurement(108,8),bad_pacing},90),
            Summarize(bad,{unsupported,unsupported},90)});
        Check(eligible && eligible->id==safe.id,
              "a repeat with bad pacing cannot displace an eligible fallback through one good sample");
        const auto weak=Measurement(100,9);
        const auto mixed=Summarize(base,{comfortable,weak},90);
        Check(mixed.reliable && !mixed.meets_target && mixed.fps==100,
              "close repeats cannot average a weak result into 13.8% reserve");
        const auto gpu_mixed=Summarize(base,{Measurement(120,7),Measurement(120,10)},90);
        Check(!gpu_mixed.meets_target && gpu_mixed.gpu_ms==10,"slower GPU repeat must preserve reserve too");
        Check(!Summarize(base,{Measurement(140,7,.85),Measurement(140,7,.85)},90).meets_target,
              "memory reserve is required in confirmation");
        auto stutter=Measurement(140,7);stutter.frame_times.assign(1000,7);
        for(int i=200;i<225;++i) stutter.frame_times[i]=20;
        Check(!Summarize(base,{stutter,stutter},90).meets_target,
              "a long slow burst defeats otherwise high FPS");
        const auto flat=Measurement(95,9);
        const auto plateau=next({Summarize(Configuration(90),{flat},90),Summarize(Configuration(70),{flat},90)});
        Check(plateau && plateau->stereo==StereoPreset::Dibr,
              "large pixel reduction without FPS gain tests stereo cost rather than further blurring");
        const auto reversal=next({Summarize(base,{Measurement(100,9)},90),
                                  Summarize(Configuration(70,1),{Measurement(86,10)},90)});
        Check(reversal && reversal->graphics[2]==1 && reversal->graphics[3]==0,
              "a large performance reversal is not mistaken for a stable CPU plateau");
        const auto after_dibr=next({Summarize(Configuration(50),{Measurement(100,9)},90),
            Summarize(Configuration(50,2,StereoPreset::Dibr),{Measurement(90,10)},90)});
        Check(after_dibr && after_dibr->stereo==StereoPreset::Native &&
              after_dibr->graphics[1]==-50 && after_dibr->graphics[3]==0,
              "failed DIBR still tests the cheapest native upscaler option");
        const auto floor=next({Summarize(Configuration(50),{Measurement(60,16)},90)});
        Check(floor && floor->graphics[1]==-50 && floor->graphics[3]==1,
              "output floor still permits a cheaper internal preset");
        const auto limited=Summarize(base,{weak,weak},90);
        const auto continue_search=next({limited});
        Check(continue_search && continue_search->id!=base.id,
              "confirmed limited fallback does not end search for comfortable performance");
        Check(!ScanActivityObserved("VR anim: class locomotion\nVR melee: #1 attack open ("),
              "startup activity cannot substitute for measured gameplay");
        std::string activity="VR projection: image\nVR gpu frame: windows of 5.0 s\n";
        for(int i=0;i<7;++i) activity+="VR gpu frame: 100 game frames:\nVR anim: class locomotion\nVR melee: #1 attack open (onset) ok=1\n";
        Check(ScanActivityObserved(activity),"movement and attacks inside warmed-up windows are accepted");
        auto no_attack=activity;
        for(size_t pos=0;(pos=no_attack.find(" open (",pos))!=std::string::npos;) no_attack.replace(pos,7," denied");
        Check(!ScanActivityObserved(no_attack),"denied swings cannot certify attack workload");
        auto failed_attack=activity;
        for(size_t pos=0;(pos=failed_attack.find("ok=1",pos))!=std::string::npos;) failed_attack.replace(pos,4,"ok=0");
        Check(!ScanActivityObserved(failed_attack),"failed game attack registration cannot certify workload");
        const auto noisy=Summarize(base,{Measurement(122,7),Measurement(100,9)},90);
        const auto recovery=next({noisy});
        Check(!noisy.reliable && recovery && recovery->quality<base.quality,
              "noisy confirmation retries at lower cost without promoting uncertain measurements");

        // Independent performance models: enumerate the feasible floor, then drive the
        // production search with GPU/CPU limits, refresh rates and startup costs.
        unsigned scenarios=0, feasible=0, found=0;
        for(int target:{60,72,80,90,120,144}) for(double gpu100:{4.,8.,12.,18.,26.,38.,52.,72.,96.})
        for(double cpu:{60.,80.,95.,110.,130.,160.,200.,260.}) for(double run_seconds:{42.,59.}) {
            const auto measure=[&](const Candidate& c) {
                const double ratio=c.graphics[2] ? (c.graphics[3]==2?1.:c.graphics[3]==1?.58/(2./3.):.5/(2./3.)) : 1.5;
                const bool dibr=c.stereo==StereoPreset::Dibr;
                const double gpu=gpu100*std::pow((100+c.graphics[1])/100.0*ratio,2)*(dibr?.55:1.);
                return Measurement(std::min(1000/gpu,cpu*(dibr?1.3:1.)),gpu);
            };
            const auto at_floor=measure(Configuration(50,0));
            const auto at_dibr_floor=measure(Configuration(50,2,StereoPreset::Dibr));
            const bool possible=Summarize(base,{at_floor,at_floor},target).meets_target ||
                                Summarize(base,{at_dibr_floor,at_dibr_floor},target).meets_target;
            std::vector<Result> history;
            unsigned runs=0;
            while(runs<12) {
                const auto chosen=Recommend(history);
                if(chosen && history[*chosen].meets_target) break;
                auto c=next(history,target);
                if(!c) break;
                const double elapsed=runs*run_seconds;
                if(runs && elapsed+2*run_seconds*1.1>QuickScanHardSeconds)
                    if(const auto best=Recommend(history,true)) c=history[*best].candidate;
                if(runs && elapsed+run_seconds*1.1>QuickScanHardSeconds) break;
                Check(c->graphics[1]>=-50 && c->graphics[1]<=0,"all search outputs stay within supported bounds");
                auto it=std::find_if(history.begin(),history.end(),[&](const auto& r){return r.candidate.id==c->id;});
                auto samples=it==history.end()?std::vector<Sample>{}:it->repeats;
                samples.push_back(measure(*c));
                const auto result=Summarize(*c,samples,target);
                if(it==history.end()) history.push_back(result); else *it=result;
                ++runs;
            }
            Check(runs<=QuickScanMaxRuns,"every simulated scan respects attempt limit");
            const auto winner=Recommend(history);
            if(winner && history[*winner].meets_target) {
                const auto& result=history[*winner];
                Check(result.reliable && result.repeats.size()>=2,"recommendation has repeated evidence");
                Check(result.fps>=target*RequiredReserve && result.gpu_ms<=1000.0/(target*RequiredReserve),"recommendation retains real reserve");
                ++found;
            }
            if(possible) ++feasible;
            if(possible && !(winner && history[*winner].meets_target)) {
                std::cout<<"Miss: target "<<target<<" GPU "<<gpu100<<" CPU "<<cpu<<" run "<<run_seconds<<":";
                for(const auto& r:history) std::cout<<" "<<r.candidate.id<<"="<<r.fps;
                std::cout<<'\n';
            }
            ++scenarios;
        }
        std::cout<<scenarios<<" simulated scans; "<<found<<" comfortable results / "<<feasible<<" feasible models\n";
        Check(found==feasible,"bounded search finds comfortable values for every feasible model in this matrix");
        std::cout<<checks<<" performance strategy checks passed\n";
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
