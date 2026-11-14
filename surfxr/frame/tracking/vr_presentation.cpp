#include "surfxr/vr_client_state.hpp"
#include <algorithm>
#include <iterator>

namespace surfxr {
std::shared_ptr<const VrStereoFrame> VrClientState::presentation_stereo(){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!stats_.requested_vr||!have_tracking_||!stage_ready(tracking_)){presentation_candidate_.reset();return nullptr;}
    const auto now=clock_ns();
    // Retire obsolete acknowledgement history, never the newest pair when
    // input stalls. History age does not delay selection of a newer pair.
    const auto retention=vr_only_?75'000'000ull:16'000'000ull;
    while(presentation_.size()>1&&now>=presentation_.front().ready_monotonic_ns&&
          now-presentation_.front().ready_monotonic_ns>retention){
        presentation_.pop_front();++stats_.paired_queue_stale;
    }
    presentation_candidate_=presentation_.empty()?stereo_:presentation_.back().frame;
    if(vr_only_){
        // Until another image is due, resubmit the exact successfully presented
        // pair. Exposing stereo_ here would show a future image, then go back.
        presentation_candidate_=presented_stereo_;
        const auto pending=std::find_if(presentation_.begin(),presentation_.end(),[&](const auto& pair){
            return !have_presented_timestamp_||pair.frame->metadata->timestamp>last_presented_timestamp_;
        });
        if(pending!=presentation_.end()){
            const auto stamp=pending->frame->metadata->timestamp;
            // Upload timestamps use this headset's monotonic clock. Anchor a
            // source timeline once, rather than delaying each frame from its
            // jittered arrival: slack then absorbs a late packet automatically.
            const auto delta=stamp>=playout_source_ns_?stamp-playout_source_ns_:0;
            if(now>=playout_local_ns_&&delta<=now-playout_local_ns_)presentation_candidate_=pending->frame;
            else ++stats_.playout_buffer_holds;
        }
    }
    return presentation_candidate_;
}
void VrClientState::stereo_presented(std::shared_ptr<const VrStereoFrame> frame){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!frame||!frame->metadata||!stats_.requested_vr||!have_tracking_||
       frame->origin_epoch!=tracking_.origin_epoch)return;
    // Pairs are produced in strictly increasing source-timestamp order within
    // a video configuration. Repeats or a late ACK cannot count twice or move
    // the acknowledgement backwards; a reset starts a new timestamp sequence.
    if(have_presented_timestamp_&&frame->metadata->timestamp<=last_presented_timestamp_)return;
    // Identity also rejects a callback from before a configuration reset that
    // reused an epoch/timestamp. An old or repeated ACK cannot pop newer work.
    const auto found=std::find_if(presentation_.begin(),presentation_.end(),[&](const auto& pending){
        return pending.frame==frame&&pending.frame->origin_epoch==frame->origin_epoch&&
               pending.frame->metadata->timestamp==frame->metadata->timestamp;
    });
    // Rendering can outlast the bounded queue: the exact selected pair is
    // still owned by XR even after decoder publication evicts its queue entry.
    // Accept that ACK without consuming either of its newer successors. Both
    // candidate and queue are cleared on configuration/epoch/mode changes.
    if(found==presentation_.end()&&frame!=presentation_candidate_)return;
    last_presented_timestamp_=frame->metadata->timestamp;have_presented_timestamp_=true;
    if(vr_only_)presented_stereo_=frame;
    ++stats_.queued_presentations;
    if(found!=presentation_.end()){
        stats_.paired_queue_skipped+=static_cast<std::uint64_t>(std::distance(presentation_.begin(),found));
        presentation_.erase(presentation_.begin(),std::next(found));
    }
}
}
