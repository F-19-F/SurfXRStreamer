#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace surfxr {
struct HapticCommand {
    bool changed=false;
    float amplitude=0;
    // A changed command with zero duration means stop.
    std::uint64_t duration_ns=0;
    float frequency=0;
};
struct HapticsStats {
    std::uint64_t metadata=0,positive_samples=0,invalid=0,duplicates=0,out_of_order=0;
    std::uint64_t coalesced=0,expired=0,discarded_disabled=0,pulses=0,stops=0,clears=0;
    std::uint64_t stale_video=0,stale_positive_video=0;
    std::uint64_t deferred_renewals=0;
};

// A fixed two-hand mailbox, not a FIFO. The receiver only ingests metadata; the
// OpenXR thread consumes commands. All clocks are the same steady-clock ns.
// Video levels expire after a bounded pulse; explicit SURFXR pulses also carry
// their negotiated duration and frequency. Focus loss stops active feedback.
class HapticsQueue {
public:
    static constexpr std::uint64_t pulse_duration_ns=30'000'000;
    static constexpr std::uint64_t freshness_ns=100'000'000;
    static constexpr std::uint64_t renewal_margin_ns=10'000'000;


    // SURFXR video metadata already contains normalized amplitudes.
    bool ingest_video_amplitudes(std::uint64_t timestamp,unsigned source,const std::array<float,2>& amplitude,std::uint64_t received_ns){
        std::lock_guard<std::mutex> lock(mutex_);
        if(source>1||!received_ns||!std::all_of(amplitude.begin(),amplitude.end(),[](float a){return std::isfinite(a)&&a>=0&&a<=1;})){
            ++stats_.invalid;return false;
        }
        return ingest_video_amplitudes_locked(timestamp,source,amplitude,received_ns,true);
    }

private:
    bool ingest_video_amplitudes_locked(std::uint64_t timestamp,unsigned source,const std::array<float,2>& amplitude,std::uint64_t received_ns,bool steady){
        std::array<bool,2> changed{true,true};
        if(have_identity_){
            // Configuration/reset is the only authority that can switch source
            // or restart the stream timestamp. Delayed old records cannot do it.
            if(source!=source_||timestamp<timestamp_){++stats_.out_of_order;return false;}
            if(timestamp==timestamp_){
                // A repeated image can still carry new feedback. Only suppress
                // the unchanged hand, otherwise a right-hand update could
                // incorrectly renew the left-hand pulse (or hide a new pulse).
                for(std::size_t hand=0;hand<2;++hand)changed[hand]=amplitude[hand]!=last_amplitude_[hand];
                if(!changed[0]&&!changed[1]){++stats_.duplicates;return false;}
            }
        }
        source_=source;timestamp_=timestamp;last_amplitude_=amplitude;have_identity_=true;++stats_.metadata;
        for(std::size_t hand=0;hand<2;++hand){
            if(!changed[hand]||amplitude[hand]<=0)continue;
            ++stats_.positive_samples;
            // Keep identity advancing while unfocused, but never retain a
            // background pulse for the first frame after focus is restored.
            if(consumed_&&!enabled_){++stats_.discarded_disabled;continue;}
            auto& state=hands_[hand];
            if(state.pending)++stats_.coalesced;
            state.pending=true;state.received_ns=received_ns;state.steady=steady;
            state.amplitude=amplitude[hand];
            state.duration_ns=pulse_duration_ns;state.frequency=0;
        }
        return true;
    }

public:
    // Legacy SURFXR supplies explicit duration/frequency independently.
    bool ingest_pulse(unsigned hand,float amplitude,std::uint64_t duration_ns,float frequency,std::uint64_t received_ns){
        std::lock_guard<std::mutex> lock(mutex_);
        if(hand>=2||!received_ns||!std::isfinite(amplitude)||amplitude<0||amplitude>1||
           !std::isfinite(frequency)||frequency<0||frequency>1000||duration_ns>5'000'000'000){++stats_.invalid;return false;}
        if(consumed_&&!enabled_){++stats_.discarded_disabled;return false;}
        auto& state=hands_[hand];if(state.pending)++stats_.coalesced;
        state.pending=amplitude>0&&duration_ns>0;state.force_stop=!state.pending;state.steady=false;
        state.amplitude=amplitude;state.duration_ns=duration_ns;state.frequency=frequency;state.received_ns=received_ns;
        return true;
    }

    // Configuration changes and connection teardown discard pending feedback.
    // Actual stop calls are deferred to consume() on the OpenXR thread. Focus
    // transitions use false to preserve identity; consume(false) also disables
    // ingestion until focus returns, preventing background pulse replay.
    void clear(bool reset_identity=true){
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto& state:hands_){state.pending=false;state.force_stop=true;}
        if(reset_identity)have_identity_=false;
        ++stats_.clears;
    }

