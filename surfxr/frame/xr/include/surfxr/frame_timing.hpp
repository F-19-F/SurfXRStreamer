#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <locale>
#include <ostream>

namespace surfxr {
// Numeric timing/input state only: no poses, pixels, text or device identifiers.
// *_ns uses steady_clock/CLOCK_MONOTONIC; *_xr_time and predicted_* are
// OpenXR's time domain. The matched pose query supplies a clock-pair anchor.
struct FrameTimingRecord {
    std::uint64_t input_sample_ns=0,system_sample_ns=0,shortcut_reset_serial=0;
    unsigned shortcut_active=0,shortcut_down=0,shortcut_keys=0,system_available=0,system_clicks=0,system_capture=0;
    unsigned input_profile_left=0,input_profile_right=0;
    bool input_focused=false,shortcut_guard=false,shortcut_toggle=false,requested_vr=false,presented_vr=false;
    std::uint64_t frame_index=0,wait_return_ns=0,before_end_ns=0,after_end_ns=0;
    std::uint64_t after_input_ns=0,after_stage_ns=0,after_render_ns=0;
    std::int64_t predicted_display_time=0,predicted_display_period=0;
    std::uint64_t source_timestamp_us=0,origin_epoch=0,pair_ready_ns=0;
    std::uint64_t decoded_left_ns=0,decoded_right_ns=0,pose_query_ns=0;
    std::uint64_t packet_left_ns=0,packet_right_ns=0,conversion_left_ns=0,conversion_right_ns=0;
    std::uint64_t decoded_focus_left_ns=0,decoded_focus_right_ns=0;
    std::uint64_t packet_focus_left_ns=0,packet_focus_right_ns=0;
    std::uint64_t conversion_focus_left_ns=0,conversion_focus_right_ns=0;
    std::uint64_t predicted_display_monotonic_ns=0,video_select_ns=0,pacing_wait_ns=0;
    std::uint64_t pre_wait_pacing_ns=0;
    std::int64_t pose_query_xr_time=0,pose_target_xr_time=0;
    // Copied from the selected stereo pair's retained first-primary metadata,
    // not from each original packet header. A repeated timestamp may update
    // offset24 at encoder completion; the demux retains the first value here.
    std::int32_t retained_first_primary_metadata_tracking_frame_id=0;
    // Last raw channel-1 recv which released that first primary record. These
    // are half-open TCP byte offsets, not exact cipher/video frame boundaries.
    std::int32_t result=0;
    bool should_render=false,has_vr=false,reused=false,has_retained_first_primary_metadata=false;
    bool foveated=false,pacing_enabled=false;
};
struct FrameTimingSnapshot {
    static constexpr std::size_t capacity=512;
    bool enabled=false;
    std::uint64_t generation=0,total_recorded=0,overwritten=0;
    std::size_t count=0;
    std::array<FrameTimingRecord,capacity> records{}; // Oldest to newest.
};
// Owned and accessed only by the XR thread. Recording is fixed-size and does
// no allocation, locking or IO. Background writers receive copied snapshots.
class FrameTimingRecorder {
public:
    void set_enabled(bool value) noexcept {
        if(value==enabled_)return;
        enabled_=value;
        if(value){++generation_;count_=next_=0;total_recorded_=0;}
    }
    bool enabled() const noexcept{return enabled_;}
    void record(FrameTimingRecord value) noexcept {
        if(!enabled_)return;
        value.frame_index=++frame_index_;
        records_[next_]=value;next_=(next_+1)%FrameTimingSnapshot::capacity;
        if(count_<FrameTimingSnapshot::capacity)++count_;
        ++total_recorded_;
    }
    FrameTimingSnapshot snapshot() const noexcept {
        FrameTimingSnapshot result;
        result.enabled=enabled_;result.generation=generation_;
        result.total_recorded=total_recorded_;result.overwritten=total_recorded_-count_;result.count=count_;
        const auto first=(next_+FrameTimingSnapshot::capacity-count_)%FrameTimingSnapshot::capacity;
        for(std::size_t i=0;i<count_;++i)result.records[i]=records_[(first+i)%FrameTimingSnapshot::capacity];
        return result;
    }
private:
    std::array<FrameTimingRecord,FrameTimingSnapshot::capacity> records_{};
    std::size_t count_=0,next_=0;
    std::uint64_t generation_=0,total_recorded_=0,frame_index_=0;
    bool enabled_=false;
};
inline void write_frame_timing_json(std::ostream& out,const FrameTimingSnapshot& snapshot,
                                    std::uint64_t process_id,std::uint64_t sampled_monotonic_ns){
    const auto previous_locale=out.getloc();
    const auto previous_precision=out.precision(std::numeric_limits<float>::max_digits10);
    out.imbue(std::locale::classic());
    const auto count=snapshot.count<FrameTimingSnapshot::capacity?snapshot.count:FrameTimingSnapshot::capacity;
    out<<"{\"schema_version\":1,\"pid\":"<<process_id<<",\"sample_monotonic_ns\":"<<sampled_monotonic_ns
       <<",\"enabled\":"<<(snapshot.enabled?"true":"false")<<",\"generation\":"<<snapshot.generation
       <<",\"capacity\":"<<FrameTimingSnapshot::capacity<<",\"total_recorded\":"<<snapshot.total_recorded
       <<",\"overwritten\":"<<snapshot.overwritten<<",\"count\":"<<count<<",\"records\":[";
    for(std::size_t i=0;i<count;++i){
        if(i)out<<',';
        const auto& value=snapshot.records[i];out<<"{\"frame_index\":"<<value.frame_index;
#define SURFXR_TIMING_FIELD(name) out<<",\"" #name "\":"<<value.name
        SURFXR_TIMING_FIELD(input_sample_ns);SURFXR_TIMING_FIELD(system_sample_ns);SURFXR_TIMING_FIELD(shortcut_reset_serial);
        SURFXR_TIMING_FIELD(shortcut_active);SURFXR_TIMING_FIELD(shortcut_down);SURFXR_TIMING_FIELD(shortcut_keys);
        SURFXR_TIMING_FIELD(system_available);SURFXR_TIMING_FIELD(system_clicks);SURFXR_TIMING_FIELD(system_capture);
        SURFXR_TIMING_FIELD(input_profile_left);SURFXR_TIMING_FIELD(input_profile_right);SURFXR_TIMING_FIELD(input_focused);
        SURFXR_TIMING_FIELD(shortcut_guard);SURFXR_TIMING_FIELD(shortcut_toggle);SURFXR_TIMING_FIELD(requested_vr);SURFXR_TIMING_FIELD(presented_vr);
        SURFXR_TIMING_FIELD(wait_return_ns);SURFXR_TIMING_FIELD(before_end_ns);SURFXR_TIMING_FIELD(after_end_ns);
        SURFXR_TIMING_FIELD(after_input_ns);SURFXR_TIMING_FIELD(after_stage_ns);SURFXR_TIMING_FIELD(after_render_ns);
        SURFXR_TIMING_FIELD(predicted_display_time);SURFXR_TIMING_FIELD(predicted_display_period);
        SURFXR_TIMING_FIELD(source_timestamp_us);SURFXR_TIMING_FIELD(origin_epoch);SURFXR_TIMING_FIELD(pair_ready_ns);
        SURFXR_TIMING_FIELD(decoded_left_ns);SURFXR_TIMING_FIELD(decoded_right_ns);SURFXR_TIMING_FIELD(pose_query_ns);
        SURFXR_TIMING_FIELD(packet_left_ns);SURFXR_TIMING_FIELD(packet_right_ns);SURFXR_TIMING_FIELD(conversion_left_ns);SURFXR_TIMING_FIELD(conversion_right_ns);
        SURFXR_TIMING_FIELD(decoded_focus_left_ns);SURFXR_TIMING_FIELD(decoded_focus_right_ns);
        SURFXR_TIMING_FIELD(packet_focus_left_ns);SURFXR_TIMING_FIELD(packet_focus_right_ns);
        SURFXR_TIMING_FIELD(conversion_focus_left_ns);SURFXR_TIMING_FIELD(conversion_focus_right_ns);
        SURFXR_TIMING_FIELD(predicted_display_monotonic_ns);SURFXR_TIMING_FIELD(video_select_ns);SURFXR_TIMING_FIELD(pacing_wait_ns);
        SURFXR_TIMING_FIELD(pre_wait_pacing_ns);
        SURFXR_TIMING_FIELD(foveated);SURFXR_TIMING_FIELD(pacing_enabled);
        SURFXR_TIMING_FIELD(pose_query_xr_time);SURFXR_TIMING_FIELD(pose_target_xr_time);SURFXR_TIMING_FIELD(result);
        if(value.has_retained_first_primary_metadata){
            SURFXR_TIMING_FIELD(retained_first_primary_metadata_tracking_frame_id);
#define SURFXR_TIMING_FINITE_FIELD(name) out<<",\"" #name "\":";if(std::isfinite(value.name))out<<value.name;else out<<"null"
#undef SURFXR_TIMING_FINITE_FIELD
        }

#undef SURFXR_TIMING_FIELD
        out<<",\"should_render\":"<<(value.should_render?"true":"false")
           <<",\"has_vr\":"<<(value.has_vr?"true":"false")<<",\"reused\":"<<(value.reused?"true":"false")<<'}';
    }
    out<<"]}\n";
    out.precision(previous_precision);out.imbue(previous_locale);
}
} // namespace surfxr
