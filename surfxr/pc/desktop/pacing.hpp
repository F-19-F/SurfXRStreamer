#pragma once
#include "network.hpp"

namespace surfxr::transport::desktop {
// A hidden service cannot depend on another process's multimedia timer period.
// Keep packet and frame deadlines on a per-thread high-resolution kernel timer.
class VideoDeadline {
public:
    VideoDeadline():timer_(CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE)){
        if(!timer_.value)timer_.value=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
        require(timer_.value,"desktop_video_timer_create");
    }
    void wait(Clock::time_point deadline){
        const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-Clock::now()).count();
        if(remaining<=0)return;
        LARGE_INTEGER due{};due.QuadPart=-(remaining+99)/100;
        require(SetWaitableTimer(timer_.value,&due,0,nullptr,nullptr,FALSE),"desktop_video_timer_arm");
        require(WaitForSingleObject(timer_.value,1000)==WAIT_OBJECT_0,"desktop_video_timer_wait");
    }
private:
    Handle timer_;
};
}
