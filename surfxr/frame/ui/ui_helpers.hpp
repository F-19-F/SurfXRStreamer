#pragma once
#include "surfxr/vr_ui.hpp"
#include "settings_layout.hpp"
#include <cmath>
namespace surfxr::ui_detail {
inline bool valid_pose(UiPose p) noexcept {
    const auto q=p.orientation;const float n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    return std::isfinite(p.position.x)&&std::isfinite(p.position.y)&&std::isfinite(p.position.z)&&std::isfinite(n)&&std::abs(n-1)<.02f;
}
inline UiPose normalized_pose(UiPose p) noexcept {
    auto& q=p.orientation;const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    q.x/=n;q.y/=n;q.z/=n;q.w/=n;return p;
}
inline bool discontinuous_grab(UiPose before,UiPose after) noexcept {
    const auto a=before.position,b=after.position;
    const float dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;
    const auto q=before.orientation,r=after.orientation;
    // A half-metre or 45-degree step in one UI sample is a tracking jump.
    // q and -q describe the same orientation.
    return dx*dx+dy*dy+dz*dz>.5f*.5f||std::abs(q.x*r.x+q.y*r.y+q.z*r.z+q.w*r.w)<.9238795f;
}
inline bool in_rect(float u,float v,float x,float y,float width,float height){return u>=x&&u<=x+width&&v>=y&&v<=y+height;}
inline std::array<bool,4> menu_actions_enabled(const RemoteSettingsCapabilities& c){
    if(c.session_mode_switch)return {c.connected&&!c.vr_requested,c.connected,c.connected&&c.surfxr_video_controls,c.connected&&c.vr_active};
    if(c.vr_only)return {false,false,false,false};
    return {c.connected&&!c.vr_requested&&c.vr_ready&&c.can_launch_vr,
        c.connected,
        c.connected&&c.has_vr&&!c.can_launch_vr,c.connected&&c.vr_active};
}
inline std::array<bool,2> monitor_actions_enabled(const RemoteSettingsCapabilities& c){
    const bool ready=c.connected&&!c.vr_requested&&!c.monitor_pending;
    return {ready&&c.can_remove_monitor&&c.desktop_monitor_count!=1,ready&&c.can_add_monitor&&c.desktop_monitor_count<4};
}
}
