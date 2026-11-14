#include "surfxr/surfxr_feedback.hpp"
#include "surfxr/vr_frame.hpp"
#include <algorithm>

namespace surfxr::transport {
void FeedbackQueue::presented(const VrStereoFrame& f,std::uint64_t now){
    if(!f.metadata||(!f.metadata->linear_layout&&!f.metadata->gaze_atlas)||!f.eyes[0])return;
    const auto stamp=f.metadata->timestamp;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!stamp||stamp<=last_timestamp_||now<stamp)return;
    const auto tracking_stamp=f.metadata->surfxr_tracking_timestamp?f.metadata->surfxr_tracking_timestamp:stamp;
    last_timestamp_=stamp;
    // Upstream statistics are keyed by tracking, not capture. Keep their ACKs
    // monotonic even when distinct new images used the same/older render pose.
    if(tracking_stamp<=last_tracking_timestamp_||now<tracking_stamp)return;
    last_tracking_timestamp_=tracking_stamp;
    const auto& eye=*f.eyes[0];
    Feedback value;value.timestamp=tracking_stamp;value.interval=last_submit_&&now>=last_submit_?now-last_submit_:0;
    if(eye.packet_submit_monotonic_ns&&eye.decoder_output_monotonic_ns>=eye.packet_submit_monotonic_ns)
        value.decode=eye.decoder_output_monotonic_ns-eye.packet_submit_monotonic_ns;
    if(eye.decoded_monotonic_ns&&now>=eye.decoded_monotonic_ns)value.rendering=now-eye.decoded_monotonic_ns;
    value.total=now-tracking_stamp;last_submit_=now;
    queue_.push_back(value);while(queue_.size()>8)queue_.pop_front();
}
bool FeedbackQueue::pop(Feedback& f){std::lock_guard<std::mutex> lock(mutex_);if(queue_.empty())return false;f=queue_.front();queue_.pop_front();return true;}
}
