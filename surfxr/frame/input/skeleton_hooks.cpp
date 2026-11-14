#include "driver.hpp"
#include <algorithm>
#include <cstring>

namespace surfxr::frame_input {
vr::EVRInputError create_skeleton(void* input,vr::PropertyContainerHandle_t container,const char* name,const char* skeleton,const char* base,
    vr::EVRSkeletalTrackingLevel level,const vr::VRBoneTransform_t* limits,std::uint32_t count,vr::VRInputComponentHandle_t* handle){
    if(forwarding_depth){const auto* api=original_interface(input);return api?api->create_skeleton(input,container,name,skeleton,base,level,limits,count,handle):vr::VRInputError_InvalidHandle;}
    auto& s=driver_state();std::lock_guard<std::mutex> lock(s.mutex);
    const auto* api=original_interface(input);if(!api)return vr::VRInputError_InvalidHandle;
    ForwardCall call;const auto result=api->create_skeleton(input,container,name,skeleton,base,level,limits,count,handle);
    if(s.enabled&&result==vr::VRInputError_None&&handle&&*handle&&name&&
       (!std::strcmp(name,"/input/skeleton/left")||!std::strcmp(name,"/input/skeleton/right"))){
        auto found=std::find_if(s.skeletons.begin(),s.skeletons.end(),[&](const auto& c){return c.handle==*handle;});
        if(found==s.skeletons.end()){
            if(s.skeletons.size()>=16){
                found=std::find_if(s.skeletons.begin(),s.skeletons.end(),[&](const auto& c){return c.container!=s.containers[0]&&c.container!=s.containers[1];});
                if(found==s.skeletons.end())return result;
                s.skeletons.erase(found);
            }
            s.skeletons.push_back({container,*handle,{}});
        }
    }
    return result;
}
vr::EVRInputError update_skeleton(void* input,vr::VRInputComponentHandle_t handle,vr::EVRSkeletalMotionRange range,
    const vr::VRBoneTransform_t* bones,std::uint32_t count){
    if(forwarding_depth){const auto* api=original_interface(input);return api?api->update_skeleton(input,handle,range,bones,count):vr::VRInputError_InvalidHandle;}
    auto& s=driver_state();std::lock_guard<std::mutex> lock(s.mutex);
    const auto* api=original_interface(input);if(!api)return vr::VRInputError_InvalidHandle;
    ForwardCall call;const auto result=api->update_skeleton(input,handle,range,bones,count);
    if(s.enabled&&result==vr::VRInputError_None&&bones&&count==31)for(auto& c:s.skeletons)if(c.handle==handle){
        Skeleton value;value.sampled_ns=monotonic_ns();
        for(unsigned i=0;i<31;++i){const auto& b=bones[i];
            value.bones[i].position={b.position.v[0],b.position.v[1],b.position.v[2]};
            value.bones[i].orientation={b.orientation.x,b.orientation.y,b.orientation.z,b.orientation.w};
        }
        if(valid_skeleton(value))c.value=value;
        break;
    }
    return result; // Skeletons always reach the local runtime, including during capture.
}
}
