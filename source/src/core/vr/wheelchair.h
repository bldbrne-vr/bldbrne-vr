// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::Wheelchair {
struct Vec { float x{}, y{}, z{}; };
inline Vec operator+(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec operator-(Vec a, Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline Vec operator*(Vec a,float s) { return {a.x*s,a.y*s,a.z*s}; }
inline float Dot(Vec a,Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float Length(Vec a) { return std::sqrt(Dot(a,a)); }
inline bool Finite(Vec a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
// `facing`: a level direction that turns with the hand about the vertical (zero: not known).
struct Hand { Vec position{}; float squeeze{}; bool bare{}; Vec facing{}; };
// The chair as the game draws it (c8040's wheel bones in the game's files): big wheels beside the
// seat on an axle AxleForward along its axis from its root (toward the footrest), small swivel
// casters at the back under the handles, 63 cm behind the axle. Like a real wheelchair it rolls
// along its axis and turns on its big wheels; they never slide sideways (2026-11-05: "I
// want it to feel like a REAL wheelchair". Turning about the hands slid them: "it feels unnatural").
inline constexpr float AxleForward = -.03f;
// It rolls with the hands at a run (0.75 m/s until the headset run 2026-11-04: "he still lagged
// behind"): each step takes the held hands' own motion along its axis at once plus the share
// 1 - exp(-dt / FollowS) of the gap, never past where the hands are heading.
// The grip holds while each hand is within GripStretch of where it closed (the hand is drawn there).
inline constexpr float FollowS = .045f, MaxSpeed = 9.0f, MaxTurn = 3.5f, GripStretch = .75f;
// Its heading follows the hold, as a firm grip steers a chair: two hands, the line between them
// against the one at the grab (2026-11-04: "If I pull one arm, like a shopping cart it
// needs to turn ... if they are fixed then it goes straight"); one hand, the hand's own turn,
// softer (WristS). Standing, it turns on its axle with the hold until the hands are TwistRoom
// from their grips (its back swings away from them: a pusher would step around it).
inline constexpr float WristS = .1f, TwistRoom = .45f;
// Rolling (in full from WalkSpeed), it steers as a pusher guides a chair: at the point LookS
// seconds of travel (at least LookMin) ahead of where the hold puts its axle, its heading turning
// 2 sin(angle) / distance per metre rolled (pure pursuit). (Pushed from behind its axle, a chair
// whose back chased the hands turned ever further away: the headset run 7, "he kept turning".)
inline constexpr float WalkSpeed = .4f, LookS = .25f, LookMin = .45f;
// Hands pulling it sideways past SkidFrom (a tight curve, a hard yank) skid its big wheels, at
// most SkidSpeed: the excess closes as the gap along it does.
inline constexpr float SkidFrom = .35f, SkidSpeed = 1.0f;
// At a stand its back swings under hands moved sideways (about its axle), SwingTwo of it while
// two hands hold the heading.
inline constexpr float SwingTwo = .35f;
// A snap turn (the hold turned SnapTurn or more in one step, beyond any wrist, every holding hand
// carried SnapMove or more with it and two hands kept their spacing) carries the chair with the
// hands, as held things go with a snap; nothing is seen to slide. (A tracking glitch, one hand
// jumping or a hand's turn read wrong, is no snap: it turns it only at MaxTurn.)
inline constexpr float SnapTurn = .2f, SnapMove = .03f;
// Let go rolling, it rolls on at the pace it had, up to CoastSpeed (1.5 m/s until the user,
// 2026-11-05: "faster!!!"; 3.5 until "maybe pace up to 6m/s"; 0.55 m/s^2 of rolling resistance on
// flat ground: from 3 m/s about 8 m, from 6 about 33), and a turn let go spins on from up to
// SpinSeed, slowing SpinDrag rad/s^2 (the big wheels roll, the casters swivel).
inline constexpr float CoastSpeed = 6.0f, SpinSeed = 2.0f, SpinDrag = 2.5f;
inline float FollowGain(double dt) { return 1.0f - std::exp(-static_cast<float>(dt) / FollowS); }
inline float Wrap(float a) { return std::remainder(a, 6.2831853f); }
// A level direction's angle in the chair's frame (positive toward `right` from `forward`).
inline float AngleIn(Vec v,Vec right,Vec forward) { return std::atan2(Dot(v,right),Dot(v,forward)); }
// (r, f) in the chair's frame turned by `yaw` the way a positive yaw turns its forward to its right.
inline Vec Turned(float r,float f,float yaw,Vec right,Vec forward) {
    const float c=std::cos(yaw),s=std::sin(yaw);
    return right*(r*c+f*s)+forward*(f*c-r*s);
}
struct Motion { Vec delta{}; float yaw{}; bool held{}; };
// Player and NPC poses can finish in either order in the same frame.
inline bool ChairRecentlySeen(std::int64_t seen,std::int64_t hand_ns) {
    return seen>0 && hand_ns>0 &&
           std::max(seen,hand_ns)-std::min(seen,hand_ns)<=100'000'000;
}
// Metres and seconds. Seed a coast only from confirmed travel and a real grip release.
class Push {
public:
    Motion Update(double now, bool allowed, Vec root, Vec right, Vec forward,
                  const std::array<Vec,2>& handles, const std::array<Hand,2>& hands) {
        Motion out{};
        const double dt=now-last_;
        if (!std::isfinite(now)||!Finite(root)||!Finite(right)||!Finite(forward)||
            !allowed||!std::isfinite(dt)||dt<=0||dt>.1) {
            Release(); last_=std::isfinite(now)?now:0; return out;
        }
        last_=now;
        const Vec travelled=root-before_;
        const float along=Dot(travelled,forward);
        const bool accepted=pending_;
        const bool moved=pending_ && along*requested_>0 && std::abs(root.y-expected_y_)<=.025f &&
            std::abs(along)>=.65f*std::abs(requested_) &&
            std::hypot(travelled.x-forward.x*requested_,travelled.z-forward.z*requested_)
                <=.002f+.15f*std::abs(requested_);
        pending_=false; // the caller must confirm each new step passed clearance and was submitted
        if(!moved) {speed_=0;shove_time_=0;}
        if(!accepted)spin_=0;
        // Credit the duration of the submitted step, even if the observing frame is shorter.
        const float observed_speed=moved ? along/step_seconds_ : 0.f;
        if(moved && was_held_) {
            if(std::abs(observed_speed)<=.45f || observed_speed*shove_direction_<0)shove_time_=0;
            shove_direction_=observed_speed;
            if(std::abs(observed_speed)>.45f)shove_time_=std::min(.2f,shove_time_+step_seconds_);
        }
        // The turn rate of the last held steps the game took (a turn let go spins on; a snap
        // turn's carry is no turn of its own).
        if(was_held_) {
            const float k=1.0f-std::exp(-float(dt)/.08f);
            const bool turned=accepted&&!carried_;
            turn_rate_+=((turned?requested_turn_/step_seconds_:0.0f)-turn_rate_)*k;
        }
        carried_=false;
        step_seconds_=float(dt);
        const Vec axle=root+forward*AxleForward;
        Vec error{},hands_mid{},was_mid{},grip_mid{}; int count=0;
        bool released=false, lost=false;
        std::array<Vec,2> grip_at{}, hand_at{};
        float moved_f=0, arm_r=0, arm_f=0; int fed=0;
        bool carried=true; // every holding hand moved SnapMove or more this step
        for (int h=0;h<2;++h) {
            const auto& hand=hands[h];
            if(!hand.bare||!Finite(hand.position)||!std::isfinite(hand.squeeze)) {
                lost|=held_[h]>=0;held_[h]=-1; armed_[h]=false;continue;
            }
            if(hand.squeeze<.35f) { released|=held_[h]>=0;held_[h]=-1;armed_[h]=true; }
            const bool held_before=held_[h]>=0;
            // A squeeze that misses keeps the hand armed: closed just before it reaches the
            // chair, it grips there (the headset run 2026-11-04 needed a second open-and-close).
            if(held_[h]<0&&armed_[h]&&hand.squeeze>.65f) {
                int k=Length(hand.position-handles[0])<Length(hand.position-handles[1])?0:1;
                if(held_[1-h]!=k&&Length(hand.position-handles[k])<.14f) {
                    held_[h]=k;offset_[h]=hand.position-handles[k];
                    armed_[h]=false;
                }
            }
            if(held_[h]<0)continue;
            grip_at[h]=handles[held_[h]]+offset_[h];
            hand_at[h]=hand.position;
            const Vec pull=hand.position-grip_at[h];
            if(!Finite(pull)||Length(pull)>GripStretch) { lost=true;held_[h]=-1;continue; }
            error=error+pull;++count;
            hands_mid=hands_mid+hand.position;grip_mid=grip_mid+grip_at[h];
            was_mid=was_mid+(held_before?last_hand_[h]:hand.position);
            if(held_before) {
                const float f=Dot(hand.position-last_hand_[h],forward);
                if(std::isfinite(f)) { moved_f+=f;++fed; }
                carried&=Length(hand.position-last_hand_[h])>=SnapMove;
            }
            last_hand_[h]=hand.position;
            const Vec arm=grip_at[h]-axle;
            arm_r+=Dot(arm,right);arm_f+=Dot(arm,forward);
            // The hand's own turn counts from where it took hold.
            const float flat=std::hypot(Dot(hand.facing,right),Dot(hand.facing,forward));
            const bool facing=std::isfinite(flat)&&flat>.5f;
            if(!held_before||!wrist_ok_[h]) {
                wrist_base_[h]=facing?AngleIn(hand.facing,right,forward):0.0f;
                last_wrist_[h]=hand.facing;
            }
            wrist_ok_[h]=facing;
        }
        if(count<2)both_=false;
        if(!count) {
            if(released && !lost && moved && was_held_ && shove_time_>=.075f)
                speed_=std::clamp(observed_speed,-CoastSpeed,CoastSpeed);
            if(released && !lost && was_held_ && accepted && std::abs(turn_rate_)>.4f)
                spin_=std::clamp(turn_rate_,-SpinSeed,SpinSeed);
            shove_time_=turn_rate_=0;
            if(lost) { speed_=spin_=0;return out; }
            if(speed_!=0) {
                const float sign=std::copysign(1.f,speed_);
                // Rolling resistance brings the chair to rest on flat ground. A gentle
                // incline accelerates it along its wheels, with no spontaneous roll from rest.
                const float magnitude=std::clamp(std::abs(speed_)-float(dt)*(.55f+7.f*grade_*sign),
                                                 0.f,CoastSpeed);
                speed_=magnitude>.025f ? sign*magnitude : 0.f;
            }
            if(spin_!=0) {
                const float magnitude=std::abs(spin_)-float(dt)*SpinDrag;
                spin_=magnitude>.05f ? std::copysign(magnitude,spin_) : 0.f;
            }
            out.yaw=spin_*float(dt);
            out.delta=Rolled(right,forward,speed_*float(dt),out.yaw);
            return out;
        }
        speed_=spin_=0; // catching either handle takes control immediately
        out.held=true;
        const float gain=FollowGain(dt),n=float(count);
        hands_mid=hands_mid*(1/n);was_mid=was_mid*(1/n);grip_mid=grip_mid*(1/n);
        const float gap_f=Dot(error,forward)/n,gap_r=Dot(error,right)/n;
        arm_r/=n;arm_f/=n;
        // The hold: how far it is turned from the chair (`hold`) and its turn this step (`ff`).
        float hold=0,ff=0,follow=gain;
        if(count==2) {
            const Vec l=hand_at[1]-hand_at[0],g=grip_at[1]-grip_at[0];
            const float lr=Dot(l,right),lf=Dot(l,forward),gr=Dot(g,right),gf=Dot(g,forward);
            if(lr*lr+lf*lf>.01f&&gr*gr+gf*gf>.01f) {
                const float line=std::atan2(lr,lf),grips=std::atan2(gr,gf);
                if(!both_) { both_=true;line_base_=Wrap(line-grips);last_line_=l; }
                hold=Wrap(line-grips-line_base_);
                ff=Wrap(line-AngleIn(last_line_,right,forward));
                carried&=std::abs(std::sqrt(lr*lr+lf*lf)-std::hypot(Dot(last_line_,right),
                                                                     Dot(last_line_,forward)))<.03f;
                last_line_=l;
            }
            // Each wrist starts over from here if the other hand lets go.
            for(int h=0;h<2;++h) {
                if(!wrist_ok_[h])continue;
                wrist_base_[h]=AngleIn(hands[h].facing,right,forward);
                last_wrist_[h]=hands[h].facing;
            }
        } else {
            const int h=held_[0]>=0?0:1;
            if(wrist_ok_[h]) {
                const float wrist=AngleIn(hands[h].facing,right,forward);
                hold=Wrap(wrist-wrist_base_[h]);
                ff=Wrap(wrist-AngleIn(last_wrist_[h],right,forward));
                last_wrist_[h]=hands[h].facing;
                follow=1.0f-std::exp(-float(dt)/WristS);
            }
        }
        if(std::abs(ff)>=SnapTurn&&fed==count&&carried) {
            // A snap turn: the chair goes with the hands, turned and moved as they were.
            const Vec from=root-was_mid;
            Vec to=hands_mid+Turned(Dot(from,right),Dot(from,forward),ff,right,forward)-root;
            to.y=0;
            out.yaw=std::clamp(ff,-.8f,.8f);
            out.delta=to;
            carried_=true;
            return out;
        }
        const float hands_moved=fed?std::clamp(moved_f/fed,-float(dt)*MaxSpeed,float(dt)*MaxSpeed):0.0f;
        // Never past where the hands are heading: a hand the chair itself shoves (the player
        // pushed by its collision) cannot run it away.
        const float reach=gap_f+hands_moved;
        const float travel=std::clamp(std::clamp(hands_moved+gap_f*gain,std::min(0.0f,reach),
                                                 std::max(0.0f,reach)),
                                      -float(dt)*MaxSpeed,float(dt)*MaxSpeed);
        const float speed=std::abs(travel)/float(dt);
        const float walk=std::clamp(speed/WalkSpeed,0.0f,1.0f);
        float turn=ff;
        // Standing: on its axle with the hold, while the hands have room; its back swings under
        // hands moved sideways.
        if(walk<1) {
            const float room=std::clamp((TwistRoom-Length(error*(1/n)))/.1f,0.0f,1.0f);
            const float swing=arm_f*gap_r/(arm_f*arm_f+.01f);
            turn+=(1-walk)*(room*follow*hold+(count==2?SwingTwo:1.0f)*gain*swing);
        }
        // Rolling: toward the point ahead on the hold's way, given some of the chair's turn.
        if(walk>0&&travel!=0) {
            const float aim=hold;
            const Vec to_axle=axle-grip_mid;
            const Vec target=hands_mid+Turned(Dot(to_axle,right),Dot(to_axle,forward),aim,right,forward);
            const float roll=std::copysign(1.0f,travel);
            const Vec ahead=target+Turned(0,roll*std::max(LookMin,speed*LookS),aim,right,forward)-axle;
            const float a=roll*Dot(ahead,forward),r=roll*Dot(ahead,right);
            const float distance=std::hypot(a,r);
            float pursue=distance>.05f?std::abs(travel)*2.0f*(r/distance)/distance:0.0f;
            if(!std::isfinite(pursue))pursue=0;
            turn+=walk*pursue;
        }
        if(!std::isfinite(turn))turn=0;
        turn=std::clamp(turn,-float(dt)*MaxTurn,float(dt)*MaxTurn);
        // The grips' sideways gap once it has turned: past SkidFrom its wheels skid.
        const float side=gap_r-turn*arm_f;
        float skid=0;
        if(std::abs(side)>SkidFrom&&std::isfinite(side))
            skid=std::clamp((side-std::copysign(SkidFrom,side))*gain,-float(dt)*SkidSpeed,
                            float(dt)*SkidSpeed);
        out.yaw=turn;
        // Off-centre grips keep their place along it as it turns.
        out.delta=Rolled(right,forward,travel+turn*arm_r,turn)+right*skid;
        return out;
    }
    void Accepted(Vec before,float requested,float expected_y,float grade,bool held,float turn=0) {
        before_=before;requested_=requested;expected_y_=expected_y;grade_=grade;was_held_=held;
        requested_turn_=std::isfinite(turn)?turn:0.0f;
        pending_=step_seconds_>0&&Finite(before)&&std::isfinite(requested)&&std::isfinite(grade);
    }
    void Release() {
        held_={-1,-1};armed_={false,false};pending_=false;
        speed_=spin_=shove_time_=turn_rate_=0;both_=false;wrist_ok_={false,false};
    }
    bool Holding(unsigned hand) const { return hand<held_.size() && held_[hand]>=0; }
private:
    // The root's step: the axle rolled `travel` along its axis, the chair turned `yaw` on it.
    static Vec Rolled(Vec right,Vec forward,float travel,float yaw) {
        const float a=-AxleForward; // the root, along its axis from the axle
        return forward*travel+right*(a*std::sin(yaw))+forward*(a*(std::cos(yaw)-1));
    }
    double last_{};
    Vec before_{};
    float requested_{},requested_turn_{},expected_y_{},grade_{},speed_{},spin_{},turn_rate_{},
        shove_time_{},shove_direction_{},step_seconds_{};
    bool pending_{},was_held_{},both_{},carried_{};
    float line_base_{};
    Vec last_line_{};
    std::array<int,2> held_{-1,-1};
    std::array<bool,2> armed_{},wrist_ok_{};
    std::array<float,2> wrist_base_{};
    std::array<Vec,2> offset_{}, last_hand_{}, last_wrist_{};
};
// Count only movement observed after an accepted native move. Teleports and shaking do not count.
class Ride {
    static constexpr float GiftDistanceMetres = 10.0f;
public:
    // Pushed at least this long too (8 s until 2026-11-05: the headset run 12 rode 10 m in under
    // 8 s at a run and let go, no gift).
    static constexpr double GiftSeconds = 4.0;
    void Expect(Vec before,Vec after,double now) {
        pending_=Finite(before)&&Finite(after)&&std::isfinite(now);
        before_=before;expected_=after;at_=now;
    }
    void Observe(Vec actual,double now,bool held) {
        if(!pending_)return;
        pending_=false;
        // What the game carried out, near the asked step (a warp a frame late included: the
        // ride never counted in the headset run 9).
        if(!held||!Finite(actual)||!std::isfinite(now)||now<=at_||now-at_>.1)return;
        const Vec travelled=actual-before_;
        const Vec requested=expected_-before_;
        const float length=std::hypot(requested.x,requested.z);
        if(!std::isfinite(length)||length<.00001f||length>.25f)return;
        const float along=(travelled.x*requested.x+travelled.z*requested.z)/length;
        const float across=std::abs(travelled.x*requested.z-travelled.z*requested.x)/length;
        // Count only forward progress along the accepted command, never reverse motion,
        // unrelated drift or distance beyond what we requested. Slow rolls still count.
        if(!std::isfinite(along)||!std::isfinite(across)||along<.00001f||
           along>1.25f*length+.005f||across>.005f+.25f*length)return;
        metres_=std::min(GiftDistanceMetres,metres_+std::min(along,1.25f*length));
        seconds_+=std::min(.05,now-at_);
    }
    bool Earned() const { return metres_>=GiftDistanceMetres&&seconds_>=GiftSeconds; }
    float Metres() const { return metres_; }
    double Seconds() const { return seconds_; }
    void CancelPending() { pending_=false; }
private:
    Vec before_{},expected_{};double at_{},seconds_{};float metres_{};bool pending_{};
};
// A level move `way` (unit) whose sweep already starts touching or inside scenery (the hit's
// `normal`, pointing out of the surface): it may go out of or along the surface, never further in
// (2026-11-06, a review: any direction was let through, so a push could drive the chair deeper).
// A normal with no usable level part (a floor, a ceiling, an unreadable one) lets it go, as before.
inline bool StartInsideMoveOk(Vec way, Vec normal) {
    const float level = std::hypot(normal.x, normal.z);
    if (!Finite(normal) || !(level > .2f)) return true;
    return way.x*normal.x + way.z*normal.z >= -.1f*level; // at most ~6 degrees into it
}
}
