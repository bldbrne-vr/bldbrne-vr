// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace VR::HunterRelics {
inline constexpr int Tonitrus = 2070, Gloves = 2080;
// Normalize storage to the original pocket sizes independently of the held fit.
inline constexpr float TonitrusHeld = 1.20f, GlovesHeld = 1.61f;
inline constexpr float StoredRatio(int goods,int slot) {
    return goods==Gloves ? (slot==2?.60f:.84f)*(1.12f/GlovesHeld)
                         : (slot==2?.65f:.85f)*(.90f/TonitrusHeld);
}
using Vector = std::array<float,3>;
// Shared by storage, drop clearance and the offline geometry check.
inline constexpr Vector GloveMin{-.066f,-.018f,-.177f}, GloveMax{.041f,.049f,.034f};
inline constexpr Vector GloveMid{(GloveMin[0]+GloveMax[0])*.5f,
                                 (GloveMin[1]+GloveMax[1])*.5f,
                                 (GloveMin[2]+GloveMax[2])*.5f};
inline bool Finite(const Vector& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline float Length2(const Vector& v) { return v[0]*v[0]+v[1]*v[1]+v[2]*v[2]; }
inline Vector Unit(const Vector& v) {
    const float n=Length2(v);
    if(!Finite(v) || !std::isfinite(n) || n<1e-8f) return {};
    const float k=1.0f/std::sqrt(n);
    return {v[0]*k,v[1]*k,v[2]*k};
}
inline Vector GroundHeading(const Vector& stroke,const Vector& pointed) {
    const auto v=Unit(stroke);
    if(v[0]*v[0]+v[2]*v[2]>.20f*.20f)return Unit({v[0],0,v[2]});
    const auto fallback=Unit(pointed);
    if(fallback[0]*fallback[0]+fallback[2]*fallback[2]>.20f*.20f)
        return Unit({fallback[0],0,fallback[2]});
    return {};
}
// ---- The charged Tiny Tonitrus' strike (the tester's 2026-11-03 "look good and feel fantastic") --
// Which stroke strikes: one going down, or forward or to the side. One going up (lifting it to
// strike) or back toward you (pulling it back) is the wind-up: it fires nothing, the next stroke
// is read (until 2026-11-03 any stroke struck, and a wind-up sent the lightning behind you).
inline constexpr float StrikeUpMax = .50f;     // a stroke rising more than 30 deg: the wind-up
inline constexpr float StrikeDownMin = -.35f;  // one falling 20 deg or more: a strike, any way
inline constexpr float StrikeBackCos = -.50f;  // a level one more than 120 deg from your look
// Where it goes: a strike down (within 37 deg of straight down) or one pulled toward you goes
// where you look; a slash goes its own way, and within StrikeSnapCos of where you look exactly
// there (aim help: a swing at what you look at hits it). Without the head, where the tool
// pointed while charging.
inline constexpr float StrikeSlashFlat = .60f;  // the stroke's level share for a slash
inline constexpr float StrikeSnapCos = .81915f; // cos 35 deg
inline constexpr float StrikeTowardCos = -.342f; // cos 110 deg: a slash toward you
inline bool StrikeStroke(const Vector& stroke, const Vector& look) {
    const auto s=Unit(stroke);
    if(Length2(s)<.5f || s[1]>StrikeUpMax) return false;
    if(s[1]<=StrikeDownMin) return true;
    const auto f=GroundHeading({},look),h=GroundHeading(s,{});
    return Length2(f)<.5f || Length2(h)<.5f || h[0]*f[0]+h[2]*f[2]>=StrikeBackCos;
}
inline Vector StrikeHeading(const Vector& stroke, const Vector& look, const Vector& pointed) {
    const auto gaze=GroundHeading({},look);
    const auto aim=Length2(gaze)>.5f ? gaze : GroundHeading({},pointed);
    const auto s=Unit(stroke);
    const float flat=std::sqrt(s[0]*s[0]+s[2]*s[2]);
    if(!(flat>=StrikeSlashFlat)) return aim;
    const Vector h{s[0]/flat,0,s[2]/flat};
    if(Length2(aim)<.5f) return h;
    const float d=h[0]*aim[0]+h[2]*aim[2];
    return d>=StrikeSnapCos || d<StrikeTowardCos ? aim : h;
}
// Locked on (the game aims its lightning at the lock target), a strike within StrikeSnapCos of
// the game's own heading keeps the game's: thrust at your target, it hits it. Level headings.
inline bool StrikeKeepsLock(bool locked, const Vector& game, const Vector& heading) {
    const auto g=GroundHeading({},game),h=GroundHeading({},heading);
    return locked && Length2(g)>.5f && Length2(h)>.5f && g[0]*h[0]+g[2]*h[2]>=StrikeSnapCos;
}
// The bridge (AddTonitrusBridge): the bolt from the tool to the chain's first link and two
// weaker restrikes along new paths, the last fading out over 0.2 s as an afterglow,
// BridgeLifeNs in all (190 ms, one strike, until 2026-11-03).
inline constexpr std::int64_t BridgeLifeNs = 400'000'000;
struct BridgeStrike {
    int index{-1};      // 0 the strike, 1 and 2 the restrikes; -1 over
    float age{0};       // seconds into this one (the shader's flicker and endpoint flash)
    float strength{0};  // its brightness, with the bridge's fade at the end
};
inline BridgeStrike BridgeStrikeAt(float seconds) {
    constexpr float start[3]{0.0f,.105f,.185f}, strength[3]{1.0f,.85f,.70f};
    const float life=static_cast<float>(BridgeLifeNs)*1e-9f;
    if(!(seconds>=0.0f) || seconds>=life) return {};
    const int i=seconds>=start[2] ? 2 : seconds>=start[1] ? 1 : 0;
    const float fade=std::clamp((life-seconds)/.20f,0.0f,1.0f);
    return {i,seconds-start[i],strength[i]*fade};
}
// Native root-to-child confirmation and accepted item use must belong to this take.
// Stale events after put-away/re-grab, pauses or a refused use cannot flash.
inline bool BridgeActive(std::int64_t cast,std::int64_t contact,std::int64_t accepted,
                         std::int64_t session,std::int64_t now) {
    return session>0 && cast>=session && accepted>=cast && accepted-cast<=400'000'000 &&
           contact>=cast && contact-cast<=400'000'000 && now>=accepted && now>=contact &&
           now-contact<BridgeLifeNs;
}
// One per-strike roll of the bolt about its own line and a mirror, from the cast and the strike
// (no two strikes alike; the same frame twice the same).
inline std::uint32_t BoltSeed(std::int64_t cast, int strike) {
    std::uint64_t x=static_cast<std::uint64_t>(cast)*0x9E3779B97F4A7C15ull+
                    static_cast<std::uint64_t>(strike+1)*0xBF58476D1CE4E5B9ull;
    x^=x>>31;x*=0x94D049BB133111EBull;x^=x>>29;
    return static_cast<std::uint32_t>(x);
}
struct DropRequest {
    std::int64_t ns{},held_since{};
    std::uint32_t space{};
    Vector velocity{}; // release velocity in tracking metres/s, captured once
};
struct DropPlan {
    DropRequest request{};
    std::int64_t start_ns{},land_ns{};
    Vector start{},land{},normal{}; // world-space mesh centre and floor normal
    float scale{1};
    bool valid{};
    Vector velocity{}, impact{}; // world velocity; end of the checked flight
    std::int64_t impact_ns{};
    bool tossed{};
    bool wall{}; // the flight hit a wall: from the impact it falls to the floor at its foot
    bool enemy{}; // ... an enemy (the wall's fall, at its feet; the tester's 2026-11-04 "stop on enemies")
};
// A wall drop's fall from its impact (game units, down) to its floor: the time it takes.
inline std::int64_t WallFallNs(float fall,float scale) {
    return 80'000'000+static_cast<std::int64_t>(std::sqrt(2*std::max(fall,0.0f)/(9.81f*scale))*1e9f);
}
inline bool Matches(const DropRequest& a,const DropRequest& b) {
    return a.ns>0 && a.ns==b.ns && a.held_since==b.held_since && a.space==b.space;
}
// A sweep fraction locates the sphere CENTRE along its path. Native hit positions
// may instead be contact points; never seat the glove centre directly at that point.
inline Vector SweepCentre(const Vector& from,const Vector& to,float fraction) {
    return {from[0]+(to[0]-from[0])*fraction,
            from[1]+(to[1]-from[1])*fraction,
            from[2]+(to[2]-from[2])*fraction};
}
inline DropPlan PlanDrop(DropRequest request,std::int64_t now,const Vector& start,
                         const Vector& hit,const Vector& normal,float scale) {
    DropPlan plan{};plan.request=request;plan.start_ns=now;
    if(request.ns<=0 || request.held_since<=0 || request.ns<request.held_since ||
       now<request.ns || now-request.ns>250'000'000 || !Finite(start) || !Finite(hit) ||
       !Finite(normal) || !std::isfinite(scale) || scale<.01f || scale>100.0f)return plan;
    const auto n=Unit(normal);
    if(n[1]<.25f)return plan; // walls/undersides are not a floor
    const float drop=(start[1]-hit[1])/scale;
    if(drop<.005f || drop>5.0f || std::hypot(start[0]-hit[0],start[2]-hit[2])>.08f*scale)return plan;
    const float seconds=std::sqrt(2*drop/9.81f);
    plan.start=start;plan.land=hit;plan.normal=n;plan.scale=scale;
    plan.land_ns=now+static_cast<std::int64_t>(seconds*1e9f);plan.valid=true;
    return plan;
}
// Glove-only launch bounds, shared by the producer and received-plan validation. A real throw
// carries (the tester's 2026-11-04 "able to be thrown far, properly": 6.5 m/s and 6 m before).
inline constexpr float GloveTossSpeed=21.f, GloveTossRange=26.f;
inline constexpr float GloveTossSpeedLimit2=GloveTossSpeed*GloveTossSpeed+.01f;
// Three recent relative-position samples: walking with a still hand does not throw.
// No peak search, stale launch velocity or tracking-spike inheritance.
class GloveRelease {
public:
    void Reset() { *this={}; }
    void Update(std::int64_t session,std::uint32_t space,double now,bool valid,const Vector& p) {
        if(!valid || session<=0 || !std::isfinite(now) || !Finite(p)){Reset();return;}
        if(session!=session_ || space!=space_ || time_<=0 || now<=time_ || now-time_>.05) {
            Reset();session_=session;space_=space;time_=now;position_=p;return;
        }
        const float dt=float(now-time_);Vector v{};
        for(int i=0;i<3;++i)v[i]=(p[i]-position_[i])/dt;
        position_=p;time_=now;
        if(!Finite(v) || Length2(v)>900.f){count_=0;return;} // over 30 m/s: a tracking spike
        samples_[2]=samples_[1];samples_[1]=samples_[0];samples_[0]=v;
        count_=std::min(3u,count_+1);
    }
    Vector Velocity() const {
        if(count_<3)return {};
        Vector v{};
        for(int i=0;i<3;++i)v[i]=.5f*samples_[0][i]+.3f*samples_[1][i]+.2f*samples_[2][i];
        const float speed=std::sqrt(Length2(v));
        if(!std::isfinite(speed) || speed<.35f)return {};
        // A 2 m/s flick launches at about 7 m/s, a firm 5 m/s throw at about 16 m/s.
        const float gain=GloveTossSpeed*std::tanh(4.5f*(speed-.35f)/GloveTossSpeed)/speed;
        for(auto& x:v)x*=gain;
        return v;
    }
private:
    std::array<Vector,3> samples_{};
    Vector position_{};
    double time_{};
    std::int64_t session_{};
    std::uint32_t space_{};
    unsigned count_{};
};
inline constexpr float TossStep=.05f;
inline constexpr unsigned TossSteps=64; // 3.2 s of flight: a far throw lands within it
enum class SweepState { Unavailable, Clear, Hit };
struct Sweep { SweepState state{SweepState::Unavailable}; float fraction{};Vector normal{}; };
inline Vector ArcPoint(const Vector& start,const Vector& velocity,float scale,float t) {
    const float horizontal=t/(1+.06f*t); // a little air drag; deliberate throws carry
    return {start[0]+velocity[0]*horizontal,
            start[1]+velocity[1]*t-4.905f*scale*t*t,start[2]+velocity[2]*horizontal};
}
inline Vector FlightPoint(const DropPlan& p,float t) {
    const float step=std::floor(t/TossStep)*TossStep;
    return SweepCentre(ArcPoint(p.start,p.velocity,p.scale,step),
                       ArcPoint(p.start,p.velocity,p.scale,step+TossStep),(t-step)/TossStep);
}
// How far (horizontally) a flight at `velocity` comes down to `drop` below its start, by ArcPoint
// alone (no geometry); 0 if it never does.
inline float ArcReach(const Vector& velocity,float scale,float drop) {
    const float g=9.81f*scale,disc=velocity[1]*velocity[1]+2*g*drop;
    if(!(disc>=0))return 0.0f;
    const float t=(velocity[1]+std::sqrt(disc))/g;
    return t>0?std::hypot(velocity[0],velocity[2])*t/(1+.06f*t):0.0f;
}
// How one checked flight of PlanToss ended: landed (the plan is valid), cancelled, or too far (it
// lands, or would, beyond GloveTossRange or 5 m up or down, or it is still flying at the end of
// the checked flight; `reach`/`drop`: how far away and how far below the start it landed, reach 0
// if it did not).
enum class TossEnd { Cancel, Far, Landed };
// At most TossSteps flight sweeps (the map's, and `foe`'s against the enemies) plus one floor
// seating sweep. Unavailable geometry, an overlap or an underside cancels; a wall drops the glove to
// the floor at its foot, and so does an enemy it meets first (its Sweep's normal: back out toward
// the thrower); never tunnel onward.
template<class Cast,class Foe>
inline TossEnd TossFlight(DropPlan& p,std::int64_t now,const Vector& start,const Vector& velocity,
                          float scale,float radius,float seat,Cast& cast,Foe& foe,float& reach,
                          float& drop) {
    Vector previous=start;
    for(unsigned i=1;i<=TossSteps;++i) {
        const auto next=ArcPoint(start,velocity,scale,i*TossStep);
        Vector delta{};for(int a=0;a<3;++a)delta[a]=next[a]-previous[a];
        if(Length2(delta)<1e-12f*scale*scale){previous=next;continue;}
        auto hit=cast(previous,next,radius);
        if(hit.state==SweepState::Unavailable)return TossEnd::Cancel;
        const Sweep met=foe(previous,next,radius);
        const bool enemy=met.state==SweepState::Hit && std::isfinite(met.fraction) &&
                         met.fraction>=0 && met.fraction<=1 &&
                         (hit.state!=SweepState::Hit || !(met.fraction>=hit.fraction));
        if(enemy) {
            // Off the enemy toward the thrower, flat: the way back along the flight without one.
            auto away=Unit({met.normal[0],0,met.normal[2]});
            if(Length2(away)<.5f)away=Unit({-delta[0],0,-delta[2]});
            hit={SweepState::Hit,std::max(met.fraction,.002f),away};
        }
        if(hit.state==SweepState::Hit) {
            const auto normal=Unit(hit.normal);
            // An underside (a ceiling) cancels; a wall stops the flight and the glove falls to
            // the floor at its foot (the tester's 2026-11-04 "thrown far, properly": a thrown glove
            // that met a wall used to vanish without a summon).
            if(!std::isfinite(hit.fraction)||hit.fraction<=.001f||hit.fraction>1||normal[1]<-.25f)
                return TossEnd::Cancel;
            const bool wall=enemy || normal[1]<.25f;
            const float t=((i-1)+hit.fraction)*TossStep;
            // A glove striking a ceiling/shelf on its upward leg does not summon there.
            if(!wall && velocity[1]-9.81f*scale*t>=0)return TossEnd::Cancel;
            const auto impact=SweepCentre(previous,next,hit.fraction);
            Vector from=impact,below{};
            if(wall) {
                for(int a=0;a<3;++a)from[a]=impact[a]+normal[a]*(radius-seat+.01f*scale); // off it
                below=from;below[1]-=5*scale;
            } else {
                for(int a=0;a<3;++a)below[a]=impact[a]-normal[a]*(radius-seat+.015f*scale);
            }
            const auto floor=cast(from,below,seat);
            if(floor.state==SweepState::Clear && wall)return TossEnd::Far; // a wall over a drop
            if(floor.state!=SweepState::Hit || !std::isfinite(floor.fraction) ||
               floor.fraction<=0 || floor.fraction>1 || Unit(floor.normal)[1]<.25f)
                return TossEnd::Cancel;
            const auto land=SweepCentre(from,below,floor.fraction);
            const float away=std::hypot(land[0]-start[0],land[2]-start[2]);
            if(away>GloveTossRange*scale || std::abs(land[1]-start[1])>5*scale) {
                const bool level=std::abs(land[1]-start[1])<=5*scale;
                reach=level?away:0.0f;drop=start[1]-land[1];
                return TossEnd::Far;
            }
            p.start=start;p.land=land;p.normal=Unit(floor.normal);p.scale=scale;p.velocity=velocity;
            p.impact=impact;p.impact_ns=now+static_cast<std::int64_t>(t*1e9f);p.wall=wall;
            p.enemy=enemy;
            p.land_ns=p.impact_ns+(wall?WallFallNs(impact[1]-land[1],scale):80'000'000);
            p.tossed=true;p.valid=true;return TossEnd::Landed;
        }
        previous=next;
    }
    return TossEnd::Far;
}
// A throw too strong for its reach is not lost (the tester's 2026-11-04 "Yes, fix it": the hardest
// throws, landing past GloveTossRange, were cancelled and the glove stayed in the hand): it flies
// again the same way, only softer, to land just inside the range where the first came down (the
// speed solved on ArcReach; a quarter softer when no landing was seen), at most TossTries flights.
inline constexpr unsigned TossTries=3;
inline float SofterFor(const Vector& velocity,float scale,float reach,float drop) {
    if(!(reach>0))return .75f;
    const float want=.95f*GloveTossRange*scale;
    float lo=.3f,hi=1.0f;
    for(int i=0;i<16;++i) {
        const float k=.5f*(lo+hi);
        const Vector v{velocity[0]*k,velocity[1]*k,velocity[2]*k};
        (ArcReach(v,scale,drop)>want?hi:lo)=k;
    }
    return std::min(lo,.95f);
}
// At most TossTries x (TossSteps + 1) sweeps, once per release. `foe` (the flight against the
// enemies, a Sweep along the step; omitted: none) stops it on the first one it meets.
template<class Cast,class Foe>
inline DropPlan PlanToss(DropRequest request,std::int64_t now,const Vector& start,
                         const Vector& velocity,float scale,float radius,float seat,Cast&& cast,
                         Foe&& foe) {
    DropPlan p{};p.request=request;p.start_ns=now;
    if(request.ns<=0 || request.held_since<=0 || request.ns<request.held_since ||
       now<request.ns || now-request.ns>250'000'000 ||
       now>std::numeric_limits<std::int64_t>::max()-5'000'000'000 || !Finite(request.velocity) ||
       Length2(request.velocity)>GloveTossSpeedLimit2 || !Finite(start) || !Finite(velocity) ||
       !std::isfinite(scale) || scale<.01f || scale>100 ||
       Length2(velocity)>GloveTossSpeedLimit2*scale*scale || !std::isfinite(radius) ||
       !std::isfinite(seat) || seat<=0 || radius<seat || radius>.5f*scale)return p;
    Vector v=velocity;
    for(unsigned attempt=0;attempt<TossTries;++attempt) {
        float reach=0.0f,drop=0.0f;
        if(TossFlight(p,now,start,v,scale,radius,seat,cast,foe,reach,drop)!=TossEnd::Far)return p;
        const float k=SofterFor(v,scale,reach,drop);
        for(auto& x:v)x*=k;
    }
    return p;
}
template<class Cast>
inline DropPlan PlanToss(DropRequest request,std::int64_t now,const Vector& start,
                         const Vector& velocity,float scale,float radius,float seat,Cast&& cast) {
    return PlanToss(request,now,start,velocity,scale,radius,seat,cast,
                    [](const Vector&,const Vector&,float){return Sweep{SweepState::Clear};});
}
// The thrown glove's spirits (the tester's 2026-11-04 "when we throw it, it needs to spawn the ghost
// that go and attack the nearest enemy, right now the ghosts die out too quickly, they do not fly
// around. WHY??"). The game's BulletParam 350: three spirits, 20 degrees apart, shot 20 degrees
// below the launch line (shootAngle -20) at 4 m/s, living 3 s (20 m), homing at 120 deg/s after
// their first 2.5 m. The game's own use launches them from the hand, about 1.2 m up, so they fly
// a few metres before the ground. From the landed glove they started 6-10 cm off the floor, dived
// into it at once and burst on the spot (the mock: gone in 0.04 s, never moving). They now rise
// from the glove (`rise`, SpiritRiseM where the space above is clear) and fly at the nearest enemy
// within SpiritSeekM of it (its lock point), else level along the throw; SpiritLaunchLine lifts
// the launch line by the param's dive so the spirits' own line is that one.
inline constexpr float SpiritRiseM = 1.0f, SpiritSeekM = 15.0f, SpiritDiveDeg = 20.0f;
struct SpiritAim {
    Vector origin{};
    Vector direction{}; // zero: nothing usable (the game's own line is kept)
    int target{-1};     // the chosen one of `targets`, -1: none in reach
};
inline SpiritAim AimSpirits(const Vector& land,const Vector* targets,std::size_t count,
                            const Vector& heading,float scale,float rise) {
    SpiritAim a{};
    if(!Finite(land) || !std::isfinite(scale) || scale<.01f || scale>100 || !std::isfinite(rise) ||
       rise<0 || rise>2*SpiritRiseM*scale)return a;
    a.origin={land[0],land[1]+rise,land[2]};
    float best=SpiritSeekM*scale*SpiritSeekM*scale;
    for(std::size_t i=0;targets!=nullptr && i<count;++i) {
        const auto& t=targets[i];
        if(!Finite(t))continue;
        const float d2=Length2({t[0]-land[0],t[1]-land[1],t[2]-land[2]});
        if(d2<best){best=d2;a.target=static_cast<int>(i);}
    }
    if(a.target>=0) {
        const auto& t=targets[a.target];
        a.direction=Unit({t[0]-a.origin[0],t[1]-a.origin[1],t[2]-a.origin[2]});
    } else {
        a.direction=Unit({heading[0],0,heading[2]});
    }
    return a;
}
// The launch line for spirits that should fly along `line` (unit, world): pitched up by the
// param's dive (SpiritDiveDeg), at most 80 degrees up; zero for a vertical or zero line.
inline Vector SpiritLaunchLine(const Vector& line) {
    const auto u=Unit(line);
    const float flat=std::sqrt(u[0]*u[0]+u[2]*u[2]);
    if(flat<1e-3f)return {};
    const float pitch=std::min(std::atan2(u[1],flat)+SpiritDiveDeg*3.14159265f/180,
                               80*3.14159265f/180);
    const float c=std::cos(pitch)/flat;
    return {u[0]*c,std::sin(pitch),u[2]*c};
}
inline Vector DropCentre(const DropPlan& plan,std::int64_t now) {
    if(!plan.valid || now<=plan.start_ns)return plan.start;
    if(now>=plan.land_ns)return plan.land;
    if(plan.tossed) {
        if(now>=plan.impact_ns) {
            float u=float(now-plan.impact_ns)/float(plan.land_ns-plan.impact_ns);
            return SweepCentre(plan.impact,plan.land,plan.wall?u*u:u*u*(3-2*u));
        }
        return FlightPoint(plan,float(now-plan.start_ns)*1e-9f);
    }
    const float t=static_cast<float>(now-plan.start_ns)*1e-9f;
    return {plan.start[0],std::max(plan.land[1],plan.start[1]-.5f*9.81f*plan.scale*t*t),plan.start[2]};
}
inline bool ValidDrop(const DropPlan& plan) {
    if(!plan.valid)return false;
    if(plan.tossed) {
        if(plan.request.ns<=0 || plan.request.held_since<=0 || plan.request.ns<plan.request.held_since ||
           plan.start_ns<plan.request.ns || plan.start_ns-plan.request.ns>250'000'000 ||
           plan.impact_ns<=plan.start_ns ||
           plan.impact_ns-plan.start_ns>static_cast<std::int64_t>(TossSteps*TossStep*1e9f) ||
           plan.land_ns<=plan.impact_ns ||
           plan.land_ns-plan.impact_ns!=(plan.wall?WallFallNs(plan.impact[1]-plan.land[1],plan.scale)
                                                  :80'000'000) ||
           !Finite(plan.start)||!Finite(plan.land)||
           !Finite(plan.impact)||!Finite(plan.velocity)||!Finite(plan.normal)||
           !std::isfinite(plan.scale)||plan.scale<.01f||plan.scale>100 ||
           Length2(plan.velocity)>GloveTossSpeedLimit2*plan.scale*plan.scale || Unit(plan.normal)[1]<.25f ||
           !Finite(plan.request.velocity)||Length2(plan.request.velocity)>GloveTossSpeedLimit2)return false;
        const auto expected=FlightPoint(plan,float(plan.impact_ns-plan.start_ns)*1e-9f);
        Vector error{},settle{};for(int i=0;i<3;++i){error[i]=expected[i]-plan.impact[i];settle[i]=plan.impact[i]-plan.land[i];}
        return Length2(error)<.002f*.002f*plan.scale*plan.scale &&
               Length2(settle)<(plan.wall?5.5f*5.5f:.5f*.5f)*plan.scale*plan.scale && settle[1]>=0 &&
               std::hypot(plan.land[0]-plan.start[0],plan.land[2]-plan.start[2])<=GloveTossRange*plan.scale &&
               std::abs(plan.land[1]-plan.start[1])<=5*plan.scale;
    }
    const auto rebuilt=PlanDrop(plan.request,plan.start_ns,plan.start,plan.land,plan.normal,plan.scale);
    return rebuilt.valid && rebuilt.land_ns==plan.land_ns;
}
inline float Ease(float t) { t=std::clamp(t,0.0f,1.0f);return t*t*(3.0f-2.0f*t); }
// One damped point for the hanging palm. The cuff stays pinned; the GPU spreads
// this displacement smoothly into the leather. No cloth solver, heap or history.
// Inputs are metres in a stable world frame; rotation changes the target too.
class SoftGlove {
public:
    void Reset() { *this={}; }
    Vector Update(std::int64_t now,std::int64_t session,std::uint32_t space,
                  const Vector& target) {
        if(session<=0 || !Finite(target)) { Reset();return {}; }
        const double dt=double(now-time_)*1e-9;
        if(now==time_ && session==session_ && space==space_) {
            Vector same{};for(int i=0;i<3;++i)same[i]=position_[i]-previous_[i];
            return same; // a second view must not advance or reset the spring
        }
        Vector jump{};for(int i=0;i<3;++i)jump[i]=target[i]-previous_[i];
        if(session!=session_ || space!=space_ || dt<=0 || dt>.05 || Length2(jump)>.25f) {
            position_=previous_=target;velocity_={};time_=now;session_=session;space_=space;
            return {};
        }
        previous_=target;time_=now;
        // Exact underdamped spring step: stable at 60/90/120 Hz and after jitter.
        constexpr float damping=9.0f,frequency=21.0f;
        const float e=std::exp(-damping*float(dt)),c=std::cos(frequency*float(dt)),
                    s=std::sin(frequency*float(dt))/frequency;
        Vector offset{};
        for(int i=0;i<3;++i) {
            const float y=position_[i]-target[i],v=velocity_[i];
            offset[i]=e*(y*c+(v+damping*y)*s);
            velocity_[i]=e*(v*c-(damping*v+(frequency*frequency+damping*damping)*y)*s);
        }
        const float n=Length2(offset);
        if(!Finite(offset) || !Finite(velocity_) || !std::isfinite(n)) { Reset();return {}; }
        if(n>.045f*.045f) {
            const float k=.045f/std::sqrt(n);
            for(int i=0;i<3;++i){offset[i]*=k;velocity_[i]*=k;}
        }
        for(int i=0;i<3;++i)position_[i]=target[i]+offset[i];
        return offset;
    }
private:
    Vector position_{},previous_{},velocity_{};
    std::int64_t time_{},session_{};
    std::uint32_t space_{};
};
struct DropVisual {
    bool detached{};
    float alpha{1},open{},burst{};
};
// Pure bounded choreography shared by rendering and finger pose. The hand stays
// tracked throughout. A rejected/cancelled drop cannot look like a successful summon.
inline DropVisual GloveVisual(const DropPlan& plan,std::int64_t session,
                              bool pending,std::int64_t answer,bool used,std::int64_t now) {
    if(!plan.valid || session<=0 || plan.request.held_since!=session ||
       now<plan.start_ns || now>plan.land_ns+850'000'000)return {};
    const bool replied=answer>=plan.land_ns && answer<=plan.land_ns+250'000'000 && answer<=now;
    if(replied && !used)return {};
    if(!replied && !pending && (now<plan.land_ns || now>plan.land_ns+250'000'000))return {};
    const float opening=Ease(float(now-plan.start_ns)*1e-9f/.10f);
    if(!replied)return {true,1,opening,0};
    const float age=float(now-answer)*1e-9f;
    if(age<.28f)return {true,1-Ease((age-.035f)/.245f),1,1-age/.45f};
    return {false,Ease((age-.28f)/.30f),1-Ease((age-.28f)/.30f),
            std::max(0.0f,1-age/.60f)};
}
template<class Shape> inline void OpenGloveHand(Shape& shape,float amount) {
    amount=std::clamp(amount,0.0f,1.0f);
    for(int finger=0;finger<5;++finger)
        for(int joint=0;joint<3;++joint) {
            const float target=finger==0?(joint==0?0.0f:.16f):(joint==0?.22f:.10f);
            shape.bend[finger][joint]+=(target-shape.bend[finger][joint])*amount;
        }
    shape.thumb_oppose+=(.20f-shape.thumb_oppose)*amount;
}
// Tracks only the stroke after the charge is ready. Positions and velocity are relative
// to the head, so walking/crouching alone cannot cast. Fixed-size, no heap or history scan.
class ChargedSwing {
public:
    void Reset() { *this={}; }
    Vector Update(double now, bool valid, const Vector& p, const Vector& v) {
        if(!valid || !std::isfinite(now) || !Finite(p) || !Finite(v)) { Reset(); return {}; }
        if(!have_ || now<=time_ || now-time_>.050) {
            Reset(); have_=true; time_=now; previous_=p; return {};
        }
        const float dt=static_cast<float>(now-time_);time_=now;
        Vector delta{p[0]-previous_[0],p[1]-previous_[1],p[2]-previous_[2]};previous_=p;
        const float speed2=Length2(v),distance2=Length2(delta);
        // Reject tracking spikes and require both reported speed and actual displacement.
        if(!std::isfinite(speed2) || speed2<1.0f || speed2>400.0f ||
           !std::isfinite(distance2) || distance2>(20.0f*dt)*(20.0f*dt)) {
            travel_={}; return {};
        }
        const float dot=delta[0]*travel_[0]+delta[1]*travel_[1]+delta[2]*travel_[2];
        if(dot<0.0f)travel_={}; // a reversal starts a new stroke; jitter never accumulates
        for(int i=0;i<3;++i)travel_[i]+=delta[i];
        if(Length2(travel_)<.08f*.08f) return {};
        const Vector result=Unit(travel_);Reset();return result;
    }
private:
    Vector previous_{},travel_{};
    double time_{};
    bool have_{};
};
} // namespace VR::HunterRelics
