#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace surfxr {
struct TrackingScheduleStats {
    std::uint64_t packets=0,missed_deadlines=0,interval_samples=0;
    std::uint64_t interval_ns_min=0,interval_ns_max=0;
    double interval_ns_mean=0;
    std::uint64_t fresh_pose_packets=0,repeated_pose_packets=0,invalid_pose_packets=0;
    std::uint64_t first_sent_ns=0,last_sent_ns=0;
};

// A single-producer schedule anchored to absolute monotonic nanoseconds.
// Rational ticks avoid accumulating rounded periods or dispatch lateness.
// Missed ticks are skipped: callers send only one current sample, never a burst.
class TrackingSchedule {
public:
    static bool valid_rate(unsigned hz) noexcept {
        switch(hz){case 24:case 30:case 60:case 72:case 80:case 90:
                   case 96:case 100:case 120:return true;default:return false;}
    }
    void set_rate(unsigned hz,std::uint64_t now_ns) {
        if(!valid_rate(hz))throw std::invalid_argument("invalid_tracking_schedule_rate");
        if(hz==hz_)return;
        hz_=hz;anchor_ns_=now_ns;
        // Initial connection can send immediately. A rate change waits for one
        // new period so it cannot cause an extra back-to-back packet.
        next_tick_=stats_.packets?1:0;
    }
    unsigned rate() const noexcept{return hz_;}
    std::uint64_t deadline_ns() const noexcept {
        if(!hz_)return 0;
        return anchor_ns_+(next_tick_/hz_)*1000000000ULL
            +((next_tick_%hz_)*1000000000ULL+hz_-1)/hz_;
    }
    bool due(std::uint64_t now_ns) const noexcept{return hz_&&now_ns>=deadline_ns();}
    int poll_timeout_ms(std::uint64_t now_ns,int usual_ms) const noexcept {
        if(usual_ms<=0||!hz_)return std::max(0,usual_ms);
        const auto deadline=deadline_ns();
        if(now_ns>=deadline)return 0;
        const auto remaining=deadline-now_ns;
        if(remaining>=static_cast<std::uint64_t>(usual_ms)*1000000ULL)return usual_ms;
        // Round up to avoid a sub-millisecond busy loop. Absolute deadlines
        // retain fractional milliseconds across polls and subsequent packets.
        return static_cast<int>((remaining+999999ULL)/1000000ULL);
    }
    void sent(std::uint64_t now_ns,std::uint64_t sequence,std::uint64_t origin_epoch,bool valid_pose) {
        if(!due(now_ns)||(stats_.packets&&now_ns<stats_.last_sent_ns))
            throw std::logic_error("tracking_schedule_send_before_deadline");
        const auto elapsed=now_ns-anchor_ns_;
        const auto last_due=(elapsed/1000000000ULL)*hz_+(elapsed%1000000000ULL)*hz_/1000000000ULL;
        stats_.missed_deadlines+=last_due-next_tick_;
        next_tick_=last_due+1;
        if(stats_.packets){
            const auto interval=now_ns-stats_.last_sent_ns;
            const auto samples=++stats_.interval_samples;
            if(samples==1)stats_.interval_ns_min=interval;
            stats_.interval_ns_min=std::min(stats_.interval_ns_min,interval);
            stats_.interval_ns_max=std::max(stats_.interval_ns_max,interval);
            stats_.interval_ns_mean+=(static_cast<double>(interval)-stats_.interval_ns_mean)/static_cast<double>(samples);
        }else stats_.first_sent_ns=now_ns;
        ++stats_.packets;stats_.last_sent_ns=now_ns;
        if(!valid_pose)++stats_.invalid_pose_packets;
        else if(have_pose_&&sequence==last_sequence_&&origin_epoch==last_epoch_)++stats_.repeated_pose_packets;
        else{++stats_.fresh_pose_packets;last_sequence_=sequence;last_epoch_=origin_epoch;have_pose_=true;}
    }
    const TrackingScheduleStats& statistics() const noexcept{return stats_;}
private:
    unsigned hz_=0;
    std::uint64_t anchor_ns_=0,next_tick_=0,last_sequence_=0,last_epoch_=0;
    bool have_pose_=false;
    TrackingScheduleStats stats_;
};
} // namespace surfxr
