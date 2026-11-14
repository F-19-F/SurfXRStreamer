#include "tracking_ledger.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr::transport {
void TrackingLedger::sent(std::uint64_t timestamp,const TrackingFrame& frame){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!entries_.empty()&&entries_.back().frame.origin_epoch!=frame.origin_epoch)entries_.clear();
    entries_.push_back({timestamp,frame});while(entries_.size()>1024)entries_.pop_front();
}
std::shared_ptr<const VrVideoMetadata> TrackingLedger::metadata(std::uint64_t timestamp,unsigned width,unsigned height,std::uint64_t received,std::uint64_t image_timestamp){
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it=std::find_if(entries_.rbegin(),entries_.rend(),[&](const auto& e){return e.timestamp==timestamp;});
    if(it==entries_.rend())return {};
    const auto& f=it->frame;auto result=std::make_shared<VrVideoMetadata>();
    result->timestamp=image_timestamp?image_timestamp:timestamp;result->surfxr_tracking_timestamp=timestamp;
    result->tracking_frame_id=static_cast<std::int32_t>(f.sequence&0x7fffffff);
    result->header_present=result->pose_valid=result->fov_valid=true;result->render_width=width;result->render_height=height;
    result->header_received_monotonic_ns=received;result->linear_layout=true;
    // A delayed reference-valid picture can preserve motion continuity while
    // the bounded playout reservoir catches up. Epoch checks and the queue
    // still discard obsolete output; no pose/crop timestamp is refreshed.
    result->rgba_deadline_monotonic_ns=timestamp+200'000'000;
    // SURFXR OpenVR views are orthogonal, with eye offsets along head-local X.
    // Match that PC view contract rather than attaching the latest XR eye pose.
    const auto q=f.head.pose.orientation;
    const UiVec3 axis{1-2*(q.y*q.y+q.z*q.z),2*(q.x*q.y+q.w*q.z),2*(q.x*q.z-q.w*q.y)};
    for(unsigned i=0;i<2;++i){const float offset=f.ipd_meters*(i?.5f:-.5f);result->eye_poses[i]=f.head.pose;
        auto& p=result->eye_poses[i].position;p.x+=axis.x*offset;p.y+=axis.y*offset;p.z+=axis.z*offset;
        const auto a=f.eye_fov[i];result->fov[i]={std::tan(a[2]),-std::tan(a[3]),-std::tan(a[0]),std::tan(a[1])};
    }
    return result;
}
}
