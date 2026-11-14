#include "driver.hpp"
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace surfxr::frame_input {
namespace {
class Provider final:public vr::IServerTrackedDeviceProvider {
    int listener_=-1,peer_=-1,lock_=-1;
    std::string path_;
    std::uint64_t next_report_=0;
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        const auto result=vr::InitServerDriverContext(context);if(result!=vr::VRInitError_None)return result;
        try{
            if(private_directory())throw 0;
            path_=socket_path();
            const auto lock_path=path_.substr(0,path_.find_last_of('/'))+"/input-driver.lock";
            lock_=open(lock_path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
            struct stat st{};
            if(lock_<0||fstat(lock_,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)||flock(lock_,LOCK_EX|LOCK_NB))throw 0;
            sockaddr_un address{};address.sun_family=AF_UNIX;
            if(path_.size()>=sizeof(address.sun_path))throw 0;
            std::memcpy(address.sun_path,path_.c_str(),path_.size()+1);
            if(!lstat(path_.c_str(),&st)){
                if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid())throw 0;
                if(unlink(path_.c_str()))throw 0;
            }else if(errno!=ENOENT)throw 0;
            listener_=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
            if(listener_<0||bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))||chmod(path_.c_str(),0600)||listen(listener_,1))throw 0;
            vr::EVRInitError error=vr::VRInitError_None;
            auto* input=context->GetGenericInterface(vr::IVRDriverInput_Version,&error);
            const bool hooked=input&&error==vr::VRInitError_None&&install_hooks(input);
            // Follow proven public adapters, not guessed private version names.
            auto& state=driver_state();unsigned resolved=0;
            for(unsigned i=0;hooked&&i<state.interface_count;++i){
                const auto target=forwarded_interface(state.interfaces[i]);
                if(!target)continue;
                const auto count=state.interface_count;
                if(install_hooks(target->instance,true)&&state.interface_count>count)++resolved;
            }
            vr::VRDriverLog()->Log(resolved?
                "[DYNAMIC_RUNTIME_ADAPTATION] Frame input: target objects and slots parsed from runtime forwarding; no private version requested":
                "[DYNAMIC_RUNTIME_ADAPTATION] Frame input: no proven private forwarding target; public SDK hook only, capture readiness must be observed");
            if(!hooked)throw 0;
            return vr::VRInitError_None;
        }catch(...){Cleanup();return vr::VRInitError_Driver_Failed;}
    }
    void Cleanup() override {
        if(peer_>=0){::close(peer_);peer_=-1;}
        if(listener_>=0){::close(listener_);listener_=-1;if(!path_.empty())unlink(path_.c_str());}
        remove_hooks();
        if(lock_>=0){::close(lock_);lock_=-1;}
        vr::CleanupDriverContext();
    }
    const char* const* GetInterfaceVersions() override {return vr::k_InterfaceVersions;}
    void RunFrame() override {
        auto now=monotonic_ns();auto& s=driver_state();std::lock_guard<std::mutex> guard(s.mutex);
        if(!s.enabled)return;
        const bool was=s.policy.snapshot(now).capture;
        refresh_devices(now);
        if(peer_<0){const int candidate=accept4(listener_,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(candidate>=0){if(same_user(candidate))peer_=candidate;else ::close(candidate);}}
        bool reply=false;
        if(peer_>=0)for(unsigned i=0;i<4;++i){
            Request request;const auto n=recv(peer_,&request,sizeof(request),MSG_DONTWAIT|MSG_TRUNC);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
            if(n!=sizeof(request)||request.tag!=magic||request.revision!=version||request.capture>1||request.reserved){
                ::close(peer_);peer_=-1;s.policy.stop();break;}
            now=monotonic_ns();
            const bool fresh=request.sampled_ns&&now>=request.sampled_ns&&now-request.sampled_ns<=lease_ns;
            if(!request.capture)s.policy.request(false,now);
            else if(fresh)s.policy.request(true,request.sampled_ns);
            reply=true;
        }
        now=monotonic_ns();
        if(now>=next_report_){next_report_=now+5'000'000'000;unsigned seen=0,registered=0,bones=0;
            for(const auto& c:s.components){if(c.seen)++seen;if(c.registered)++registered;}
            for(const auto& c:s.skeletons)if(c.value.sampled_ns)++bones;
            const auto ready=s.policy.snapshot(now);
            char message[208];std::snprintf(message,sizeof(message),"Frame input readiness: devices=%u components=%u registered=%u observed=%u click_ready=%u touch_ready=%u skeletons=%u sampled=%u",
                unsigned(bool(s.containers[0]))+unsigned(bool(s.containers[1])),unsigned(s.components.size()),registered,seen,ready.available,ready.touch_available,unsigned(s.skeletons.size()),bones);
            vr::VRDriverLog()->Log(message);}
        if(was!=bool(s.policy.snapshot(now).capture))release_forwarded();
        if(reply&&peer_>=0){auto snapshot=s.policy.snapshot(now);
            for(unsigned hand=0;hand<2;++hand)if(s.containers[hand])
                for(const auto& c:s.skeletons)if(c.container==s.containers[hand]&&c.value.sampled_ns&&
                    now>=c.value.sampled_ns&&now-c.value.sampled_ns<=fresh_ns)snapshot.skeletons[hand]=c.value;
            if(send(peer_,&snapshot,sizeof(snapshot),MSG_DONTWAIT|MSG_NOSIGNAL)!=sizeof(snapshot)){
                ::close(peer_);peer_=-1;s.policy.stop();release_forwarded();}}
    }
    bool ShouldBlockStandbyMode() override {return false;}
    void EnterStandby() override {auto& s=driver_state();std::lock_guard<std::mutex> guard(s.mutex);s.policy.stop();release_forwarded();}
    void LeaveStandby() override {}
};
Provider provider;
}
}
extern "C" __attribute__((visibility("default"))) void* HmdDriverFactory(const char* version,int* error){
    if(version&&!std::strcmp(version,vr::IServerTrackedDeviceProvider_Version))return &surfxr::frame_input::provider;
    if(error)*error=vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
