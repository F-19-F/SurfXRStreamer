#pragma once
#include "surfxr/system_input.hpp"
#include <openvr_driver.h>
#include <mutex>
#include <vector>
#include <optional>

namespace surfxr::frame_input {
// Public adapters can forward through another hooked ABI on the same thread.
// The outer hook owns the mutex and observes the physical update exactly once.
inline thread_local unsigned forwarding_depth=0;
struct ForwardCall {ForwardCall(){++forwarding_depth;}~ForwardCall(){--forwarding_depth;}};
using CreateBoolean=vr::EVRInputError(*)(void*,vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*);
using UpdateBoolean=vr::EVRInputError(*)(void*,vr::VRInputComponentHandle_t,bool,double);
using CreateSkeleton=vr::EVRInputError(*)(void*,vr::PropertyContainerHandle_t,const char*,const char*,const char*,vr::EVRSkeletalTrackingLevel,const vr::VRBoneTransform_t*,std::uint32_t,vr::VRInputComponentHandle_t*);
using UpdateSkeleton=vr::EVRInputError(*)(void*,vr::VRInputComponentHandle_t,vr::EVRSkeletalMotionRange,const vr::VRBoneTransform_t*,std::uint32_t);
struct HookInterface {
    std::array<unsigned,4> slots{{0,1,5,6}}; // Public SDK layout; private slots are decoded.
    void** table=nullptr;
    void* instance=nullptr;
    CreateBoolean create=nullptr;
    UpdateBoolean update=nullptr;
    CreateSkeleton create_skeleton=nullptr;
    UpdateSkeleton update_skeleton=nullptr;
};
struct SkeletalComponent {vr::PropertyContainerHandle_t container=0;vr::VRInputComponentHandle_t handle=0;Skeleton value{};};
struct Component {
    vr::PropertyContainerHandle_t container=0;
    vr::VRInputComponentHandle_t handle=0;
    void* input=nullptr;
    int hand=-1;
    bool touch=false,down=false,seen=false,registered=false;
};
struct DriverState {
    std::mutex mutex;
    CapturePolicy policy;
    std::vector<Component> components;
    std::vector<SkeletalComponent> skeletons;
    std::array<vr::PropertyContainerHandle_t,2> containers{};
    CreateBoolean create=nullptr;
    UpdateBoolean update=nullptr;
    bool enabled=false;
    void* interface=nullptr;
    std::array<HookInterface,3> interfaces{};
    unsigned interface_count=0;
};
DriverState& driver_state();
bool install_hooks(void* interface,bool require_known_abi=false);
std::optional<HookInterface> forwarded_interface(const HookInterface&);
void remove_hooks();
void remember_component(void* input,vr::PropertyContainerHandle_t container,const char* path,vr::VRInputComponentHandle_t handle,bool registered=false);
void refresh_components(std::uint64_t now,const std::array<vr::PropertyContainerHandle_t,2>& selected);
void refresh_devices(std::uint64_t now);
void release_forwarded();
const HookInterface* original_interface(void* input);
vr::EVRInputError create_skeleton(void*,vr::PropertyContainerHandle_t,const char*,const char*,const char*,vr::EVRSkeletalTrackingLevel,const vr::VRBoneTransform_t*,std::uint32_t,vr::VRInputComponentHandle_t*);
vr::EVRInputError update_skeleton(void*,vr::VRInputComponentHandle_t,vr::EVRSkeletalMotionRange,const vr::VRBoneTransform_t*,std::uint32_t);
}
