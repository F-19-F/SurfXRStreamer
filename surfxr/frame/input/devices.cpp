#include "driver.hpp"
#include <array>
#include <cstring>

namespace surfxr::frame_input {
void refresh_devices(std::uint64_t now){
    auto& s=driver_state();
    auto* props=vr::VRProperties();auto* host=vr::VRServerDriverHost();
    if(!props||!host){s.policy.advance(now,0,0);return;}
    std::array<vr::TrackedDevicePose_t,vr::k_unMaxTrackedDeviceCount> poses{};
    std::array<vr::PropertyContainerHandle_t,2> selected{};
    host->GetRawTrackedDevicePoses(0,poses.data(),poses.size());
    for(unsigned index=0;index<poses.size();++index){
        if(!poses[index].bDeviceIsConnected)continue;
        const auto container=props->TrackedDeviceToPropertyContainer(index);
        if(props->GetStringProperty(container,vr::Prop_ControllerType_String)!="frame_controller")continue;
        const auto role=props->GetInt32Property(container,vr::Prop_ControllerRoleHint_Int32);
        const int hand=role==vr::TrackedControllerRole_LeftHand?0:role==vr::TrackedControllerRole_RightHand?1:-1;
        if(hand>=0&&!selected[hand])selected[hand]=container;
    }
    refresh_components(now,selected);
}
void refresh_components(std::uint64_t now,const std::array<vr::PropertyContainerHandle_t,2>& selected){
    auto& s=driver_state();unsigned available=0,touch_available=0;
    if(selected!=s.containers){
        s.policy.advance(now,0,0);s.containers=selected;
        for(auto& c:s.components)c.hand=-1; // Reseed unchanged hands after the reset too.
    }
    for(auto& c:s.components)if(c.container!=selected[0]&&c.container!=selected[1])c.hand=-1;
    for(unsigned hand=0;hand<2;++hand){
        const auto container=selected[hand];if(!container)continue;
        // Recover handles if hardware activation preceded provider Init. Only
        // a hardware creation or update grants readiness. A recovery call
        // alone cannot enable capture for an unused component.
        for(const auto* path:{"/input/system/click","/input/system/touch"}){
            bool known=false;for(const auto& c:s.components)if(c.container==container&&c.touch==(std::strcmp(path,"/input/system/touch")==0))known=true;
            if(!known){vr::VRInputComponentHandle_t handle=0;
                ForwardCall call;
                if(s.create(s.interface,container,path,&handle)==vr::VRInputError_None)remember_component(s.interface,container,path,handle);}
        }
        for(auto& c:s.components)if(c.container==container){
            if(c.hand!=static_cast<int>(hand)){c.hand=static_cast<int>(hand);s.policy.update(hand,c.touch,c.down,now);}
            if(c.registered||c.seen){if(c.touch)touch_available|=1u<<hand;else available|=1u<<hand;}
        }
    }
    s.policy.advance(now,available,touch_available);
}
}
