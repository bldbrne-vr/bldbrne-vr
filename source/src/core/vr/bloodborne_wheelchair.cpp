// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/vr/bloodborne_wheelchair.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/seqlock.h"
#include "core/vr/vr_session.h"
#include "core/vr/wheelchair.h"

namespace VR::Bloodborne {
namespace {
namespace W=VR::Wheelchair;
using Hook::Read;
using Hook::Plausible;
u8* base{};
bool enabled{};
struct Input { CorpseHandsIn hands{};u32 player_handle{}; };
struct Gift { const u8* player{};u32 handle{};s64 ns{}; };
SeqLock<Input> input;
SeqLock<Gift> gift;
// Where each holding hand is drawn: on its grip, as the chair now stands (bloodborne_body.cpp
// CorpseGripPresent). The chair's wheels, not the controller, decide where the handles go.
SeqLock<std::array<CorpseGrip,2>> chair_grips;
std::array<std::atomic<s64>,2> near_ns{};
// Only a recent pose from seated Gehrman requests the player's hand snapshot.
std::atomic<s64> chair_seen_ns{};
std::atomic_flag step_busy=ATOMIC_FLAG_INIT;
W::Push push;
// The last pose sent to the game's warp. The game moves the chair only half way to it each frame
// (the mock, 2026-11-04: asked 50 mm, moved 25 mm, every frame), so the next step builds on this
// target, not on the root, and the chair keeps up a frame behind it.
struct Sent { W::Vec to{}; float yaw{}; s64 at{}; bool on{}; };
Sent sent;
// The ground under and ahead of it. Map sweeps are the game's own Havok casts, about 20 us each on
// its thread (eleven a frame cost 0.2 ms while pushing, the mock 2026-11-05). So the floor it
// stands on is the one probed under the last step's destination (`floor`), and the way ahead (the
// step at its leading edge, stairs, walls, the wheels over a drop) is looked at AheadReuse past the
// step and kept for the next steps the same way, at most AheadNs (`ahead`): about 2.5 sweeps a
// frame rolling. `rest`: its root's height above the floor while standing on it; `lifted`: the
// last step lifted it onto a step (then that height is not measured).
constexpr float AheadReuse=.12f;
constexpr s64 AheadNs=100'000'000;
struct Ground {
    float rest{};
    bool lifted{};
    struct Floor { W::Vec at{}; int under{}; float height{}; Body::Vec3 normal{}; bool on{}; } floor;
    struct Ahead { W::Vec from{},way{}; float yaw{},edge{}; s64 ns{}; bool step{},on{}; } ahead;
};
Ground ground;
W::Ride ride;
// Where each hand closed on the chair, in its frame (x right, y up, z forward, metres from its
// root): that hand's handle while it holds.
std::array<W::Vec,2> grip_local{};
std::array<bool,2> grip_set{};
const u8* owner{};
u32 owner_handle{},npc_handle{};
s64 last_step{};
s64 next_distance_check{};
float last_scale{};
using WarpFn=void(PS4_SYSV_ABI*)(u8*,float*,float*,u32);
using FlagFn=u32(PS4_SYSV_ABI*)(void*,u32);
constexpr u64 WarpRva=0x18CD600,FlagRva=0x13CFB60;
constexpr std::array<u8,22> WarpSig{0x55,0x48,0x89,0xE5,0x41,0x56,0x53,0x41,0x89,0xCE,0x48,0x89,0xFB,0x66,0xC7,0x83,0x9D,0x02,0,0,1,0};
constexpr std::array<u8,24> FlagSig{0x44,0x8B,0x4F,0x1C,0x31,0xD2,0x89,0xF0,0x41,0xF7,0xF1,0x89,0xC1,0x41,0x89,0xC8,0x45,0x0F,0xAF,0xC1,0x4C,0x8B,0x57,0x38};
W::Vec Vec(Body::Vec3 v) { return {v.x,v.y,v.z}; }
Body::Vec3 BodyVec(W::Vec v) { return {v.x,v.y,v.z}; }
bool Quiet() { return VR::IsImmersive()&&!VR::CinematicActive()&&!VR::IsGameMenuOpen()&&!VR::IsNpcTalking(); }
bool StoryAllows() {
    u8* flags=Read<u8*>(base+0x553B100);
    if(!Plausible(flags)||Read<u32>(flags+0x1C)==0||!Plausible(Read<u8*>(flags+0x38)))return false;
    const auto flag=[flags](u32 id){return reinterpret_cast<FlagFn>(base+FlagRva)(flags,id)!=0;};
    // These are read-only: never override story state or force a hidden Gehrman into the Dream.
    if(flag(1028)||flag(1029)||flag(1030)||flag(12101802)||flag(72100136))return false;
    return flag(1021)||flag(1023)||flag(1025)||flag(72100133);
}
// The pose path's story check, at most once a second (9 native flag reads each frame before;
// the tester, 2026-11-04: it must never slow the game). Only the pose path calls it (step_busy).
bool StoryAllowsCached(s64 now) {
    static s64 next{};
    static bool allowed{};
    if(now>=next) {
        allowed=StoryAllows();
        next=now+1'000'000'000;
    }
    return allowed;
}
// The floor under a point, from 30 cm above it to 60 cm below: Floor (its height), Inside (the
// probe starts inside something low: a rug edge, a hearth, furniture; not a floor problem, the
// wall check owns obstacles) or Drop (nothing within 60 cm: a ledge).
enum class Under { Floor, Inside, Drop };
Under Probe(W::Vec point,float& height,Body::Vec3& normal,float above=.30f) {
    const Body::Vec3 a{point.x,point.y+above,point.z},b{point.x,point.y-.60f,point.z};
    float t{};
    if(SweepMapSphere(a,b,.03f,normal,t)!=MapSweep::Hit)return Under::Drop;
    if(t<=.001f||!Body::IsFinite(normal))return Under::Inside;
    height=a.y+(b.y-a.y)*t-.03f;
    return std::isfinite(height)?Under::Floor:Under::Inside;
}
// Walls and anything over 30 cm (a step it cannot be lifted onto) block only the way the chair
// goes: a 25 cm sphere swept from its centre to 30 cm past the step, 55 cm and 1 m up (`lift`
// higher on stairs, so their risers are no wall), so it stops about 5 cm short. Already touching
// something, it still moves away from or along it (a 55 cm sphere around it refused every move
// near a wall, the headset run 8, 2026-11-04), never further in (W::StartInsideMoveOk). A turn in
// place is not checked.
bool ClearMove(W::Vec root,W::Vec dest,float lift) {
    const W::Vec step{dest.x-root.x,0,dest.z-root.z};
    const float length=W::Length(step);
    if(!std::isfinite(length))return false;
    if(length<.00001f)return true;
    const W::Vec way=step*(1/length);
    for(float y:{.55f+lift,1.0f+lift}) {
        const W::Vec from=root+W::Vec{0,y,0};
        Body::Vec3 normal{};
        float t{};
        const auto sweep=SweepMapSphere(BodyVec(from),BodyVec(from+way*(.30f+length)),.25f,normal,t);
        if(sweep==MapSweep::Unavailable)return false;
        if(sweep==MapSweep::Hit&&t>.001f)return false;
        // t ~0: it starts inside or touching: out or along, not deeper.
        if(sweep==MapSweep::Hit&&!W::StartInsideMoveOk(way,{normal.x,normal.y,normal.z}))return false;
    }
    return true;
}
}
void InstallGehrmanChair(VAddr address,u64 size) {
    // On by default (2026-11-04: "I want it always ON, so I can test it");
    // VR_GEHRMAN_CHAIR=0 turns it off.
    const char* opt=std::getenv("VR_GEHRMAN_CHAIR");
    if((opt&&std::strcmp(opt,"0")==0)||!address||size<=0x5562878+8)return;
    auto* module=reinterpret_cast<u8*>(address);
    if(std::memcmp(module+WarpRva,WarpSig.data(),WarpSig.size())||
       std::memcmp(module+FlagRva,FlagSig.data(),FlagSig.size())) {
        LOG_WARNING(Core,"VR wheelchair: native signature mismatch; disabled");return;
    }
    base=module;enabled=true;
    LOG_INFO(Core,"VR wheelchair: prototype on (VR_GEHRMAN_CHAIR=0 turns it off); seated c8040 only, native movement, 10 m ride gift");
}
void GehrmanChairHands(const CorpseHandsIn& hands) {
    if(!enabled)return;
    const s64 seen=chair_seen_ns.load(std::memory_order_relaxed);
    if(!W::ChairRecentlySeen(seen,hands.now))return;
    Input value{hands,0};
    std::array<CorpseGrip,2> grips{};
    if(LatestCorpseGrips(grips))for(int h=0;h<2;++h)
        if(grips[h].on&&hands.now>=grips[h].ns&&hands.now-grips[h].ns<250'000'000)
            value.hands.bare[h]=false;
    if(Plausible(hands.player))value.player_handle=Read<u32>(hands.player+8);
    input.Store(value);
}
// Diagnostics (log builds only; the headset run 2026-11-04 refused silently). The first
// character ids the pose hook sees (is c8040 there, at chr+0x74?), and the gate that refuses,
// at most every 5 s per reason.
std::atomic<bool> census_done{false};
void NoteCharacter(s32 id) {
    if(census_done.load(std::memory_order_relaxed))return;
    static std::mutex mutex;
    static std::array<std::atomic<s32>,48> seen{};
    static std::atomic<int> count{0};
    const int known=count.load(std::memory_order_acquire);
    if(known>=int(seen.size()))return;
    for(int i=0;i<known;++i)if(seen[i].load(std::memory_order_relaxed)==id)return;
    std::scoped_lock lock{mutex};
    const int n=count.load(std::memory_order_relaxed);
    for(int i=0;i<n;++i)if(seen[i].load(std::memory_order_relaxed)==id)return;
    if(n>=int(seen.size()))return;
    seen[n].store(id,std::memory_order_relaxed);count.store(n+1,std::memory_order_release);
    LOG_INFO(Core,"VR wheelchair: character c{:04} seen (pose hook chr+0x74){}",id,
             id==8040?": Gehrman's chair":"");
    if(id==8040)census_done.store(true,std::memory_order_relaxed);
}
void Why(const char* why,s64 now) {
    // Each reason at most every 5 s: two that alternate (far, then no fresh hands after the
    // distance pause) logged ~6 lines a second when throttled together.
    static std::array<const char*,16> reasons{};
    static std::array<s64,16> next{};
    size_t i=0;
    while(i<reasons.size()&&reasons[i]&&reasons[i]!=why)++i;
    if(i==reasons.size())return;
    if(reasons[i]==why&&now<next[i])return;
    reasons[i]=why;next[i]=now+5'000'000'000;
    LOG_INFO(Core,"VR wheelchair: c8040 not pushable now: {}",why);
}
void GehrmanChairPose(u8* chr) {
    if constexpr (Common::Log::Enabled) {
        if(enabled&&Plausible(chr))NoteCharacter(Read<s32>(chr+0x74));
    }
    if(!enabled||!Plausible(chr)||Read<s32>(chr+0x74)!=8040)return;
    if(step_busy.test_and_set(std::memory_order_acquire))return;
    struct Unlock { ~Unlock() { step_busy.clear(std::memory_order_release); } } unlock;
    const s64 now=Body::SteadyNowNs();
    if(now<next_distance_check)return;
    chair_seen_ns.store(now,std::memory_order_relaxed);
    // Any invalid native state releases input ownership immediately, not after hint expiry.
    bool valid=false;
    struct ResetInvalid {
        bool& valid;
        ~ResetInvalid() {
            if(valid||last_step==0)return;
            push.Release();ride.CancelPending();gift.Store({});chair_grips.Store({});last_step=0;
            for(auto& stamp:near_ns)stamp.store(0,std::memory_order_relaxed);
        }
    } reset{valid};
    Input in{};
    if(!input.Load(in,3)||in.hands.now<=0||now<in.hands.now||now-in.hands.now>100'000'000||
       !Quiet()) {
        if constexpr (Common::Log::Enabled)
            Why(!Quiet()?"not quiet (a menu, a talk, a cutscene or not immersive)"
                        :"no fresh hands from the player's pose (100 ms)",now);
        return;
    }
    const float scale=in.hands.scale;
    if(!std::isfinite(scale)||scale<.01f||scale>100||!Body::IsFinite(in.hands.feet)) {
        if constexpr (Common::Log::Enabled) Why("the player's scale or feet invalid",now);
        return;
    }
    u8* manager=Read<u8*>(base+0x553E878);
    if(!Plausible(manager)) {
        if constexpr (Common::Log::Enabled) Why("no WorldChrMan",now);
        return;
    }
    const u8* player=Read<u8*>(manager+0x60);
    if(!Plausible(player)||player!=in.hands.player||Read<u32>(player+8)!=in.player_handle) {
        if constexpr (Common::Log::Enabled) Why("the hands are not the main player's",now);
        return;
    }
    u8* vt=Read<u8*>(chr);
    if(vt!=base+0x538E2D0&&vt!=base+0x538EBC0) {
        if constexpr (Common::Log::Enabled) {
            static bool once=false;
            if(!once) {
                once=true;
                LOG_INFO(Core,"VR wheelchair: c8040's vtable is +{:#x}",
                         reinterpret_cast<uintptr_t>(vt)-reinterpret_cast<uintptr_t>(base));
            }
            Why("its vtable is not the ChrIns/EnemyIns one",now);
        }
        return;
    }
    if(Read<u8*>(vt+0x5F8)!=base+WarpRva) {
        if constexpr (Common::Log::Enabled) Why("its warp slot (vtable +0x5F8) is not the warp",now);
        return;
    }
    u8* ctrl=Read<u8*>(chr+0x58);u8* mods=Read<u8*>(chr+0x3B0);
    if(!Plausible(ctrl)||Read<u8*>(ctrl+8)!=chr||!Plausible(mods)) {
        if constexpr (Common::Log::Enabled) Why("its controller or modules invalid",now);
        return;
    }
    u8* physics=Read<u8*>(mods+0x68);u8* data=Read<u8*>(mods+0x20);
    if(!Plausible(physics)||Read<u8*>(physics+8)!=chr||!Plausible(data)||Read<u8*>(data+8)!=chr||Read<s32>(data+0xF8)<=0) {
        if constexpr (Common::Log::Enabled) Why("its physics or data module invalid, or HP 0",now);
        return;
    }
    W::Vec root{Read<float>(ctrl+0x9C),Read<float>(ctrl+0xAC),Read<float>(ctrl+0xBC)};
    const float yaw=Read<float>(physics+0x1D4);
    if(!W::Finite(root)||!std::isfinite(yaw)) {
        if constexpr (Common::Log::Enabled) Why("its root or yaw not finite",now);
        return;
    }
    const auto distance=root-Vec(in.hands.feet);
    if(W::Dot(distance,distance)>9*scale*scale) {
        // Out of reach: stop streaming hand data until we next check proximity.
        chair_seen_ns.store(0,std::memory_order_relaxed);
        next_distance_check=now+200'000'000;
        if constexpr (Common::Log::Enabled)
            Why("the player more than 3 m away (its root to the player's feet)",now);
        return;
    }
    if(!StoryAllowsCached(now)) {
        if constexpr (Common::Log::Enabled) Why("the story flags (Gehrman's state) refuse",now);
        return;
    }
    const u32 handle=Read<u32>(chr+8);
    if(owner!=in.hands.player||owner_handle!=in.player_handle||npc_handle!=handle) {
        push={};ride={};gift.Store({});ground={};owner=in.hands.player;owner_handle=in.player_handle;npc_handle=handle;
    } else if(scale!=last_scale||now<=last_step||now-last_step>250'000'000) {
        // A pause (a menu, a talk, a hitch, a walk away): grip again, the metres so far stay.
        push={};ride.CancelPending();ground={};
    }
    last_step=now;last_scale=scale;
    // Use the character's actual world basis, not an assumed relationship to Euler yaw.
    W::Vec right{Read<float>(ctrl+0x90),0,Read<float>(ctrl+0xB0)};
    W::Vec forward{Read<float>(ctrl+0x98),0,Read<float>(ctrl+0xB8)};
    const float rl=W::Length(right),fl=W::Length(forward);
    if(!W::Finite(right)||!W::Finite(forward)||rl<.9f||rl>1.1f||fl<.9f||fl>1.1f) {
        if constexpr (Common::Log::Enabled) Why("its world axes are not unit length",now);
        return;
    }
    right=right*(1/rl);forward=forward*(1/fl);
    // Build on the last target while the chair is on its way to it (within 30 cm and 30 degrees,
    // sent in the last 60 ms); a blocked chair falls back to its own root.
    const W::Vec actual=root,actual_right=right,actual_forward=forward;
    float waiting_turn=0;
    const float behind_turn=std::remainder(sent.yaw-yaw,6.2831853f);
    if(sent.on&&now-sent.at<60'000'000&&W::Length(sent.to-root)<.30f*scale&&
       std::abs(behind_turn)<.52f) {
        root=sent.to;
        waiting_turn=behind_turn;
        const float c=std::cos(waiting_turn),s=std::sin(waiting_turn);
        const W::Vec r=right*c-forward*s,f=right*s+forward*c;
        right=r;forward=f;
    }
    sent.on=false;
    // A hand grips the chair anywhere on it (its back, sides, frame), not at two guessed handle
    // points (the headset run 2026-11-04: hands 41-73 cm from those never caught). Each grip is
    // kept in the chair's frame, so the handle the push turns about is where the hand closed.
    const W::Vec root_m=root*(1/scale);
    const auto to_local=[&](W::Vec p) {
        const W::Vec d=p-root_m;
        return W::Vec{W::Dot(d,right),d.y,W::Dot(d,forward)};
    };
    const auto to_world=[&](W::Vec l) { return root_m+right*l.x+W::Vec{0,l.y,0}+forward*l.z; };
    // The handles were measured at about (+-0.21, 1.24, +0.60) in the headset (2026-11-04); the
    // zone covers both ends of the chair along its axis and its sides.
    const auto on_chair=[](W::Vec l) {
        return std::abs(l.x)<=.55f&&l.y>=.45f&&l.y<=1.5f&&std::abs(l.z)<=.9f;
    };
    std::array<W::Hand,2> hands{};
    for(int h=0;h<2;++h) {
        // The hand's turn about the vertical: its rotation's twist about up, as a level direction
        // (zero when the hand points straight up or down: no turn to read).
        const auto& q=in.hands.q[h];
        const float w2=q.w*q.w+q.y*q.y;
        const W::Vec facing=w2>.1f ? W::Vec{2*q.w*q.y/w2,0,(q.w*q.w-q.y*q.y)/w2} : W::Vec{};
        hands[h]={Vec(in.hands.palm[h])*(1/scale),in.hands.squeeze[h],in.hands.bare[h],facing};
    }
    std::array<W::Vec,2> handles{};
    std::array<bool,2> reach{};
    for(int h=0;h<2;++h) {
        reach[h]=hands[h].bare&&W::Finite(hands[h].position)&&on_chair(to_local(hands[h].position));
        // Holding: where it closed. Free on the chair: the hand itself (a squeeze grips there,
        // each hand its own handle). Elsewhere: nowhere it could grip.
        handles[h]=push.Holding(h)&&grip_set[h] ? to_world(grip_local[h])
                   : reach[h]                   ? hands[h].position
                                                : root_m+W::Vec{0,-100.0f,0};
    }
    const auto motion=push.Update(now*1e-9,true,root_m,right,forward,handles,hands);
    valid=true;
    for(int h=0;h<2;++h) {
        if(push.Holding(h)&&!grip_set[h]) {
            grip_local[h]=to_local(handles[h]);
            grip_set[h]=true;
            if constexpr (Common::Log::Enabled)
                LOG_INFO(Core,"VR wheelchair: {} hand grips the chair at ({:.2f}, {:.2f}, {:.2f}) m "
                         "(right, up, forward of its root)",h?"right":"left",grip_local[h].x,
                         grip_local[h].y,grip_local[h].z);
        } else if(!push.Holding(h)) {
            grip_set[h]=false;
        }
    }
    const auto publish=[&](W::Vec at,W::Vec r,W::Vec f) {
        std::array<CorpseGrip,2> grips{};
        for(int h=0;h<2;++h) {
            if(!push.Holding(h)||!grip_set[h])continue;
            const W::Vec l=grip_local[h];
            grips[h]={now,true,BodyVec((at+r*l.x+W::Vec{0,l.y,0}+f*l.z)*scale)};
        }
        chair_grips.Store(grips);
    };
    publish(root_m,right,forward);
    if constexpr (Common::Log::Enabled) {
        static s64 next_hands{};
        static bool was_held{};
        if(motion.held!=was_held) {
            was_held=motion.held;
            if(!motion.held)LOG_INFO(Core,"VR wheelchair: the chair let go");
        }
        // C0000 order: 0 left, 1 right.
        const W::Vec l=to_local(hands[0].position),r=to_local(hands[1].position);
        const auto close_by=[](W::Vec v){return std::abs(v.x)<1.f&&v.y>.2f&&v.y<1.8f&&std::abs(v.z)<1.2f;};
        if(!motion.held&&(close_by(l)||close_by(r))&&now>=next_hands) {
            next_hands=now+2'000'000'000;
            LOG_INFO(Core,"VR wheelchair: near, not gripped: left bare={} squeeze={:.2f} on the "
                     "chair={} at ({:.2f}, {:.2f}, {:.2f}); right bare={} squeeze={:.2f} on the "
                     "chair={} at ({:.2f}, {:.2f}, {:.2f}) (a squeeze from open grips)",
                     hands[0].bare,hands[0].squeeze,reach[0],l.x,l.y,l.z,hands[1].bare,
                     hands[1].squeeze,reach[1],r.x,r.y,r.z);
        }
    }
    for(int h=0;h<2;++h)
        near_ns[h].store(reach[h]||push.Holding(h)?now:0,std::memory_order_relaxed);
    ride.Observe(actual*(1/scale),now*1e-9,motion.held);
    if(ride.Earned())gift.Store({owner,owner_handle,now});
    // Log builds: every 2 s while held, how much of the asked steps and turns the game carried out
    // (the headset run 9: it stopped at a step and trailed, with no refusal), and each ride metre.
    struct Asked {
        bool pending{};
        W::Vec from{},to{},right{},forward{};
        float turn{};
        s64 at{},next{};
        float asked{},moved{},rose{},turn_asked{},turned{};
        int metres{-1};
        W::Vec actual{};
    };
    static Asked asked;
    if constexpr (Common::Log::Enabled) {
        if(asked.pending) {
            asked.pending=false;
            if(now-asked.at<100'000'000) {
                // The new step's length against how far the game moved it since it was sent.
                const W::Vec want=asked.to-asked.from,got=actual*(1/scale)-asked.actual;
                const float length=std::hypot(want.x,want.z);
                if(length>.00001f) {
                    asked.asked+=length;
                    asked.moved+=(got.x*want.x+got.z*want.z)/length;
                }
                asked.rose+=got.y;
                asked.turn_asked+=asked.turn;
                asked.turned+=std::atan2(W::Dot(actual_forward,asked.right),
                                         W::Dot(actual_forward,asked.forward));
            }
        }
        if(now>=asked.next) {
            if(asked.asked>.02f||std::abs(asked.turn_asked)>.02f)
                LOG_INFO(Core,"VR wheelchair: 2 s held: asked {:.0f} cm, the game moved it {:.0f} cm "
                         "(up {:.0f} cm); asked a turn of {:.0f} deg, it turned {:.0f} deg; ride "
                         "{:.1f} of 10 m",asked.asked*100,asked.moved*100,asked.rose*100,
                         asked.turn_asked*57.2958f,asked.turned*57.2958f,ride.Metres());
            asked.asked=asked.moved=asked.rose=asked.turn_asked=asked.turned=0;
            asked.next=now+2'000'000'000;
        }
        if(int(ride.Metres())>asked.metres) {
            asked.metres=int(ride.Metres());
            LOG_INFO(Core,"VR wheelchair: ride {} of 10 m ({}){}",asked.metres,
                     ride.Earned()||ride.Seconds()>=W::Ride::GiftSeconds?"pushed long enough"
                                                                    :"not pushed long enough yet",
                     ride.Earned()?": the Kos Lights gift is ready":"");
        }
    }
    if(W::Length(motion.delta)<.0001f&&std::abs(motion.yaw)<.0001f)return;
    // Its own height above the floor under its centre (a seated character's root need not touch
    // it), taken while it stands on that floor, not lifted onto a step.
    Body::Vec3 normal_here{};
    float floor_here{};
    Under here;
    if(ground.floor.on&&ground.floor.at.x==root.x&&ground.floor.at.z==root.z) {
        here=static_cast<Under>(ground.floor.under);floor_here=ground.floor.height;
        normal_here=ground.floor.normal;
    } else {
        here=Probe(root,floor_here,normal_here);
    }
    if(here==Under::Floor&&!ground.lifted)ground.rest=std::clamp(root.y-floor_here,-.15f,.15f);
    bool step_lifted=false;
    Ground::Floor seen{};
    const float heading=yaw+waiting_turn;
    // The step's checks; `why` names the first that refuses.
    const auto check=[&](W::Vec delta,float step_yaw,W::Vec& dest,float& grade,const char*& why) {
        dest=root+delta*scale;
        step_lifted=false;
        // Its height is always from the floor: the floor under its centre (a change over 30 cm,
        // a ledge, stops it), or a step up to 30 cm where its leading edge arrives (lifted onto it,
        // clear of the edge: the headset run 9, the game's own collision held it at a step), plus
        // `rest`. (Each frame's lift built on the last floated it 56 cm up, the mock 2026-11-05.)
        // No floor found keeps the height (the headset run 7: rugs, the hearth).
        Body::Vec3 normal{0,1,0};
        float floor_there{};
        const Under there=Probe(dest,floor_there,normal);
        const W::Vec flat{delta.x,0,delta.z};
        const float flat_length=W::Length(flat);
        const W::Vec way=flat_length>.0001f?flat*(1/flat_length):W::Vec{};
        const bool floors=here==Under::Floor&&there==Under::Floor;
        if(floors) {
            if(std::abs(floor_there-floor_here)>.30f) { why="a ledge over 30 cm under it"; return false; }
            dest.y=floor_there+ground.rest;
        } else {
            dest.y=root.y;
            normal={0,1,0};
        }
        if(there==Under::Drop&&here==Under::Floor) { why="a drop under its centre"; return false; }
        seen={dest,static_cast<int>(there),floor_there,normal,true};
        const float turn_sin=std::sin(step_yaw),turn_cos=std::cos(step_yaw);
        const W::Vec next_right=right*turn_cos-forward*turn_sin;
        const W::Vec next_forward=right*turn_sin+forward*turn_cos;
        grade=normal.y>.5f?-Body::Dot(normal,BodyVec(next_forward))/normal.y:0.0f;
        // The way ahead, kept while it rolls on the same way within the stretch it was looked at.
        auto& look=ground.ahead;
        const bool rolling=flat_length>.0001f;
        if(look.on&&now-look.ns<=AheadNs&&rolling==(W::Length(look.way)>.5f)&&
           (!rolling||W::Dot(way,look.way)>=.95f)&&
           std::hypot(dest.x-look.from.x,dest.z-look.from.z)<=AheadReuse&&
           std::abs(std::remainder(heading+step_yaw-look.yaw,6.2831853f))<=.1f) {
            if(floors&&look.step&&look.edge+ground.rest>dest.y) {
                dest.y=look.edge+ground.rest;
                step_lifted=true;
            }
            return true;
        }
        look.on=false;
        const W::Vec beyond=dest+way*AheadReuse;
        float edge_floor{};
        bool step=false;
        if(floors&&rolling) {
            W::Vec edge=dest+way*.45f;
            edge.y=floor_there;
            Body::Vec3 edge_normal{};
            if(Probe(edge,edge_floor,edge_normal)==Under::Floor) {
                const float rise=edge_floor-floor_there;
                if(rise>.03f&&rise<=.30f) { dest.y=edge_floor+ground.rest;step=step_lifted=true; }
            }
        }
        const float floor=there==Under::Floor?floor_there:dest.y;
        // The wheels where it will be AheadReuse on guard drops only (something tall under one is
        // the wall check's): two over a drop of 50 cm or more (a ledge; stairs going down are
        // less) stop it; one is fine (the headset run 8: one wheel by a 25 cm step stopped every
        // push).
        int wheels_over_drop=0;
        for(float x:{-.36f,.36f})for(float z:{-.30f,.48f}) {
            W::Vec corner=beyond+next_right*x+next_forward*z;
            corner.y=floor;
            Body::Vec3 corner_normal{};
            float y{};
            const Under under=Probe(corner,y,corner_normal);
            if((under==Under::Drop||(under==Under::Floor&&corner.y-y>.50f))&&++wheels_over_drop>=2) {
                why="two wheels over a drop (a ledge)";
                return false;
            }
        }
        // Walls, then (only when something is in the way) stairs or a ramp ahead, no step of them
        // over 30 cm: the wall check rides that much higher (2026-11-05: "it should not
        // get stuck on carpet and bumps (or even stairs)"). Near a wall it looks only as far as
        // this step (and keeps nothing), so it still rolls up close.
        const auto clear=[&](W::Vec to) {
            if(ClearMove(root,to,0))return true;
            if(!floors||!rolling)return false;
            float lift=0,last=floor_there;
            for(float d:{.35f,.7f}) {
                W::Vec at=dest+way*d;
                at.y=last;
                Body::Vec3 n{};
                float h{};
                if(Probe(at,h,n,.45f)!=Under::Floor||std::abs(h-last)>.30f)break;
                last=h;
                lift=std::max(lift,h-floor_there);
            }
            return lift>.03f&&ClearMove(root,to,std::min(lift,.6f));
        };
        if(!clear(beyond)) {
            if(!rolling||!clear(dest)) { why="a wall or something tall in the way"; return false; }
            return true;
        }
        look={dest,way,heading+step_yaw,edge_floor,now,step,true};
        return true;
    };
    W::Vec delta=motion.delta,dest{};
    float turn=motion.yaw,grade{};
    const char* why=nullptr;
    if(!check(delta,turn,dest,grade,why)) {
        // Blocked: the turn alone (steering away from the wall or the step), else the roll alone.
        const char* const first=why;
        const bool turns=std::abs(turn)>=.0001f,rolls=W::Length(delta)>=.0001f;
        if(turns&&rolls&&check({},turn,dest,grade,why)) delta={};
        else if(turns&&rolls&&check(delta,0,dest,grade,why)) turn=0;
        else {
            if constexpr (Common::Log::Enabled) Why(first,now);
            return;
        }
    }
    alignas(16) std::array<float,4> position{dest.x,dest.y,dest.z,1};
    alignas(16) std::array<float,4> rotation{};
    std::memcpy(rotation.data(),physics+0x1D0,sizeof(rotation));
    for(float v:rotation)if(!std::isfinite(v))return;
    rotation[1]=yaw+waiting_turn+turn;
    // The game's own character warp updates its controller/collision; never move just the mesh.
    reinterpret_cast<WarpFn>(base+WarpRva)(chr,position.data(),rotation.data(),1);
    chr[0x1E1]|=2;
    ground.lifted=step_lifted;
    ground.floor=seen;
    push.Accepted(root*(1/scale),W::Dot(delta,forward),dest.y/scale,grade,motion.held,turn);
    {
        const float c=std::cos(turn),s=std::sin(turn);
        publish(dest*(1/scale),right*c-forward*s,right*s+forward*c);
    }
    sent={dest,rotation[1],now,true};
    if(motion.held)ride.Expect(actual*(1/scale),dest*(1/scale),now*1e-9);
    if constexpr (Common::Log::Enabled)
        asked={true,root_m,dest*(1/scale),actual_right,actual_forward,turn,now,asked.next,asked.asked,
               asked.moved,asked.rose,asked.turn_asked,asked.turned,asked.metres,actual*(1/scale)};
}
bool GehrmanChairGiftReady(const u8* player) {
    Gift value{};
    if(!enabled||!gift.Load(value,3)||value.player!=player||value.ns<=0)return false;
    const s64 now=Body::SteadyNowNs();
    return now>=value.ns&&now-value.ns<2'000'000'000&&Plausible(player)&&
           value.handle==Read<u32>(player+8)&&Quiet()&&StoryAllows();
}
bool GehrmanChairEnabled() { return enabled; }
bool LatestChairGrips(std::array<CorpseGrip,2>& out) { return enabled&&chair_grips.Load(out,4); }
bool GehrmanChairGrabAvailable(unsigned hand,s64 now) {
    if(hand>=near_ns.size())return false;
    const auto seen=near_ns[hand].load(std::memory_order_relaxed);
    return seen>0&&now>=seen&&now-seen<100'000'000;
}
}
