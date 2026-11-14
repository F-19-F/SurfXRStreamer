#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace surfxr {
// Bounded two-second window on the headset's monotonic clock. Source metadata
// timestamps from the PC are identities only, never a clock to subtract from.
class PerformanceOverlay {
public:
    struct Summary {double mean=0,p95=0,variance=0,stddev=0;unsigned count=0;};
    enum Metric {NewFrame,Submit,PoseToDisplay,DecodeToDisplay,Count};
    struct Sample {std::uint64_t time=0;std::array<double,Count> values{-1,-1,-1,-1};};
    void reset(){*this=PerformanceOverlay{};}
    void observe(std::uint64_t now,std::uint64_t frame_id,std::uint64_t pose,
                 std::uint64_t decoded,std::uint64_t predicted_display,std::uint64_t origin_epoch=0){
        if(last_submit_&&(origin_epoch!=last_epoch_||now<=last_submit_||now-last_submit_>2'000'000'000ull))reset();
        Sample sample;sample.time=now;
        if(last_submit_)sample.values[Submit]=double(now-last_submit_)/1e6;
        if(!last_new_||frame_id!=last_id_){
            if(last_new_)sample.values[NewFrame]=double(now-last_new_)/1e6;
            last_new_=now;last_id_=frame_id;
        }
        // Missing/invalid prediction or source times stay unavailable, not 0ms.
        if(predicted_display&&pose&&predicted_display>=pose)
            sample.values[PoseToDisplay]=double(predicted_display-pose)/1e6;
        if(predicted_display&&decoded&&predicted_display>=decoded)
            sample.values[DecodeToDisplay]=double(predicted_display-decoded)/1e6;
        samples_[next_]=sample;next_=(next_+1)%samples_.size();last_submit_=now;last_epoch_=origin_epoch;
    }
    Summary summary(Metric metric,std::uint64_t now)const {
        std::array<double,512> values{};Summary result;
        for(const auto& item:samples_)if(item.time&&now>=item.time&&now-item.time<=2'000'000'000ull){
            const auto value=item.values[metric];
            if(value>=0&&std::isfinite(value)){values[result.count++]=value;result.mean+=value;}
        }
        if(result.count){result.mean/=result.count;std::sort(values.begin(),values.begin()+result.count);
            result.p95=values[(result.count*95+99)/100-1];
            for(unsigned i=0;i<result.count;++i){const auto delta=values[i]-result.mean;result.variance+=delta*delta;}
            result.variance/=result.count;result.stddev=std::sqrt(result.variance);}
        return result;
    }
private:
    std::array<Sample,512> samples_{};std::size_t next_=0;
    std::uint64_t last_submit_=0,last_new_=0,last_id_=0,last_epoch_=0;
};
} // namespace surfxr
