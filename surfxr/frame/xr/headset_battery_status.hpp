#pragma once
#include "surfxr/battery.hpp"
#include <cmath>

namespace surfxr::xr_detail {
// Private runtime-bridge packet; distinct size/magic from existing datagrams.
struct HeadsetBatteryStatus {
    std::uint32_t magic=0x48425331,flags=0;
    std::uint64_t sampled_ns=0;
    float level=0;
    std::uint32_t reserved=0;
    enum : std::uint32_t { HasLevel=1,ChargingKnown=2,Charging=4 };
    bool valid()const {
        return magic==0x48425331&&reserved==0&&(flags&~7u)==0&&
            (!(flags&Charging)||(flags&ChargingKnown))&&
            (!(flags&HasLevel)||(std::isfinite(level)&&level>=0&&level<=1));
    }
    DeviceBattery battery()const {
        if(!valid()||!(flags&HasLevel))return {};
        return {static_cast<int>(std::lround(static_cast<double>(level)*100)),
                (flags&Charging)!=0,(flags&ChargingKnown)!=0};
    }
};
static_assert(sizeof(HeadsetBatteryStatus)==24);
HeadsetBatteryStatus steamvr_headset_battery(std::uint64_t now);
}
