#pragma once
#include "network.hpp"
#include "diagnostics.hpp"
#include <array>

namespace surfxr::transport::desktop {
// Bounded numeric timing only. Never includes coordinates, input, media or keys.
class VideoMetrics {
public:
    explicit VideoMetrics(const char* code):code_(code){}
    void reset(unsigned generation){generation_=generation;start_=Clock::now();frames_={};times_={};batches_=0;}
    void frame(unsigned id){++frames_[id];}
    void time(unsigned stage,Clock::time_point start){times_[stage]+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
    void batch(){
        ++batches_;const auto seconds=std::chrono::duration<double>(Clock::now()-start_).count();if(seconds<5)return;
        char detail[384]{};
        std::snprintf(detail,sizeof(detail),"{\"generation\":%u,\"seconds\":%.3f,\"frames\":[%llu,%llu,%llu,%llu],\"batches\":%llu,\"stage_ms\":[%.3f,%.3f,%.3f]}",
            generation_,seconds,static_cast<unsigned long long>(frames_[0]),static_cast<unsigned long long>(frames_[1]),static_cast<unsigned long long>(frames_[2]),static_cast<unsigned long long>(frames_[3]),static_cast<unsigned long long>(batches_),times_[0],times_[1],times_[2]);
        report_event(code_,0,EVENTLOG_INFORMATION_TYPE,detail);reset(generation_);
    }
private:
    const char* code_;unsigned generation_=0;Clock::time_point start_=Clock::now();
    std::array<std::uint64_t,4> frames_{};std::array<double,3> times_{};std::uint64_t batches_=0;
};
}
