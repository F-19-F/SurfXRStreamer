#include "wire.hpp"
#include <cmath>

namespace surfxr::transport {
Bytes frame_skeleton(const TrackingFrame& frame,unsigned hand,bool enabled,std::uint64_t now){
    require(hand<2,"surfxr_invalid_skeleton_hand");const auto& skeleton=frame.controller_skeletons[hand];
    bool active=enabled&&frame.focused&&frame.visible&&tracking_controller_connected(frame.hands[hand])&&frame.hands[hand].profile==TrackingProfile::SteamFrame&&
        skeleton.sampled_monotonic_ns&&now>=skeleton.sampled_monotonic_ns&&now-skeleton.sampled_monotonic_ns<=100'000'000;
    if(active)for(const auto& bone:skeleton.bones){const auto& q=bone.orientation;const auto& p=bone.position;
        const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
        if(!std::isfinite(norm)||norm<.5f||norm>1.5f||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
           std::abs(p.x)>2||std::abs(p.y)>2||std::abs(p.z)>2)active=false;
    }
    Writer body;body.integer(0x5644534b,4);body.integer(1,4);body.integer(hand,4);body.integer(active,4);
    body.integer(active?skeleton.sampled_monotonic_ns:0,8);
    if(active)for(const auto& bone:skeleton.bones){const auto& q=bone.orientation;const auto& p=bone.position;
        for(float value:{p.x,p.y,p.z,q.x,q.y,q.z,q.w})body.real(value);
    }
    Writer control(true);control.integer(11,4);control.blob(body.bytes);return std::move(control.bytes);
}
}
