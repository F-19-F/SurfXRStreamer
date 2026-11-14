#pragma once
#include "surfxr/tracking.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr {
// Invalid samples never overwrite the last coordinate of an individual eye.
inline UiVec3 gaze_rotate(UiQuat q,UiVec3 v) noexcept {
    const UiVec3 t{2*(q.y*v.z-q.z*v.y),2*(q.z*v.x-q.x*v.z),2*(q.x*v.y-q.y*v.x)};
    return {v.x+q.w*t.x+q.y*t.z-q.z*t.y,v.y+q.w*t.y+q.z*t.x-q.x*t.z,v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
// Project the gaze ray in each eye's camera coordinates, preserving asymmetric
// FOV and canting. gaze and eye poses must share a reference space/query time.
inline bool project_gaze(const TrackingPose& gaze,const UiPose& eye,const std::array<float,4>& fov,
                         std::array<float,2>& output) noexcept {
    TrackingPose camera;camera.pose=eye;camera.flags=OrientationValid|PositionValid;
    if(!tracking_pose_valid(gaze)||!tracking_pose_valid(camera))return false;
    for(float angle:fov)if(!std::isfinite(angle)||std::abs(angle)>=1.570796327f)return false;
    const float l=std::tan(fov[0]),r=std::tan(fov[1]),u=std::tan(fov[2]),d=std::tan(fov[3]);
    if(!(l<r&&d<u))return false;
    const auto ray=gaze_rotate(gaze.pose.orientation,{0,0,-1});
    const auto q=eye.orientation;
    const auto local=gaze_rotate({-q.x,-q.y,-q.z,q.w},ray);
    if(!std::isfinite(local.z)||local.z>=-1e-6f)return false;
    const float x=(2*local.x/-local.z-(r+l))/(r-l);
    const float y=-(2*local.y/-local.z-(u+d))/(u-d);
    if(!std::isfinite(x)||!std::isfinite(y))return false;
    output={std::clamp(x,-.5f,.5f),std::clamp(y,-.5f,.5f)};return true;
}
} // namespace surfxr