    std::array<HapticCommand,2> consume(std::uint64_t now_ns,bool enabled){
        std::lock_guard<std::mutex> lock(mutex_);
        std::array<HapticCommand,2> result{};
        // Reject invalid/overflowing clocks without extending existing feedback.
        enabled=enabled&&now_ns>0&&now_ns<=std::numeric_limits<std::uint64_t>::max()-pulse_duration_ns;
        for(std::size_t hand=0;hand<2;++hand){
            auto& state=hands_[hand];auto& command=result[hand];
            if(!enabled){
                if(state.pending)++stats_.discarded_disabled;
                state.pending=false;
                if(!consumed_||enabled_||state.active||state.force_stop){command.changed=true;++stats_.stops;}
                state.active=false;state.force_stop=false;continue;
            }
            if(state.force_stop){
                command.changed=true;state.active=false;state.force_stop=false;++stats_.stops;continue;
            }
            if(state.pending){
                state.pending=false;
                if(state.received_ns>now_ns||now_ns-state.received_ns>=freshness_ns){++stats_.expired;}
                else{
                    auto duration=state.duration_ns;bool apply=true;
                    if(state.steady&&state.applied_steady&&state.active&&state.applied_amplitude==state.amplitude){
                        // Video metadata describes a level, not a new event
                        // every 8ms. xrApply cancels incomplete output: allow
                        // the current pulse to run before renewing its tail.
                        const auto end=state.received_ns+pulse_duration_ns;
                        if(end<=state.active_until_ns||end<=now_ns)apply=false;
                        else if(state.active_until_ns>now_ns&&state.active_until_ns-now_ns>renewal_margin_ns){
                            state.pending=true;++stats_.deferred_renewals;continue;
                        }else duration=end-now_ns;
                    }
                    if(apply){
                        command={true,state.amplitude,duration,state.frequency};
                        state.active=true;state.active_until_ns=now_ns+duration;
                        state.applied_amplitude=state.amplitude;state.applied_steady=state.steady;
                        state.force_stop=false;++stats_.pulses;continue;
                    }
                }
            }
            if(state.active&&now_ns>=state.active_until_ns){
                command.changed=true;state.active=false;state.force_stop=false;++stats_.stops;
            }
        }
        consumed_=true;enabled_=enabled;
        return result;
    }

    HapticsStats statistics() const{std::lock_guard<std::mutex> lock(mutex_);return stats_;}
    void reject_stale_video(bool positive){
        std::lock_guard<std::mutex> lock(mutex_);++stats_.stale_video;
        if(positive)++stats_.stale_positive_video;
    }
private:
    struct HandState {
        bool pending=false,active=false,force_stop=false,steady=false,applied_steady=false;
        float amplitude=0,applied_amplitude=0;
        std::uint64_t received_ns=0,active_until_ns=0;
        std::uint64_t duration_ns=pulse_duration_ns;float frequency=0;
    };
    mutable std::mutex mutex_;
    std::array<HandState,2> hands_{};
    HapticsStats stats_{};
    bool have_identity_=false,consumed_=false,enabled_=false;
    std::array<float,2> last_amplitude_{};
    std::uint8_t source_=0;
    std::uint64_t timestamp_=0;
};
} // namespace surfxr
