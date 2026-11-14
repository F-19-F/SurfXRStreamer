#pragma once
#include "socket.hpp"

namespace surfxr::transport {
// A retry budget limits establishment, never a successfully restored stream.
// Priority ingestion may observe the transition, so completion is atomic.
class SessionDeadline {
public:
    SessionDeadline(Clock::time_point session,Clock::time_point recovery):session_(session),recovery_(recovery){}
    bool expired(Clock::time_point now,bool startup_pending=false)const{
        return now>=session_||(!startup_pending&&!connected_.load(std::memory_order_relaxed)&&now>=recovery_);
    }
    void connected(){connected_.store(true,std::memory_order_relaxed);}
private:
    const Clock::time_point session_,recovery_;
    std::atomic_bool connected_{false};
};
}
