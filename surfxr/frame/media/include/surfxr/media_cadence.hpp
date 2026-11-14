#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace surfxr {
struct MediaCadenceStats {
    std::uint64_t packets=0,intervals=0,receive_interval_ns_sum=0,receive_interval_ns_max=0;
    std::uint64_t source_intervals=0,source_delta_raw_sum=0,source_delta_raw_max=0;
    std::uint64_t source_repeated=0,source_regressions=0,overflow_intervals=0;
};
// Packet completion cadence, not kernel arrival time or headset presentation.
// Reset the previous sample on every media configuration, including a switch
// back to desktop, so menu/mode/reconfiguration gaps do not depress the rate.
// Source ticks remain raw until their clock frequency is independently known.
class MediaCadence {
public:
    void configuration() noexcept {have_previous_=false;}
    void observe(std::uint64_t timestamp,std::uint64_t received_ns) noexcept {
        ++stats_.packets;
        if(have_previous_){
            if(received_ns>=previous_received_){
                const auto delta=received_ns-previous_received_;
                if(delta<=std::numeric_limits<std::uint64_t>::max()-stats_.receive_interval_ns_sum){
                    ++stats_.intervals;stats_.receive_interval_ns_sum+=delta;
                    stats_.receive_interval_ns_max=std::max(stats_.receive_interval_ns_max,delta);
                }else ++stats_.overflow_intervals;
            }
            if(timestamp>previous_timestamp_){
                const auto delta=timestamp-previous_timestamp_;
                if(delta<=std::numeric_limits<std::uint64_t>::max()-stats_.source_delta_raw_sum){
                    ++stats_.source_intervals;stats_.source_delta_raw_sum+=delta;
                    stats_.source_delta_raw_max=std::max(stats_.source_delta_raw_max,delta);
                }else ++stats_.overflow_intervals;
            }else if(timestamp==previous_timestamp_)++stats_.source_repeated;
            else ++stats_.source_regressions;
        }
        previous_timestamp_=timestamp;previous_received_=received_ns;have_previous_=true;
    }
    const MediaCadenceStats& statistics() const noexcept{return stats_;}
private:
    bool have_previous_=false;
    std::uint64_t previous_timestamp_=0,previous_received_=0;
    MediaCadenceStats stats_;
};
} // namespace surfxr
