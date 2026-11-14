#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace surfxr::transport::desktop {
// Ownership persists through inactivity. Each input thread owns its motion
// accumulator; only the ownership epoch, activity and policy cross threads.
class PointerArbiter {
public:
    using Clock=std::chrono::steady_clock;
    bool pc_moved(double dx,double dy,Clock::time_point now){
        motion(Pc,dx,dy,now,pc_motion_);
        return (owner_.load()&mask)!=Vr;
    }
    void prefer_pc(bool value){prefer_pc_.store(value);}
    void reset(){
        positioned_=false;vr_.store(unseen);
        // Releases and monitor/desktop changes invalidate pending motion, but
        // cannot give a stationary VR ray back the PC's cursor.
        owner_.fetch_add(epoch);
    }
    bool sample(double x,double y,Clock::time_point now){
        const bool first=!positioned_;
        const auto dx=first?0:x-x_,dy=first?0:y-y_;
        x_=x;y_=y;positioned_=true;
        if(first){
            vr_motion_={};
            auto state=owner_.load();
            if((state&mask)==None)owner_.compare_exchange_strong(state,next(state,Vr));
            const bool allowed=allows();
            if(allowed)vr_.store(ticks(now));
            return allowed;
        }
        return motion(Vr,dx,dy,now,vr_motion_)&&allows();
    }
    bool allows()const{return (owner_.load()&mask)!=Pc;}
private:
    enum Owner:std::uint64_t {None=0,Pc=1,Vr=2};
    static constexpr std::uint64_t mask=3,epoch=4;
    static constexpr std::int64_t activity_window=150'000'000;
    static constexpr auto unseen=std::numeric_limits<std::int64_t>::min();
    struct Motion {
        std::uint64_t state=0;
        std::int64_t time=unseen;
        double x=0,y=0;
    };
    static std::uint64_t next(std::uint64_t state,Owner owner){return ((state+epoch)&~mask)|owner;}
    static std::int64_t ticks(Clock::time_point time){
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }
    bool motion(Owner source,double dx,double dy,Clock::time_point now,Motion& pending){
        const auto time=ticks(now);auto state=owner_.load();
        if(pending.state!=state||pending.time==unseen||time-pending.time>=activity_window){
            pending.x=0;pending.y=0;
        }
        pending.state=state;
        if(dx!=0||dy!=0)pending.time=time;
        pending.x+=dx;pending.y+=dy;
        const auto owner=state&mask;
        // Keep normal pointer precision. Require a deliberate 12-pixel net
        // displacement to challenge an established owner, not summed jitter.
        const double threshold=owner==source||owner==None?2:12;
        if(pending.x*pending.x+pending.y*pending.y<threshold*threshold)return false;
        pending.x=0;pending.y=0;
        (source==Pc?pc_:vr_).store(time);
        if(owner==source)return true;
        const auto other=(source==Pc?vr_:pc_).load();
        const bool simultaneous=other!=unseen&&time-other<activity_window;
        if(owner!=None&&simultaneous&&prefer_pc_.load()!=(source==Pc))return false;
        // A losing motion sample is consumed now; it cannot become a delayed
        // takeover just because the current owner's activity expires.
        return owner_.compare_exchange_strong(state,next(state,source));
    }
    std::atomic<std::uint64_t> owner_{None};
    std::atomic<std::int64_t> pc_{unseen},vr_{unseen};
    std::atomic_bool prefer_pc_{false};
    Motion pc_motion_,vr_motion_;
    double x_=0,y_=0;bool positioned_=false;
};
}
