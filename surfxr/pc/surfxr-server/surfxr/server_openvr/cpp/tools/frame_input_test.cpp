#include "../surfxr_server/FrameSkeleton.h"
#include "../surfxr_server/FrameInput.h"
#include <cassert>
#include <iostream>
#include <thread>

// Stable fake path identity; wire hash agreement is covered by Rust/Frame tests.
extern "C" unsigned long long PathStringToHash(const char* text){
    unsigned long long result=14695981039346656037ull;
    while(*text)result=(result^static_cast<unsigned char>(*text++))*1099511628211ull;
    return result;
}
void component_checks(){
    for(bool left:{true,false}){
        const std::string hand=left?"/user/hand/left/input/":"/user/hand/right/input/";
        const auto check=[&](const char* name,const char* suffix){
            const auto id=PathStringToHash((hand+name+"/"+suffix).c_str());const auto value=FrameInput(id,left);
            assert(value.steamvr_paths.size()==1&&FrameInput(id,!left).steamvr_paths.empty());
            const auto component=std::string("/input/")+(std::string(name)=="squeeze"?"grip":name)+"/"+suffix;
            assert(value.steamvr_paths[0]==component);
        };
        for(const char* name:{"bumper","trigger","squeeze","thumbstick","system"})for(const char* suffix:{"click","touch"})check(name,suffix);
        for(const char* name:(left?std::vector<const char*>{"dpad_up","dpad_down","dpad_left","dpad_right","view"}:std::vector<const char*>{"a","b","x","y","menu"}))
            for(const char* suffix:{"click","touch"})check(name,suffix);
        check("trigger","value");check("squeeze","value");check("thumbstick","x");check("thumbstick","y");
    }
    assert(FrameInput(PathStringToHash("/user/hand/left/input/x/click"),true).steamvr_paths.empty());
    assert(FrameInput(PathStringToHash("/user/hand/right/input/dpad_up/click"),false).steamvr_paths.empty());
}

class Input final:public vr::IVRDriverInput {
public:
    unsigned calls=0;std::array<vr::VRBoneTransform_t,31> latest{};
    vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override{return vr::VRInputError_None;}
    vr::EVRInputError UpdateBooleanComponent(vr::VRInputComponentHandle_t,bool,double) override{return vr::VRInputError_None;}
    vr::EVRInputError CreateScalarComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*,vr::EVRScalarType,vr::EVRScalarUnits) override{return vr::VRInputError_None;}
    vr::EVRInputError UpdateScalarComponent(vr::VRInputComponentHandle_t,float,double) override{return vr::VRInputError_None;}
    vr::EVRInputError CreateHapticComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override{return vr::VRInputError_None;}
    vr::EVRInputError CreateSkeletonComponent(vr::PropertyContainerHandle_t,const char*,const char*,const char*,vr::EVRSkeletalTrackingLevel,const vr::VRBoneTransform_t*,uint32_t,vr::VRInputComponentHandle_t*) override{return vr::VRInputError_None;}
    vr::EVRInputError UpdateSkeletonComponent(vr::VRInputComponentHandle_t handle,vr::EVRSkeletalMotionRange range,const vr::VRBoneTransform_t* bones,uint32_t count) override{
        assert(handle==1&&count==31&&range==(calls%2?vr::VRSkeletalMotionRange_WithoutController:vr::VRSkeletalMotionRange_WithController));
        ++calls;std::copy(bones,bones+count,latest.begin());return vr::VRInputError_None;
    }
    vr::EVRInputError CreatePoseComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override{return vr::VRInputError_None;}
    vr::EVRInputError UpdatePoseComponent(vr::VRInputComponentHandle_t,const vr::HmdMatrix34_t*,double) override{return vr::VRInputError_None;}
    vr::EVRInputError CreateEyeTrackingComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override{return vr::VRInputError_None;}
    vr::EVRInputError UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t,const vr::VREyeTrackingData_t*,double) override{return vr::VRInputError_None;}
};
int main(){
    component_checks();
    FrameSkeleton cache;Input input;assert(!cache.apply(&input,1));FfiHandSkeleton value{};
    for(unsigned i=0;i<31;++i){value.jointRotations[i]={.1f,.2f,.3f,.92736185f};value.jointPositions[i][0]=i*.001f;}
    cache.update(100,&value);assert(cache.apply(&input,1)&&input.calls==2);
    assert(input.latest[30].position.v[0]==30*.001f&&input.latest[30].position.v[3]==1);
    assert(input.latest[30].orientation.x==.1f&&input.latest[30].orientation.w==.92736185f);
    value.jointPositions[30][0]=.04f;cache.update(99,&value);cache.apply(&input,1);assert(input.latest[30].position.v[0]==30*.001f);
    assert(!cache.apply(nullptr,1)&&!cache.apply(&input,0));
    std::this_thread::sleep_for(std::chrono::milliseconds(260));assert(!cache.apply(&input,1));
    cache.update(101,&value);assert(cache.apply(&input,1));cache.update(0,nullptr);assert(!cache.apply(&input,1));
    std::cout<<"Native 48 components, 31-joint bones, ranges, age and release passed; no SteamVR or hardware input\n";
}
