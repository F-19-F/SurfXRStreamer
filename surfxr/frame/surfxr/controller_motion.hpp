#pragma once
#include "surfxr/tracking.hpp"

namespace surfxr::transport {
// Keep poses during focus loss; explicit forwarding disable is a disconnect.
class ControllerMotionCache {
public:
    void apply(TrackingFrame& frame,const std::array<bool,2>& active,bool enabled){
        if(epoch_!=frame.origin_epoch){poses_={};epoch_=frame.origin_epoch;}
        for(unsigned i=0;i<2;++i){auto& hand=frame.hands[i];
            if(!enabled||!tracking_controller_connected(hand)){poses_[i]={};hand.grip={};hand.grip_active=false;}
            else if(active[i])poses_[i]=hand.grip;
            else{hand.grip=poses_[i];hand.grip.linear_velocity_valid=hand.grip.angular_velocity_valid=false;
                hand.grip_active=tracking_pose_valid(hand.grip);}
        }
    }
private:
    std::array<TrackingPose,2> poses_{};
    std::uint64_t epoch_=0;
};
}
