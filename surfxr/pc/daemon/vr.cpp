#include "surfxr/ports.hpp"
#include "backend.hpp"
#include "worker.hpp"
#include "../desktop/diagnostics.hpp"
#include <charconv>
#include <cctype>
#include <cstring>

namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
namespace {
unsigned port(const Bytes& config){
    Reader r(config,true);const auto json=r.string(2*1024*1024);const auto field=json.find("\"stream_port\"");require(field!=std::string::npos,"daemon_vr_port_missing");
    auto at=json.find(':',field);require(at!=std::string::npos,"daemon_vr_port_missing");++at;while(at<json.size()&&std::isspace(static_cast<unsigned char>(json[at])))++at;
    unsigned value=0;const auto result=std::from_chars(json.data()+at,json.data()+json.size(),value);require(result.ec==std::errc{}&&value>=1024&&value<=65533,"daemon_vr_port_invalid");return value;
}
Socket connect(){
    Socket socket(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));require(socket.value!=INVALID_SOCKET,"daemon_vr_socket");
    sockaddr_in local{},peer{};local.sin_family=peer.sin_family=AF_INET;inet_pton(AF_INET,"127.0.0.2",&local.sin_addr);inet_pton(AF_INET,"127.0.0.1",&peer.sin_addr);peer.sin_port=htons(ports::vr_control);
    require(bind(socket.value,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0,"daemon_vr_local_bind");
    require(::connect(socket.value,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==0,"daemon_vr_not_running");DWORD timeout=2000;
    const BOOL nodelay=TRUE;setsockopt(socket.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&nodelay),sizeof(nodelay));
    setsockopt(socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(socket.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    return socket;
}
}
void Backend::run_vr(Bytes caps,Worker& worker){
    std::unique_ptr<Control> connection;Bytes config;unsigned video_port=0;
    const auto detach=[this]{std::lock_guard<std::mutex> lock(mutex_);control_socket_=INVALID_SOCKET;};
    struct Detach {std::function<void()> release;~Detach(){release();}} detach_control{detach};
    const auto startup=Clock::now()+std::chrono::seconds(60);
    auto next_error=Clock::now()+std::chrono::seconds(5);
    while(!stopped()&&Clock::now()<startup){
        try{connection=std::make_unique<Control>(connect());{std::lock_guard<std::mutex> lock(mutex_);control_socket_=connection->socket.value;if(stopped())shutdown(control_socket_,SD_BOTH);}connection->send(caps);config=connection->receive();video_port=port(config);break;}
        catch(const std::exception& error){if(!stopped()&&Clock::now()>=next_error){report_failure(error,"daemon_vr_handshake_failed");next_error=Clock::now()+std::chrono::seconds(5);}detach();connection.reset();for(unsigned i=0;i<25&&!stopped();++i)Sleep(20);}
    }
    require(connection&&!stopped()&&video_port,"daemon_vr_start_timeout");auto& control=*connection;require(control_(config),"daemon_control_send_failed");
    auto priority=Socket::bind("127.0.0.2",video_port+1,false,128*1024);
    const auto connect_udp=[&](Socket& socket,unsigned p){sockaddr_in peer{};peer.sin_family=AF_INET;inet_pton(AF_INET,"127.0.0.1",&peer.sin_addr);peer.sin_port=htons(static_cast<u_short>(p));require(::connect(socket.value,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==0,"daemon_vr_media_connect");};
    connect_udp(priority,video_port+1);worker.destination(video_);
    // Retirement blocks new sends and waits only in-flight synchronous sends.
    // The actual GPU owner keeps its lease until joined, without retaining this
    // Session callable during a driver stall or waiting for source cooperation.
    struct Destination {Worker& worker;~Destination(){worker.destination(0);}}destination{worker};
    const auto start=control.receive();require(control_(start),"daemon_control_send_failed");
    {Reader r(start,true);if(r.integer(4)==2){reason_.store(2);return;}}
    Bytes buffer(2*1024*1024);const auto size=receive(buffer.data(),buffer.size());require(size>0,"daemon_vr_ready_cancelled");control.send(Bytes(buffer.begin(),buffer.begin()+size));
    {std::lock_guard<std::mutex> lock(mutex_);uplink_=priority.value;}
    Handle network_ready(CreateEventW(nullptr,TRUE,FALSE,nullptr));require(network_ready.value,"daemon_vr_control_event");
    require(WSAEventSelect(control.socket.value,network_ready.value,FD_READ|FD_WRITE|FD_CLOSE)==0,"daemon_vr_nonblocking_control");
    struct Retire {Backend& self;~Retire(){std::lock_guard<std::mutex> lock(self.mutex_);self.uplink_=INVALID_SOCKET;}} retire{*this};
    while(!stopped()){
        WSANETWORKEVENTS events{};require(WSAEnumNetworkEvents(control.socket.value,network_ready.value,&events)==0,"daemon_vr_control_events");
        bool received_control=false;
        if(const auto bytes=control.poll_receive()){received_control=true;require(control_(*bytes),"daemon_control_send_failed");Reader r(*bytes,true);if(r.integer(4)==2){reason_.store(2);return;}}
        const bool writable=control.flush_send();
        if(writable&&readable(0)){const auto count=receive(buffer.data(),buffer.size());if(count>0){control.queue_send(Bytes(buffer.begin(),buffer.begin()+count));control.flush_send();}continue;}
        if(!received_control){
            // Pending writes wait for FD_WRITE rather than spinning on queued
            // local controls. A read can be partial and remains in Control.
            const HANDLE wait[]{stop_ready_.value,network_ready.value,controls_ready_.value};
            const auto result=WaitForMultipleObjects(writable?3:2,wait,FALSE,INFINITE);
            require(result!=WAIT_FAILED,"daemon_vr_control_wait");
        }
    }
}
void Backend::run_tracking(Bytes caps){
    std::unique_ptr<Control> connection;
    try{connection=std::make_unique<Control>(connect());}
    catch(const std::exception& error){
        if(std::strcmp(error.what(),"daemon_vr_not_running")!=0)throw;
        require(control_(reserved(5,"frame_session_stopped")),"daemon_tracking_reply_failed");return;
    }
    auto& control=*connection;control.send(std::move(caps));Bytes bytes(4096);
    while(!stopped()){
        if(control.socket.ready(2)){
            const auto reply=control.receive();require(control_(reply),"daemon_tracking_reply_failed");Reader r(reply,true);const auto type=r.integer(4);
            if(type==2||(type==5&&r.string(4096)=="frame_session_stopped"))return;
        }
        if(readable(2)){const auto size=receive(bytes.data(),bytes.size());if(size>0)control.send(Bytes(bytes.begin(),bytes.begin()+size));}
    }
}
}
