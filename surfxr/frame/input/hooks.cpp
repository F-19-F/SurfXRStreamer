#include "driver.hpp"
#include "surfxr/runtime_memory.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/mman.h>
#include <unistd.h>

namespace surfxr::frame_input {
DriverState& driver_state(){static DriverState state;return state;}
void remember_component(void* input,vr::PropertyContainerHandle_t container,const char* path,vr::VRInputComponentHandle_t handle,bool registered){
    if(!handle||!path||(std::strcmp(path,"/input/system/click")&&std::strcmp(path,"/input/system/touch")))return;
    auto& s=driver_state();
    for(auto& c:s.components)if(c.handle==handle){c.registered|=registered;return;}
    if(s.components.size()>=64){
        const auto stale=std::find_if(s.components.begin(),s.components.end(),[](const auto& c){return c.hand<0;});
        if(stale==s.components.end())return;
        s.components.erase(stale);
    }
    s.components.push_back({container,handle,input,-1,std::strcmp(path,"/input/system/touch")==0,false,false,registered});
}
const HookInterface* original_interface(void* input){
    if(!input)return nullptr;
    const auto table=*reinterpret_cast<void***>(input);const auto& s=driver_state();
    for(unsigned i=0;i<s.interface_count;++i)if(s.interfaces[i].table==table)return &s.interfaces[i];
    return nullptr;
}
namespace {
vr::EVRInputError create(void* input,vr::PropertyContainerHandle_t container,const char* path,vr::VRInputComponentHandle_t* handle){
    if(forwarding_depth){const auto* api=original_interface(input);return api?api->create(input,container,path,handle):vr::VRInputError_InvalidHandle;}
    auto& s=driver_state();std::lock_guard<std::mutex> lock(s.mutex);
    const auto* api=original_interface(input);if(!api)return vr::VRInputError_InvalidHandle;
    ForwardCall call;const auto result=api->create(input,container,path,handle);
    if(s.enabled&&result==vr::VRInputError_None&&handle)remember_component(input,container,path,*handle,true);
    return result;
}
vr::EVRInputError update(void* input,vr::VRInputComponentHandle_t handle,bool value,double offset){
    if(forwarding_depth){const auto* api=original_interface(input);return api?api->update(input,handle,value,offset):vr::VRInputError_InvalidHandle;}
    auto& s=driver_state();std::lock_guard<std::mutex> lock(s.mutex);
    const auto* api=original_interface(input);if(!api)return vr::VRInputError_InvalidHandle;
    if(s.enabled)for(auto& c:s.components)if(c.handle==handle){
        c.input=input;c.down=value;c.seen=true;
        if(c.hand>=0){
            const bool was=s.policy.snapshot(0).capture;
            value=s.policy.update(static_cast<unsigned>(c.hand),c.touch,value,monotonic_ns());
            if(was!=bool(s.policy.snapshot(0).capture))release_forwarded();
        }
        break;
    }
    ForwardCall call;return api->update(input,handle,value,offset);
}
// Public slots come from the SDK; forwarded slots are derived from code.
// Other virtual methods pass through unchanged.
// Preserve the original mapping protections, including executable mappings.
bool patch(void** table,const HookInterface& api,bool install){
    const auto page_size=static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    if(!page_size)return false;
    const auto address=reinterpret_cast<std::uintptr_t>(table),page=address&~(page_size-1);
    const auto last=*std::max_element(api.slots.begin(),api.slots.end());
    if(last>=256||(address+sizeof(void*)*(last+1)-1)/page_size!=address/page_size)return false;
    std::ifstream maps("/proc/self/maps");std::string line;int protection=-1;
    while(std::getline(maps,line)){
        std::istringstream stream(line);std::string range,permissions;stream>>range>>permissions;
        const auto dash=range.find('-');if(dash==std::string::npos||permissions.size()<3)continue;
        const auto start=std::stoull(range.substr(0,dash),nullptr,16),end=std::stoull(range.substr(dash+1),nullptr,16);
        if(page>=start&&page+page_size<=end){protection=(permissions[0]=='r'?PROT_READ:0)|(permissions[1]=='w'?PROT_WRITE:0)|(permissions[2]=='x'?PROT_EXEC:0);break;}
    }
    const auto& slots=api.slots;
    const std::array<void*,4> originals{reinterpret_cast<void*>(api.create),reinterpret_cast<void*>(api.update),
        reinterpret_cast<void*>(api.create_skeleton),reinterpret_cast<void*>(api.update_skeleton)};
    const std::array<void*,4> hooks{reinterpret_cast<void*>(create),reinterpret_cast<void*>(update),
        reinterpret_cast<void*>(create_skeleton),reinterpret_cast<void*>(update_skeleton)};
    const auto& expected=install?originals:hooks;const auto& replacements=install?hooks:originals;
    if(protection<0)return false;
    for(unsigned i=0;i<slots.size();++i)if(__atomic_load_n(table+slots[i],__ATOMIC_ACQUIRE)!=expected[i])return false;
    if(mprotect(reinterpret_cast<void*>(page),page_size,protection|PROT_WRITE))return false;
    for(unsigned i=0;i<slots.size();++i)__atomic_store_n(table+slots[i],replacements[i],__ATOMIC_RELEASE);
    // This page was just made writable; restoration uses the same validated
    // address and size. A failed restore leaves callable slots, never nulls.
    return mprotect(reinterpret_cast<void*>(page),page_size,protection)==0;
}
}
bool install_hooks(void* interface,bool require_known_abi){
    if(!interface)return false;
    auto& s=driver_state();std::lock_guard<std::mutex> lock(s.mutex);
    RuntimeMemory read;HookInterface candidate;candidate.instance=interface;
    if(!read(reinterpret_cast<std::uintptr_t>(interface),&candidate.table,sizeof(candidate.table)))return false;
    auto** table=candidate.table;
    unsigned index=0;while(index<s.interface_count&&s.interfaces[index].table!=table)++index;
    if(index==s.interface_count){
        if(index==s.interfaces.size())return false;
        bool compatible=!require_known_abi,parsed=false;
        if(require_known_abi)for(unsigned i=0;i<s.interface_count;++i)
            if(const auto dynamic=forwarded_interface(s.interfaces[i]);dynamic&&dynamic->instance==interface){candidate=*dynamic;compatible=parsed=true;break;}
        if(!parsed){
            std::array<void*,4> entries{};
            for(unsigned i=0;i<entries.size();++i)
                if(!read(reinterpret_cast<std::uintptr_t>(table+candidate.slots[i]),&entries[i],sizeof(void*))||!entries[i])return false;
            candidate.create=reinterpret_cast<CreateBoolean>(entries[0]);candidate.update=reinterpret_cast<UpdateBoolean>(entries[1]);
            candidate.create_skeleton=reinterpret_cast<CreateSkeleton>(entries[2]);candidate.update_skeleton=reinterpret_cast<UpdateSkeleton>(entries[3]);
            if(require_known_abi)for(unsigned i=0;i<s.interface_count;++i){const auto& known=s.interfaces[i];
                compatible|=candidate.create==known.create&&candidate.update==known.update&&candidate.create_skeleton==known.create_skeleton&&
                    candidate.update_skeleton==known.update_skeleton&&candidate.slots==known.slots;
            }
        }
        if(!compatible)return false;
        s.interfaces[index]=candidate;++s.interface_count;
    }
    const auto& api=s.interfaces[index];
    if(s.enabled&&table[api.slots[0]]==reinterpret_cast<void*>(create)&&table[api.slots[1]]==reinterpret_cast<void*>(update))return true;
    if(!patch(table,api,true)){
        patch(table,api,false);return false;
    }
    if(!s.enabled){s.interface=interface;s.create=api.create;s.update=api.update;}
    s.enabled=true;return true;
}
void release_forwarded(){
    auto& s=driver_state();if(!s.update)return;
    ForwardCall call;
    for(const auto& c:s.components)if(c.hand>=0)if(const auto* api=original_interface(c.input))
        api->update(c.input,c.handle,s.policy.forwarded(static_cast<unsigned>(c.hand),c.touch),0);
}
void remove_hooks(){
    auto& s=driver_state();std::lock_guard<std::mutex> lock(s.mutex);
    s.enabled=false;s.policy.stop();release_forwarded();
    for(unsigned i=0;i<s.interface_count;++i){const auto& api=s.interfaces[i];
        patch(api.table,api,false);}
    s.components.clear();s.skeletons.clear();s.containers={};
    // Original function pointers and NODELETE code remain callable by a thread
    // that fetched a hook immediately before restoration.
}
}
