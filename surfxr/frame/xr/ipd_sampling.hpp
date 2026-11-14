#pragma once
#include <cmath>
#include <cstdint>

namespace surfxr::xr_detail {
// Event-driven reads with a slow watchdog for missed runtime notifications.
// The same cache bounds the XR fallback when the OpenVR bridge is unavailable.
class IpdSampling {
    std::uint64_t interval_,sampled_at_=0;
    bool sampled_=false;
    float value_=0;
public:
    explicit IpdSampling(std::uint64_t interval):interval_(interval){}
    bool due(std::uint64_t now,bool event=false)const{
        return event||!sampled_||now-sampled_at_>=interval_;
    }
    void sampled(float value,std::uint64_t now){
        value_=std::isfinite(value)&&value>0&&value<.2f?value:0;
        sampled_at_=now;sampled_=true;
    }
    float value()const{return value_;}
};
}
