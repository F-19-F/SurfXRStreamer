#pragma once
#include "surfxr/tracking.hpp"
#include <cmath>

namespace surfxr::xr_detail {
// Private runtime-bridge datagram, distinct from boundary/IPD/refresh packets.
struct ControllerStatus {
    std::uint32_t magic=0x43535431,known_mask=0,connected_mask=0,battery_mask=0;
    std::uint64_t sampled_ns=0;
    std::array<float,2> levels{};
    std::uint32_t charging_known_mask=0,charging_mask=0;
    bool valid()const {
        if(magic!=0x43535431||(known_mask|connected_mask|battery_mask|charging_known_mask|charging_mask)>3)return false;
        if((connected_mask&~known_mask)||(battery_mask&~connected_mask)||
           (charging_known_mask&~connected_mask)||(charging_mask&~charging_known_mask))return false;
        for(unsigned i=0;i<2;++i)if((battery_mask&(1u<<i))&&
            (!std::isfinite(levels[i])||levels[i]<0||levels[i]>1))return false;
        return true;
    }
    bool fresh(std::uint64_t now)const {
        return valid()&&sampled_ns&&sampled_ns<=now&&now-sampled_ns<=2'000'000'000;
    }
    void apply(unsigned i,TrackingController& hand,DeviceBattery& battery,std::uint64_t now)const {
        if(i>=2||!fresh(now)||(hand.profile!=TrackingProfile::SteamFrame&&hand.profile!=TrackingProfile::None))return;
        const auto bit=1u<<i;
        hand.connection_known=(known_mask&bit)!=0;hand.connected=(connected_mask&bit)!=0;
        if(hand.connection_known&&!hand.connected){battery={};return;}
        // OpenXR remains the preferred battery source when it actually supplies a value.
        if(battery.percent<0&&(battery_mask&bit))battery.percent=static_cast<int>(std::lround(levels[i]*100));
        if(!battery.charging_known&&(charging_known_mask&bit)){
            battery.charging_known=true;battery.charging=(charging_mask&bit)!=0;
        }
    }
};
static_assert(sizeof(ControllerStatus)==40);
}
