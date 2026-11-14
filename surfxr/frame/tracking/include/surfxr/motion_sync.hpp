#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace surfxr {
struct MotionSyncTarget {
    bool valid=false;
    std::int64_t xr_time=0;
    float lead_seconds=0;
};
struct MotionSyncStats {
    double latency_seconds=0,previous_draw_seconds=0,last_sample_seconds=0;
    std::uint64_t updates=0,rejected_invalid_time=0,rejected_epoch=0;
    std::uint64_t rejected_duplicate=0,rejected_out_of_order=0,epoch_resets=0;
};

// A future locate may not cross a pending reference-space change, and a
// freshly selected epoch may not label a query from before its own boundary.
// Zero boundaries mean that side of the interval is not yet constrained.
inline bool motion_target_in_epoch(std::int64_t target,std::int64_t epoch_begin,
                                   std::int64_t next_change) noexcept {
    return target>0&&(epoch_begin==0||target>=epoch_begin)&&
           (next_change==0||target<next_change);
}

// XR-thread-owned feedback estimator. These are local query/draw/display durations,
// not measurements of photon latency. XrTime and steady-clock nanoseconds are
// separate domains: only target() does arithmetic on XrTime. The returned
// image's remote timestamp is an identity, never a local-clock measurement.
class MotionSync {
public:
    static constexpr double smoothing_weight=.02;
    static constexpr double maximum_sample_seconds=.999;
    static constexpr double maximum_lead_seconds=.066;

    MotionSyncTarget target(std::int64_t query_xr_time) const noexcept {
        if(query_xr_time<=0)return {};
        const double lead=std::min(stats_.latency_seconds,maximum_lead_seconds);
        if(!std::isfinite(lead)||lead<0)return {};
        const auto offset=static_cast<std::int64_t>(std::llround(lead*1'000'000'000.0));
        if(query_xr_time>std::numeric_limits<std::int64_t>::max()-offset)return {};
        return {true,query_xr_time+offset,static_cast<float>(double(offset)/1'000'000'000.0)};
    }

    // Recenter/reference-space changes invalidate capture identities and the
    // preceding draw duration, but do not erase the learned network/render age.
    // Calling this repeatedly with the current epoch is a no-op.
    void set_epoch(std::uint64_t epoch) noexcept {
        if(epoch_==epoch)return;
        epoch_=epoch;have_timestamp_=false;last_timestamp_=0;
        stats_.previous_draw_seconds=0;++stats_.epoch_resets;
    }

    // SURFXR predicts on the client (head velocities on the wire are zero).
    // Include the local compositor's remaining wait, using this image's first
    // successful submission and converted predicted display time. GPU work is
    // already inside this interval; a later fence retirement is not its end.
    // These are estimated display times, never measured photon latency.
    bool observe_display_frame(std::uint64_t epoch,std::uint64_t source_timestamp,
                               std::uint64_t query_ns,std::uint64_t submitted_ns,
                               std::uint64_t display_ns) noexcept {
        if(!query_ns||submitted_ns<query_ns||display_ns<submitted_ns||
           display_ns-query_ns>999'000'000){
            ++stats_.rejected_invalid_time;return false;
        }
        return observe_sample(epoch,source_timestamp,double(display_ns-query_ns)/1'000'000'000.0,0);
    }
    const MotionSyncStats& statistics() const noexcept{return stats_;}
private:
    bool observe_sample(std::uint64_t epoch,std::uint64_t source_timestamp,
                        double sample,double draw_seconds) noexcept {
        if(!epoch_||epoch!=epoch_){++stats_.rejected_epoch;return false;}
        if(have_timestamp_&&source_timestamp==last_timestamp_){++stats_.rejected_duplicate;return false;}
        if(have_timestamp_&&source_timestamp<last_timestamp_){++stats_.rejected_out_of_order;return false;}
        sample=std::min(maximum_sample_seconds,sample);
        stats_.latency_seconds+=smoothing_weight*(sample-stats_.latency_seconds);
        stats_.last_sample_seconds=sample;
        stats_.previous_draw_seconds=draw_seconds;
        last_timestamp_=source_timestamp;have_timestamp_=true;++stats_.updates;
        return true;
    }
    std::uint64_t epoch_=0,last_timestamp_=0;
    bool have_timestamp_=false;
    MotionSyncStats stats_;
};
} // namespace surfxr
