#pragma once
#include "rotational_timewarp.hpp"
#include <openxr/openxr.h>
#include <cstdint>

namespace surfxr::xr_detail {
// Called for both new and retained images after staging, just before their
// reconstruction. The input views always describe immutable decoded pixels.
// A failed stereo locate restores source pixels/poses as one atomic choice.
inline bool late_projection_timewarp(XrSession session,XrSpace stage,XrTime time,
        PFN_xrLocateViews locate_views,std::array<XrCompositionLayerProjectionView,2>& views,
        std::array<RotationalTimewarp,2>& output){
    output={};
    if(!session||!stage||time<=0||!locate_views)return false;
    XrViewLocateInfo locate{};locate.type=XR_TYPE_VIEW_LOCATE_INFO;
    locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=time;locate.space=stage;
    XrViewState valid{};valid.type=XR_TYPE_VIEW_STATE;
    std::array<XrView,2> eyes{};for(auto& eye:eyes)eye.type=XR_TYPE_VIEW;
    std::uint32_t count=0;
    if(locate_views(session,&locate,&valid,2,&count,eyes.data())!=XR_SUCCESS||count!=2||
       !(valid.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT))return false;
    std::array<RotationalTimewarp,2> transforms;
    for(unsigned eye=0;eye<2;++eye){
        const auto& source=views[eye].pose.orientation;const auto& target=eyes[eye].pose.orientation;
        const auto& f=views[eye].fov;
        if(!rotational_timewarp({source.x,source.y,source.z,source.w},
            {target.x,target.y,target.z,target.w},
            {-std::tan(f.angleLeft),std::tan(f.angleRight),std::tan(f.angleUp),-std::tan(f.angleDown)},transforms[eye]))return false;
    }
    output=transforms;
    for(unsigned eye=0;eye<2;++eye){const auto& q=transforms[eye].orientation;
        views[eye].pose.orientation={q[0],q[1],q[2],q[3]};}
    // Without depth, keep source eye positions and FOV. Declare the actual
    // corrected orientation so the runtime's final warp does not apply twice.
    return true;
}
} // namespace surfxr::xr_detail
