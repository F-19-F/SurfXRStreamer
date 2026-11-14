#pragma once
#include "surfxr/tracking.hpp"
#include "surfxr/motion_sync.hpp"
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cmath>
#include <mutex>

namespace surfxr::xr_detail {
// Upload owns fresh locate calls; XR owns action synchronization and publishes
// handle/epoch/input snapshots. clear() is a barrier before session shutdown.
class CurrentTracking {
public:
    void publish(XrInstance instance,PFN_xrConvertTimespecTimeToTimeKHR convert,
                 PFN_xrLocateSpace locate,XrSpace stage,XrSpace head,
                 std::array<XrSpace,2> grips,const TrackingFrame& frame,
                 XrTime epoch_begin,XrTime next_change){
        std::lock_guard<std::mutex> lock(mutex_);
        instance_=instance;convert_=convert;locate_=locate;stage_=stage;head_=head;grips_=grips;
        frame_=frame;epoch_begin_=epoch_begin;next_change_=next_change;
        available_=stage&&head&&convert&&locate&&frame.optics_valid&&frame.origin_epoch;
    }
    void clear(){std::lock_guard<std::mutex> lock(mutex_);available_=false;}
    bool sample(TrackingFrame& out,const timespec& monotonic,std::uint64_t query_ns){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!available_||!frame_.sampled_monotonic_ns||query_ns<frame_.sampled_monotonic_ns||
           query_ns-frame_.sampled_monotonic_ns>100'000'000)return false;
        XrTime time=0;
        if(convert_(instance_,&monotonic,&time)!=XR_SUCCESS||!motion_target_in_epoch(time,epoch_begin_,next_change_))return false;
        auto frame=frame_;
        frame.head=locate(head_,time);
        if(!tracking_pose_valid(frame.head))return false;
        for(unsigned hand=0;hand<2;++hand){
            auto& controller=frame.hands[hand];
            controller.grip={};
            if(controller.grip_active&&tracking_controller_connected(controller))controller.grip=locate(grips_[hand],time);
            controller.grip_active=controller.grip_active&&tracking_pose_valid(controller.grip);
        }
        frame.query_xr_time=frame.predicted_xr_time=time;
        frame.sampled_monotonic_ns=frame.query_started_monotonic_ns=query_ns;
        frame.prediction_lead_seconds=0;frame.prediction_time_valid=true;frame.stage_locatable=true;
        frame.sequence=++sequence_;
        // Optics are cached independently; wire metadata uses head-relative,
        // orthogonal OpenVR eyes, not the prior XR frame's world eye poses.
        const auto q=frame.head.pose.orientation;
        const UiVec3 axis{1-2*(q.y*q.y+q.z*q.z),2*(q.x*q.y+q.w*q.z),2*(q.x*q.z-q.w*q.y)};
        for(unsigned eye=0;eye<2;++eye){frame.eye_poses[eye]=frame.head.pose;
            auto& p=frame.eye_poses[eye].position;const auto d=frame.ipd_meters*(eye?.5f:-.5f);
            p.x+=axis.x*d;p.y+=axis.y*d;p.z+=axis.z*d;}
        out=std::move(frame);return true;
    }
private:
    TrackingPose locate(XrSpace space,XrTime time){
        TrackingPose result;if(!space)return result;
        XrSpaceVelocity velocity{};velocity.type=XR_TYPE_SPACE_VELOCITY;
        XrSpaceLocation location{};location.type=XR_TYPE_SPACE_LOCATION;location.next=&velocity;
        if(locate_(space,stage_,time,&location)!=XR_SUCCESS)return result;
        result.flags=static_cast<std::uint32_t>(location.locationFlags)&15u;
        const auto p=location.pose.position;const auto q=location.pose.orientation;
        result.pose={{p.x,p.y,p.z},{q.x,q.y,q.z,q.w}};
        const auto finite=[](XrVector3f v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
        if((velocity.velocityFlags&XR_SPACE_VELOCITY_LINEAR_VALID_BIT)&&finite(velocity.linearVelocity)){
            const auto v=velocity.linearVelocity;result.linear_velocity={v.x,v.y,v.z};result.linear_velocity_valid=true;}
        if((velocity.velocityFlags&XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)&&finite(velocity.angularVelocity)){
            const auto v=velocity.angularVelocity;result.angular_velocity={v.x,v.y,v.z};result.angular_velocity_valid=true;}
        return result;
    }
    std::mutex mutex_;bool available_=false;TrackingFrame frame_;
    XrInstance instance_=XR_NULL_HANDLE;XrSpace stage_=XR_NULL_HANDLE,head_=XR_NULL_HANDLE;
    std::array<XrSpace,2> grips_{};XrTime epoch_begin_=0,next_change_=0;std::uint64_t sequence_=0;
    PFN_xrConvertTimespecTimeToTimeKHR convert_=nullptr;PFN_xrLocateSpace locate_=nullptr;
};
} // namespace surfxr::xr_detail
