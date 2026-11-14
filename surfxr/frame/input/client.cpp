#include "surfxr/system_input.hpp"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace surfxr::frame_input {
SystemInputClient::~SystemInputClient(){close();}
void SystemInputClient::close() noexcept {
    auto_pending_=false;disconnect();
}
void SystemInputClient::suspend() noexcept {disconnect();}
void SystemInputClient::session(bool active) noexcept {
    if(active==session_active_)return;
    close();session_active_=active;auto_pending_=active;retry_ns_=0;
}
void SystemInputClient::disconnect() noexcept {
    if(socket_>=0){Request request;request.sampled_ns=monotonic_ns();::send(socket_,&request,sizeof(request),MSG_DONTWAIT|MSG_NOSIGNAL);::close(socket_);}
    socket_=-1;requested_=false;have_snapshot_=false;latest_={};awaiting_=false;pending_since_=0;
}
Snapshot SystemInputClient::poll(bool eligible,std::uint64_t now) noexcept {
    if(!eligible)requested_=false;
    if(socket_<0&&now>=retry_ns_)try{
        retry_ns_=now+1'000'000'000;
        if(private_directory())return {};
        const auto path=socket_path();sockaddr_un address{};address.sun_family=AF_UNIX;
        if(path.size()>=sizeof(address.sun_path))return {};
        std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        socket_=::socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(socket_<0)return {};
        if(connect(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))||!same_user(socket_)){disconnect();return {};}
    }catch(...){disconnect();return {};}
    if(socket_<0)return {};
    for(unsigned i=0;i<4;++i){
        Snapshot value;const auto n=recv(socket_,&value,sizeof(value),MSG_DONTWAIT|MSG_TRUNC);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
        if(n!=sizeof(value)||value.tag!=magic||value.revision!=version||value.reserved||
           value.available>3||value.touch_available>3||value.clicks>3||value.touches>3||value.capture>1||
           !valid_skeleton(value.skeletons[0])||!valid_skeleton(value.skeletons[1])){close();return {};}
        if(have_snapshot_&&value.exit_serial!=last_exit_){requested_=false;auto_pending_=false;}
        awaiting_=false;
        last_exit_=value.exit_serial;latest_=value;have_snapshot_=true;
    }
    now=monotonic_ns();
    if(awaiting_&&now>=pending_since_&&now-pending_since_>fresh_ns){close();return {};}
    const bool fresh=have_snapshot_&&latest_.sampled_ns&&now>=latest_.sampled_ns&&now-latest_.sampled_ns<=fresh_ns;
    if(have_snapshot_&&!fresh){close();return {};}
    // Registration can precede the first physical update. Wait for a fresh
    // driver acknowledgement and released clicks before this one entry.
    if(auto_pending_&&eligible&&fresh&&latest_.available==3&&latest_.touch_available==3&&!latest_.clicks){
        requested_=true;auto_pending_=false;
    }
    Request request;request.capture=requested_&&eligible;request.sampled_ns=now;
    if(!awaiting_||last_request_!=bool(request.capture)){
        const auto n=send(socket_,&request,sizeof(request),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK){close();return {};}
        if(n==sizeof(request)){awaiting_=true;pending_since_=now;last_request_=request.capture;}
    }
    if(!fresh)return {};
    auto result=latest_;if(!requested_||!eligible)result.capture=0;
    for(auto& skeleton:result.skeletons)if(!skeleton.sampled_ns||now<skeleton.sampled_ns||now-skeleton.sampled_ns>fresh_ns)skeleton={};
    return result;
}
}
