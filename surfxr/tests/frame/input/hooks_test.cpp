#include "driver.hpp"
#include <stdexcept>
#include <cstddef>

using namespace surfxr::frame_input;
namespace {
class FakeInput:public vr::IVRDriverInput {
public:
    vr::VRInputComponentHandle_t next=1,last=0;
    bool value=false;
    unsigned skeleton_updates=0;
    vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t* handle) override {*handle=next++;return vr::VRInputError_None;}
    vr::EVRInputError UpdateBooleanComponent(vr::VRInputComponentHandle_t handle,bool down,double) override {last=handle;value=down;return vr::VRInputError_None;}
    vr::EVRInputError CreateScalarComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*,vr::EVRScalarType,vr::EVRScalarUnits) override {return vr::VRInputError_None;}
    vr::EVRInputError UpdateScalarComponent(vr::VRInputComponentHandle_t,float,double) override {return vr::VRInputError_None;}
    vr::EVRInputError CreateHapticComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override {return vr::VRInputError_None;}
    vr::EVRInputError CreateSkeletonComponent(vr::PropertyContainerHandle_t,const char*,const char*,const char*,vr::EVRSkeletalTrackingLevel,const vr::VRBoneTransform_t*,std::uint32_t,vr::VRInputComponentHandle_t* handle) override {*handle=next++;return vr::VRInputError_None;}
    vr::EVRInputError UpdateSkeletonComponent(vr::VRInputComponentHandle_t,vr::EVRSkeletalMotionRange,const vr::VRBoneTransform_t*,std::uint32_t) override {++skeleton_updates;return vr::VRInputError_None;}
#ifdef SURFXR_INPUT_POSE_ABI
    vr::EVRInputError CreatePoseComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override {return vr::VRInputError_None;}
    vr::EVRInputError UpdatePoseComponent(vr::VRInputComponentHandle_t,const vr::HmdMatrix34_t*,double) override {return vr::VRInputError_None;}
#endif
#ifdef SURFXR_INPUT_EYE_ABI
    vr::EVRInputError CreateEyeTrackingComponent(vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t*) override {return vr::VRInputError_None;}
    vr::EVRInputError UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t,const vr::VREyeTrackingData_t*,double) override {return vr::VRInputError_None;}
