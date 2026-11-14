#include "surfxr/tracking.hpp"
#include <cmath>
namespace surfxr {
TrackingPredictionStatus set_tracking_prediction_time(TrackingFrame& frame,std::int64_t now) noexcept {
    frame.prediction_time_valid=false;frame.prediction_lead_seconds=0;
    if(now<=0||frame.predicted_xr_time<=0)return TrackingPredictionStatus::InvalidTime;
    if(frame.predicted_xr_time<now)return TrackingPredictionStatus::Past;
    const auto duration=frame.predicted_xr_time-now; // positive operands, no overflow
    if(duration>66'000'000)return TrackingPredictionStatus::TooFar;
    frame.prediction_lead_seconds=static_cast<float>(static_cast<double>(duration)*1e-9);
    frame.prediction_time_valid=true;return TrackingPredictionStatus::Valid;
}
bool tracking_pose_valid(const TrackingPose& value) noexcept {
    if((value.flags&(OrientationValid|PositionValid))!=(OrientationValid|PositionValid))return false;
    const auto p=value.pose.position;const auto q=value.pose.orientation;
    const float n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(n)&&std::abs(n-1)<.02f;
}
bool tracking_pose_tracked(const TrackingPose& value) noexcept {
    return tracking_pose_valid(value)&&(value.flags&(OrientationTracked|PositionTracked))==(OrientationTracked|PositionTracked);
}
bool tracking_shortcut_reserved(TrackingProfile profile,unsigned hand,bool vr_mode) noexcept {
    // Frame left View belongs to the remote SteamVR menu in both modes. Only
    // the physical right Menu participates in local keyboard/mode gestures.
    if(profile==TrackingProfile::SteamFrame)return hand==1;
    if(!vr_mode)return true;
    return profile==TrackingProfile::Simple||
        (profile==TrackingProfile::OculusTouch&&hand==0);
}
std::array<TrackingController,2> touch_controller_mapping(const TrackingFrame& frame) noexcept {
    auto result=frame.hands;
    const std::uint32_t common=ButtonThumbstick|ButtonTrigger|ButtonSqueeze|ButtonThumbrest;
    for(unsigned i=0;i<2;++i){const std::uint32_t keep=common|(i?(ButtonA|ButtonB):(ButtonX|ButtonY));
        result[i].buttons&=keep;result[i].buttons_active&=keep;result[i].touches&=keep;result[i].touches_active&=keep;
    }
    const auto transfer=[&](unsigned from,std::uint32_t source,std::uint32_t target){
        const auto& raw=frame.hands[from];auto& left=result[0];
        if(raw.buttons_active&source)left.buttons_active|=target;
        if(raw.buttons&raw.buttons_active&source)left.buttons|=target;
        if(raw.touches_active&source)left.touches_active|=target;
        if(raw.touches&raw.touches_active&source)left.touches|=target;
    };
    if(frame.hands[1].profile==TrackingProfile::SteamFrame){transfer(1,ButtonX,ButtonX);transfer(1,ButtonY,ButtonY);}
    if(frame.hands[0].profile==TrackingProfile::SteamFrame){
        transfer(0,ButtonView,ButtonMenu);
        transfer(0,ButtonSystem,ButtonMenu); // optional additional physical key
    }
    if(frame.hands[0].profile==TrackingProfile::ValveIndex){transfer(0,ButtonA,ButtonX);transfer(0,ButtonB,ButtonY);}
    for(auto& hand:result){hand.buttons&=hand.buttons_active;hand.touches&=hand.touches_active;}
    return result;
}
}
