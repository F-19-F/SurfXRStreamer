#include "steamvr_controllers.hpp"
#include <cstring>
#ifdef SURFXR_HAVE_OPENVR_REFRESH
#include <openvr.h>
#endif

namespace surfxr::xr_detail {
bool SteamVrControllers::poll(ControllerStatus& result,std::uint64_t now,bool changed){
    if(!changed&&now<next_sample_)return false;
    next_sample_=now+1'000'000'000;result={};result.sampled_ns=now;
#ifdef SURFXR_HAVE_OPENVR_REFRESH
    auto* system=vr::VRSystem();if(!system)return true;
    result.known_mask=3;unsigned seen=0;
    for(vr::TrackedDeviceIndex_t id=0;id<vr::k_unMaxTrackedDeviceCount;++id){
        if(system->GetTrackedDeviceClass(id)!=vr::TrackedDeviceClass_Controller||!system->IsTrackedDeviceConnected(id))continue;
        char type[128]{};vr::ETrackedPropertyError error=vr::TrackedProp_Success;
        system->GetStringTrackedDeviceProperty(id,vr::Prop_ControllerType_String,type,sizeof(type),&error);
        if(error!=vr::TrackedProp_Success||std::strcmp(type,"frame_controller"))continue;
        const auto role=system->GetControllerRoleForTrackedDeviceIndex(id);
        if(role!=vr::TrackedControllerRole_LeftHand&&role!=vr::TrackedControllerRole_RightHand)continue;
        const unsigned hand=role==vr::TrackedControllerRole_RightHand,bit=1u<<hand;
        // Ambiguous roles must not choose a battery or a physical controller by enumeration order.
        if(seen&bit){result.known_mask&=~bit;result.connected_mask&=~bit;result.battery_mask&=~bit;
            result.charging_known_mask&=~bit;result.charging_mask&=~bit;continue;}
        seen|=bit;result.connected_mask|=bit;
        const auto level=system->GetFloatTrackedDeviceProperty(id,vr::Prop_DeviceBatteryPercentage_Float,&error);
        if(error==vr::TrackedProp_Success&&std::isfinite(level)&&level>=0&&level<=1){
            result.levels[hand]=level;result.battery_mask|=bit;
        }
        const auto charging=system->GetBoolTrackedDeviceProperty(id,vr::Prop_DeviceIsCharging_Bool,&error);
        if(error==vr::TrackedProp_Success){result.charging_known_mask|=bit;if(charging)result.charging_mask|=bit;}
    }
#endif
    return true;
}
}