#endif
};
// A distinct virtual table models runtimes that use separate version adapters.
class AlternateInput:public FakeInput {
public:
    vr::IVRDriverInput* delegate=nullptr;
    vr::EVRInputError CreateBooleanComponent(vr::PropertyContainerHandle_t container,const char* name,vr::VRInputComponentHandle_t* handle) override {
        return FakeInput::CreateBooleanComponent(container,name,handle);
    }
    vr::EVRInputError UpdateBooleanComponent(vr::VRInputComponentHandle_t handle,bool down,double offset) override {
        if(delegate)return delegate->UpdateBooleanComponent(handle,down,offset);
        return FakeInput::UpdateBooleanComponent(handle,down,offset);
    }
};
class CompatibleInput:public FakeInput {};
__attribute__((noinline)) void make(vr::IVRDriverInput* input,const char* path,vr::VRInputComponentHandle_t& handle,vr::PropertyContainerHandle_t container=1){input->CreateBooleanComponent(container,path,&handle);}
__attribute__((noinline)) void update(vr::IVRDriverInput* input,vr::VRInputComponentHandle_t handle,bool value){input->UpdateBooleanComponent(handle,value,0);}
__attribute__((noinline)) void skeletal(vr::IVRDriverInput* input){
    vr::VRInputComponentHandle_t handle=0;
    input->CreateSkeletonComponent(1,"/input/skeleton/left","/skeleton/hand/left","/pose/raw",vr::VRSkeletalTracking_Partial,nullptr,0,&handle);
    std::array<vr::VRBoneTransform_t,31> bones{};
    for(unsigned i=0;i<31;++i){bones[i].orientation.w=1;bones[i].position.v[0]=float(i)*.001f;}
    input->UpdateSkeletonComponent(handle,vr::VRSkeletalMotionRange_WithoutController,bones.data(),bones.size());
}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
}
#if defined(__aarch64__)
// Real executable forwarding adapters with non-baseline object offset and slots.
// The driver parses these instructions; tests do not supply a precomputed plan.
extern "C" void dynamic_create();extern "C" void dynamic_update();
extern "C" void dynamic_create_skeleton();extern "C" void dynamic_update_skeleton();
asm(R"(
.text
.p2align 2
.global dynamic_create
.type dynamic_create,%function
dynamic_create:
 ldr x0,[x0,#24]
 ldr x16,[x0]
 ldr x16,[x16,#16]
 br x16
.global dynamic_update
.type dynamic_update,%function
dynamic_update:
 ldr x0,[x0,#24]
 ldr x17,[x0]
 ldr x16,[x17,#32]
 br x16
.global dynamic_create_skeleton
.type dynamic_create_skeleton,%function
dynamic_create_skeleton:
 ldr x0,[x0,#24]
 ldr x16,[x0]
 ldr x17,[x16,#64]
 br x17
.global dynamic_update_skeleton
.type dynamic_update_skeleton,%function
dynamic_update_skeleton:
 ldr x0,[x0,#24]
 ldr x16,[x0]
 ldr x16,[x16,#80]
 br x16
)");
void dynamic_hook_checks(){
    FakeInput implementation;void** methods=*reinterpret_cast<void***>(&implementation);
    std::array<void*,11> target_table{};
    target_table[2]=reinterpret_cast<void*>(+[](void*,vr::PropertyContainerHandle_t,const char*,vr::VRInputComponentHandle_t* h){*h=0x100000001ull;return vr::VRInputError_None;});
    target_table[4]=reinterpret_cast<void*>(+[](void* object,vr::VRInputComponentHandle_t h,bool v,double){
        auto* values=reinterpret_cast<std::uint64_t*>(static_cast<char*>(object)+sizeof(void*));values[0]=h;values[1]=v;return vr::VRInputError_None;});
    // Never invoked in this fixture; the resolver still verifies all four ABIs.
    target_table[8]=methods[5];target_table[10]=methods[6];
    struct Target {void** table;std::uint64_t handle=0,down=0;} target{target_table.data()};
    std::array<void*,7> public_table{{reinterpret_cast<void*>(dynamic_create),reinterpret_cast<void*>(dynamic_update),
        nullptr,nullptr,nullptr,reinterpret_cast<void*>(dynamic_create_skeleton),reinterpret_cast<void*>(dynamic_update_skeleton)}};
    struct Adapter {void** table;std::uint64_t padding[2];void* target;} adapter{public_table.data(),{},&target};
    static_assert(offsetof(Adapter,target)==24);
    check(install_hooks(&adapter),"dynamic fixture public hook installed");
    const auto plan=forwarded_interface(driver_state().interfaces[0]);
    check(plan&&plan->instance==&target&&plan->slots==std::array<unsigned,4>{{2,4,8,10}},"derive real ARM object and private slots");
    check(install_hooks(&target,true),"hook dynamically relocated slots");
    const auto create=reinterpret_cast<CreateBoolean>(target_table[2]);
    const auto send=reinterpret_cast<UpdateBoolean>(target_table[4]);
    vr::VRInputComponentHandle_t handle=0;create(&target,1,"/input/system/click",&handle);
    auto& state=driver_state();state.components.back().hand=0;
    const auto now=monotonic_ns();state.policy.advance(now,3,3);state.policy.request(true,now);
    send(&target,handle,true,.125);
    check(target.handle==0x100000001ull&&!target.down&&state.policy.snapshot(now).clicks==1,"private hook captures without handle truncation");
    remove_hooks();check(target_table[4]==reinterpret_cast<void*>(plan->update),"restore dynamically located slot");
    // No concurrent callers in this fixture. Discard its stack-owned interfaces.
    state.policy=CapturePolicy{};state.interfaces={};state.interface_count=0;state.interface=nullptr;state.create=nullptr;state.update=nullptr;
}
#endif
void hook_checks(){
#if defined(__aarch64__)
    dynamic_hook_checks();
#endif

    FakeInput input;auto** original=*reinterpret_cast<void***>(&input);const auto slot0=original[0],slot1=original[1];
    check(install_hooks(&input),"install actual production vtable hooks on fake driver input");
    check(install_hooks(&input),"interface versions sharing one vtable are patched once");
    AlternateInput alternate;alternate.next=100;
    auto** second=*reinterpret_cast<void***>(&alternate);const auto second0=second[0],second1=second[1];
    check(install_hooks(&alternate),"patch second version adapter without losing the first");
    CompatibleInput compatible;
    check(install_hooks(&compatible,true),"newer adapter requires exact known boolean and skeleton functions");
    auto& state=driver_state();auto now=monotonic_ns();
    refresh_components(now,{{11,12}});
    check(!state.policy.snapshot(now).available&&!state.policy.snapshot(now).touch_available,"recovery-created components cannot enable capture without hardware registration or update");
    const auto recovered=state.components.size();
    vr::VRInputComponentHandle_t system=0,touch=0,trigger=0;
    make(&input,"/input/system/click",system);make(&input,"/input/system/touch",touch);make(&input,"/input/trigger/click",trigger);
    check(state.components.size()==recovered+2,"only system components observed");
    vr::VRInputComponentHandle_t right=0,right_touch=0;
    make(&alternate,"/input/system/click",right,2);make(&alternate,"/input/system/touch",right_touch,2);
    refresh_components(now,{{1,2}});
    check(state.policy.snapshot(now).available==3&&state.policy.snapshot(now).touch_available==3,"connected hardware registration is ready before first physical system press");
    check(state.policy.snapshot(now).clicks==0&&state.policy.snapshot(now).touches==0,"registration does not invent physical input");
    state.policy.request(true,now);
    update(&input,system,true);check(!input.value&&state.policy.snapshot(now).clicks==1,"physical press captured before runtime");
    update(&input,touch,true);check(!input.value&&state.policy.snapshot(now).touches==1,"physical touch captured");
    update(&alternate,right,true);check(!alternate.value&&state.policy.snapshot(now).clicks==3,"both version adapters observe and suppress independently");
    alternate.delegate=&input;
    update(&alternate,right,false);update(&alternate,right,true);
    check(state.policy.snapshot(now).clicks==3&&!input.value,"nested public-to-native forwarding observes once without deadlock");
    alternate.delegate=nullptr;
    skeletal(&input);
    check(input.skeleton_updates==1&&state.skeletons.size()==1&&state.skeletons[0].value.sampled_ns&&
        state.skeletons[0].value.bones[30].position[0]>.029f,"measured bones copied while local skeleton update passes through during capture");
    update(&input,trigger,true);check(input.value&&input.last==trigger,"other components pass through");
    state.policy.advance(now+lease_ns+1,3,3);release_forwarded();check(input.value,"touch restored on expiry");
    update(&input,system,false);update(&input,system,true);check(input.value,"system route restored after release");
    remove_hooks();check(original[0]==slot0&&original[1]==slot1,"Cleanup restores both original slots");
    check(second[0]==second0&&second[1]==second1,"Cleanup restores the second adapter too");
    update(&input,system,true);check(input.value,"original interface remains callable");
}
