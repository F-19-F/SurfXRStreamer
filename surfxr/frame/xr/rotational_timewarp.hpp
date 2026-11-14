#pragma once
#include <array>
#include <cmath>

namespace surfxr::xr_detail {
// Destination rays -> immutable source-eye rays. FOV is left/right/up/down
// positive tangents, shared by the source and destination projections.
struct RotationalTimewarp {
    std::array<float,4> rotation{0,0,0,1},fov{},orientation{0,0,0,1};
};
inline bool rotational_timewarp(std::array<float,4> source,std::array<float,4> target,
        const std::array<float,4>& fov,RotationalTimewarp& output){
    output={};
    for(float v:fov)if(!std::isfinite(v)||v<=0||v>100)return false;
    for(auto* q:{&source,&target}){
        float norm=0;for(float v:*q){if(!std::isfinite(v))return false;norm+=v*v;}
        if(norm<.9f||norm>1.1f)return false;
        const float scale=1/std::sqrt(norm);for(float& v:*q)v*=scale;
    }
    const float x=-source[0],y=-source[1],z=-source[2],w=source[3];
    const auto& b=target;
    output.rotation={w*b[0]+x*b[3]+y*b[2]-z*b[1],w*b[1]-x*b[2]+y*b[3]+z*b[0],
                     w*b[2]+x*b[1]-y*b[0]+z*b[3],w*b[3]-x*b[0]-y*b[1]-z*b[2]};
    output.fov=fov;output.orientation=target;return true;
}
} // namespace surfxr::xr_detail
