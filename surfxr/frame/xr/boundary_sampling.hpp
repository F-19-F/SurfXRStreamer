#pragma once
#include <cstdint>
#include <array>
#include <cmath>

namespace surfxr::xr_detail {
// Compare to the last sampled origin, so small real corrections accumulate.
// Frame reconstructs this matrix with sub-micrometer float noise on cache flush.
inline bool boundary_origin_changed(const std::array<float,12>& a,const std::array<float,12>& b){
    for(unsigned i=0;i<12;++i)
        if(!std::isfinite(a[i])||!std::isfinite(b[i])||std::abs(a[i]-b[i])>(i%4==3?1e-4f:1e-5f))return true;
    return false;
}
// A predicted display time may enter the new Stage before OpenVR's current
// boundary has changed. Only sample once runtime "now" is in this epoch.
inline bool boundary_time_ready(std::int64_t now,std::int64_t begin,std::int64_t next){
    // Suspend as soon as a future change is announced: OpenVR has no timed
    // boundary query, so its notification may already expose the new geometry.
    return now>0&&now>=begin&&!next;
}
class BoundarySampling {
public:
    bool update(std::uint64_t epoch,bool ready){
        if(epoch==epoch_&&ready==ready_)return false;
        epoch_=epoch;ready_=ready;++generation_;return true;
    }
    bool ready()const{return ready_;}
    std::uint64_t generation()const{return generation_;}
    bool accepts(std::uint64_t generation)const{return ready_&&generation==generation_;}
private:
    std::uint64_t epoch_=0,generation_=0;
    bool ready_=false;
};
}
