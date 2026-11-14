#include "steamvr_boundary.hpp"
#include "steamvr_boundary_ipc.hpp"
#include "boundary_sampling.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace surfxr::xr_detail {
struct SteamVrBoundary::State {
    int socket=-1;
    pid_t child=-1;
    BoundarySampling sampling;
    std::uint64_t sent_generation=0;
    std::uint32_t sample_count=0;
    IpdSnapshot ipd;
    RefreshRequest refresh_request;
    RefreshSnapshot refresh;
    ControllerStatus controllers;
    HeadsetBatteryStatus headset_battery;
    std::uint64_t refresh_sent=0;
    std::shared_ptr<const StageBoundary> latest;
    void disconnect(){if(socket>=0)::close(socket);socket=-1;latest.reset();ipd.meters=0;controllers={};headset_battery={};}
    ~State(){
        disconnect();
        if(child<=0)return;
        // EOF normally exits the worker. Bound shutdown even if OpenVR stalls.
        for(unsigned i=0;i<25;++i){
            const auto result=::waitpid(child,nullptr,WNOHANG);
            if(result==child||(result<0&&errno==ECHILD))return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(child,SIGKILL);
        while(::waitpid(child,nullptr,0)<0&&errno==EINTR){}
    }
};
SteamVrBoundary::SteamVrBoundary():state_(std::make_unique<State>()){
#ifdef SURFXR_HAVE_OPENVR_REFRESH
    int endpoints[2];
    if(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,endpoints))return;
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)){::close(endpoints[0]);::close(endpoints[1]);return;}
    const bool ready=posix_spawn_file_actions_adddup2(&actions,endpoints[1],STDIN_FILENO)==0&&
        posix_spawn_file_actions_addclose(&actions,endpoints[0])==0&&
        posix_spawn_file_actions_addclose(&actions,endpoints[1])==0&&
        posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0)==0&&
        posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0)==0;
    // SteamVR keys background clients by executable path. Re-executing the
    // viewer steals its scene application's PID and breaks per-app video UI.
    const auto executable=(std::filesystem::read_symlink("/proc/self/exe").parent_path()/"surfxr-runtime-bridge").string();
    char name[]="surfxr-runtime-bridge";char* args[]={name,nullptr};
    pid_t child=-1;
    const auto result=ready?posix_spawn(&child,executable.c_str(),&actions,nullptr,args,environ):-1;
    posix_spawn_file_actions_destroy(&actions);::close(endpoints[1]);
    if(result){::close(endpoints[0]);return;}
    state_->socket=endpoints[0];state_->child=child;
#endif
}
SteamVrBoundary::~SteamVrBoundary()=default;
std::uint32_t SteamVrBoundary::sample_count()const{return state_->sample_count;}
float SteamVrBoundary::ipd_meters()const{return state_->ipd.meters;}
std::uint32_t SteamVrBoundary::ipd_samples()const{return state_->ipd.samples;}
std::uint32_t SteamVrBoundary::ipd_events()const{return state_->ipd.events;}
bool SteamVrBoundary::request_refresh(float hz){
    if(state_->socket<0)return false;
    state_->refresh_request.hz=hz;++state_->refresh_request.serial;return true;
}
RefreshSnapshot SteamVrBoundary::refresh()const{return state_->refresh;}
ControllerStatus SteamVrBoundary::controllers()const{return state_->controllers;}
HeadsetBatteryStatus SteamVrBoundary::headset_battery()const{return state_->headset_battery;}
std::shared_ptr<const StageBoundary> SteamVrBoundary::snapshot(std::uint64_t epoch,bool ready){
    auto& s=*state_;
    if(s.sampling.update(epoch,ready))s.latest.reset();
    if(s.socket<0)return {};
    for(unsigned i=0;i<4;++i){
        std::array<unsigned char,sizeof(BoundarySnapshot)> bytes{};
        const auto count=::recv(s.socket,bytes.data(),bytes.size(),MSG_DONTWAIT|MSG_TRUNC);
        if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
        if(count==sizeof(HeadsetBatteryStatus)){
            HeadsetBatteryStatus status;std::memcpy(&status,bytes.data(),sizeof(status));
            if(!status.valid()){s.disconnect();return {};}
            s.headset_battery=status;continue;
        }
        if(count==sizeof(ControllerStatus)){
            ControllerStatus status;std::memcpy(&status,bytes.data(),sizeof(status));
            if(!status.valid()){s.disconnect();return {};}
            s.controllers=status;continue;
        }
        if(count==sizeof(RefreshSnapshot)){
            RefreshSnapshot refresh;std::memcpy(&refresh,bytes.data(),sizeof(refresh));
            if(refresh.magic!=0x52465331||!std::isfinite(refresh.actual)||refresh.actual<0||refresh.actual>1000){s.disconnect();return {};}
            s.refresh=refresh;continue;
        }
        if(count==sizeof(IpdSnapshot)){
            IpdSnapshot ipd;std::memcpy(&ipd,bytes.data(),sizeof(ipd));
            if(ipd.magic!=0x49504431||!std::isfinite(ipd.meters)||ipd.meters<0||ipd.meters>=.2f){s.disconnect();return {};}
            s.ipd=ipd;continue;
        }
        if(count!=sizeof(BoundarySnapshot)){s.disconnect();return {};}
        BoundarySnapshot packet;std::memcpy(&packet,bytes.data(),sizeof(packet));
        if(packet.count>StageBoundary::max_quads){s.disconnect();return {};}
        s.sample_count=packet.sample_count;
        if(!s.sampling.accepts(packet.token))continue;
        s.latest.reset();
        auto value=std::make_shared<StageBoundary>();value->origin_epoch=epoch;
        value->width=packet.width;value->depth=packet.depth;
        value->quads.assign(packet.quads.begin(),packet.quads.begin()+packet.count);
        if(valid_stage_boundary(*value))s.latest=std::move(value);
    }
    if(s.sent_generation!=s.sampling.generation()){
        const auto token=s.sampling.ready()?s.sampling.generation():0;
        const auto count=::send(s.socket,&token,sizeof(token),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count==sizeof(token))s.sent_generation=s.sampling.generation();
        else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)s.disconnect();
    }
    if(s.socket>=0&&s.refresh_sent!=s.refresh_request.serial){
        const auto count=::send(s.socket,&s.refresh_request,sizeof(s.refresh_request),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(count==sizeof(s.refresh_request))s.refresh_sent=s.refresh_request.serial;
        else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)s.disconnect();
    }
    return s.latest;
}
}
