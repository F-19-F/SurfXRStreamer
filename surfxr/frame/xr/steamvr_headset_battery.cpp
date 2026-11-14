#include "headset_battery_status.hpp"
#ifdef SURFXR_HAVE_OPENVR_REFRESH
#include <openvr.h>
#endif

namespace surfxr::xr_detail {
HeadsetBatteryStatus steamvr_headset_battery(std::uint64_t now){
    HeadsetBatteryStatus result;result.sampled_ns=now;
#ifdef SURFXR_HAVE_OPENVR_REFRESH
    auto* system=vr::VRSystem();
    if(!system||!system->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd))return result;
    vr::ETrackedPropertyError error=vr::TrackedProp_Success;
    const auto level=system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_DeviceBatteryPercentage_Float,&error);
    if(error==vr::TrackedProp_Success&&std::isfinite(level)&&level>=0&&level<=1){
        result.level=level;result.flags|=HeadsetBatteryStatus::HasLevel;
    }
    const auto charging=system->GetBoolTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_DeviceIsCharging_Bool,&error);
    if(error==vr::TrackedProp_Success){
        result.flags|=HeadsetBatteryStatus::ChargingKnown;
        if(charging)result.flags|=HeadsetBatteryStatus::Charging;
    }
#endif
    return result;
}
}
