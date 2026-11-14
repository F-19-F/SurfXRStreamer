#include "network.hpp"
#include "video.hpp"
#include "input.hpp"
#include "audio.hpp"
#include "diagnostics.hpp"
#include "dashboard.hpp"
#include "media_lease.hpp"
#include "host_session.hpp"
#include "surfxr/surfxr_desktop_capture.hpp"
#include "surfxr/windows_timer_period.hpp"
#include "surfxr/surfxr_stream.hpp"
#include <condition_variable>
#include <iostream>
#include <thread>
#include <optional>
#include <objbase.h>

namespace surfxr::transport::desktop {
namespace {
void discovery(Socket& socket,const Options& o){
    char query[128];sockaddr_in peer{};int length=sizeof(peer);const auto n=recvfrom(socket.value,query,sizeof(query),0,reinterpret_cast<sockaddr*>(&peer),&length);
    if(n!=16||std::memcmp(query,"ALVRFD01",8))return;
    Bytes reply{'A','L','V','R','P','C','0','1'};reply.insert(reply.end(),reinterpret_cast<std::uint8_t*>(query)+8,reinterpret_cast<std::uint8_t*>(query)+16);
    const std::string version="21-dev14",name="Steam Frame SURFXR Desktop";reply.insert(reply.end(),version.begin(),version.end());
    reply.push_back(static_cast<std::uint8_t>(o.port));reply.push_back(static_cast<std::uint8_t>(o.port>>8));reply.push_back(static_cast<std::uint8_t>(name.size()));reply.insert(reply.end(),name.begin(),name.end());
    sendto(socket.value,reinterpret_cast<const char*>(reply.data()),static_cast<int>(reply.size()),0,reinterpret_cast<sockaddr*>(&peer),length);
}
void audio_worker(const Options& o,sockaddr_in peer,const Key& key,State& state){
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        Audio audio;auto socket=Socket::bind(o.bind,o.port+2,false);peer.sin_port=htons(static_cast<u_short>(o.port+2));
        {std::lock_guard<std::mutex> lock(state.mutex);state.audio_initialized=true;state.audio_available=true;state.ready.notify_one();}
        while(!state.stop.load()&&!state.stream_ready.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Cipher cipher(key);std::uint32_t index=0;
        while(!state.stop.load()){
            auto pcm=audio.poll();if(!pcm.empty())for(const auto& shard:surfxr::transport::shards(2,index++,pcm,packet_size,true)){
                auto record=cipher.seal(shard);const auto sent=sendto(socket.value,reinterpret_cast<const char*>(record.data()),static_cast<int>(record.size()),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer));require(sent==static_cast<int>(record.size()),"desktop_audio_send_failed");
            }std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::lock_guard<std::mutex> lock(state.mutex);state.audio_initialized=true;state.ready.notify_one();}
    if(SUCCEEDED(com))CoUninitialize();
}
void session(Options o,Socket accepted,sockaddr_in peer,Key root,const std::atomic_bool& stop){
    DWORD timeout=2000;setsockopt(accepted.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(accepted.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    BOOL nodelay=TRUE;setsockopt(accepted.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&nodelay),sizeof(nodelay));
    Control control(std::move(accepted));std::optional<Ticket> ticket;
    if(o.dashboard_managed){ticket=consume_ticket(control,peer);root=ticket->key;o.fps=ticket->fps;o.bitrate=ticket->bitrate;}
    Key server{},client{};authenticate(control,root,server,client);const bool multi=verify_capabilities(control.receive());
    MediaLease media_lease; // Released only after all capture/encoder/sender owners join.
    auto media=Socket::bind(o.bind,o.port+1,false);peer.sin_port=htons(static_cast<u_short>(o.port+1));
    DWORD send_timeout=100;setsockopt(media.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&send_timeout),sizeof(send_timeout));
    run_media_session(o,control,std::move(media),peer,proof(root,server,client,"ALVR-DESKTOP-media-s2c-v1"),proof(root,server,client,"ALVR-DESKTOP-audio-s2c-v1"),multi,ticket,stop);
}
}
void answer_discovery(Socket& socket,const Options& options){discovery(socket,options);}
void run_media_session(const Options& o,Control& control,Socket media,sockaddr_in peer,const Key& video_key,const Key& audio_key,
        bool multi,const std::optional<Ticket>& ticket,const std::atomic_bool& stop){
    std::atomic_bool prefer_pc{ticket?dashboard_pointer_policy(*ticket,peer):false};
    State state;if(ticket)state.requested_count.store(std::max(1u,dashboard_monitors(*ticket,peer)));
    if(o.monitors)state.requested_count.store(std::max(1u,o.monitors(0)));
    std::thread worker(video_worker,std::cref(o),std::ref(control),std::move(media),peer,video_key,std::ref(state),multi);
    std::thread sound,policy;
    struct Join {State& s;std::thread &t,&audio,&policy;~Join(){s.stop.store(true);if(t.joinable())t.join();if(audio.joinable())audio.join();if(policy.joinable())policy.join();}} join{state,worker,sound,policy};
    if(ticket)policy=std::thread([&]{
        DashboardWatch watch(*ticket,peer);
        while(!state.stop.load()){
            if(watch.revoked()){state.stop.store(true);shutdown(control.socket.value,SD_BOTH);break;}
            if(!dashboard_announce()||!dashboard_lease(*ticket,peer,state.stream_ready.load())){state.stop.store(true);break;}
            prefer_pc.store(dashboard_pointer_policy(*ticket,peer,prefer_pc.load()));
            for(unsigned i=0;i<50&&!state.stop.load()&&!watch.revoked();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    if(!o.embedded&&(!ticket||ticket->audio))sound=std::thread(audio_worker,std::cref(o),peer,audio_key,std::ref(state));
    else {std::lock_guard<std::mutex> lock(state.mutex);state.audio_initialized=true;state.ready.notify_one();}
    {std::unique_lock<std::mutex> lock(state.mutex);require(state.ready.wait_for(lock,std::chrono::seconds(10),[&]{return (state.initialized&&state.audio_initialized)||state.error;}),"desktop_encoder_start_timeout");if(state.error)std::rethrow_exception(state.error);}
    control.send(configuration(state.layout.streams.front().display,o.fps,o.port+1,state.audio_available,multi));control.send(desktop::control(0));
    const auto primary=state.layout.streams.front().display;
    auto b=control.receive();Reader ready(b,true);require(ready.integer(4)==3,"desktop_stream_ready_expected");ready.end();state.stream_ready.store(true);
    std::optional<CaptureRequest> capture_pending;
    Input input(primary);unsigned input_generation=0;auto last_received=Clock::now(),next_keepalive=Clock::now();
    while(!stop.load()&&!state.stop.load()){
        Layout active;{std::lock_guard<std::mutex> lock(state.mutex);active=state.layout;}
        if(input_generation!=active.generation){std::vector<Display> displays;for(const auto& stream:active.streams)displays.push_back(stream.display);input.displays(displays);input_generation=active.generation;}
        input.prefer_pc(o.prefer_pc_mouse?o.prefer_pc_mouse():prefer_pc.load());
        input.tick();require(Clock::now()-last_received<std::chrono::seconds(3),"desktop_input_lease_expired");
        for(unsigned batch=0;batch<64&&!stop.load()&&!state.stop.load()&&control.socket.ready(batch?0:2);++batch){auto bytes=control.receive();Reader r(bytes,true);const auto type=r.integer(4);
            if(type==1){r.end();state.idr.store(true);}else if(type==2){r.end();}
            else if(type==11&&!multi){const auto data=r.blob(64);r.end();const auto e=event(data);if((o.allow_input&&(!ticket||ticket->input))||e.kind==EventKind::Release)input.apply(e);}
            else if(type==input_type&&multi){const auto generation=r.integer(4);const auto data=r.blob(64);r.end();const auto e=event(data);std::lock_guard<std::mutex> lock(state.mutex);if(((o.allow_input&&(!ticket||ticket->input))||e.kind==EventKind::Release)&&((generation==input_generation&&generation==state.layout.generation)||e.kind==EventKind::Release))input.apply(e);}
            else if(type==capture_type&&multi){const auto request=capture_record(bytes);input.release();
                if(request.generation==active.generation){state.capture_requested.store(request.mask);capture_pending=request;}}
            else if(type==monitor_type&&multi){const bool add=r.boolean();r.end();input.release();const auto count=static_cast<unsigned>(active.streams.size());
                const auto requested=add?std::min(count+1,active.capacity):std::max(count-1,1u);state.requested_count.store(requested);
                if(ticket&&!dashboard_monitors(*ticket,peer,requested))report_event("desktop_monitor_preference_failed");
                if(o.monitors&&!o.monitors(requested))report_event("desktop_monitor_preference_failed");}
            else fail("desktop_unexpected_client_control");last_received=Clock::now();
        }
        if(capture_pending&&state.capture_generation.load()==capture_pending->generation&&state.capture_applied.load()==capture_pending->mask){
            control.send(capture_record(*capture_pending,true));capture_pending.reset();
        }
        if(Clock::now()>=next_keepalive){control.send(desktop::control(3));next_keepalive=Clock::now()+std::chrono::seconds(1);}
    }
    input.release();control.send(reserved(5,"frame_session_stopped"));
    {std::lock_guard<std::mutex> lock(state.mutex);if(state.error)std::rethrow_exception(state.error);}
}
int run_host(const Options& o,const std::atomic_bool& stop){
    surfxr::WindowsTimerPeriod timer_period;
    WSADATA startup{};require(WSAStartup(MAKEWORD(2,2),&startup)==0,"desktop_winsock_start");struct Cleanup {~Cleanup(){WSACleanup();}} cleanup;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto root=o.dashboard_managed?Key{}:read_key(o.key_file);auto listener=Socket::bind(o.bind,o.port,true);auto announce=Socket::bind(o.bind,o.port,false);
    auto next_dashboard=Clock::now();
    while(!stop.load()){
        if(o.dashboard_managed&&Clock::now()>=next_dashboard){next_dashboard=Clock::now()+std::chrono::milliseconds(500);if(!dashboard_announce()&&!o.agent)return 0;}
        if(announce.ready(0))discovery(announce,o);
        if(!listener.ready(20))continue;sockaddr_in peer{};int length=sizeof(peer);Socket accepted(accept(listener.value,reinterpret_cast<sockaddr*>(&peer),&length));if(accepted.value==INVALID_SOCKET)continue;
        try{session(o,std::move(accepted),peer,root,stop);}catch(const std::exception& e){
            std::cerr<<e.what()<<'\n';
            // A hidden SYSTEM agent has no console. Preserve only a bounded
            // local check identifier, never input, keys or protocol contents.
            std::string code=e.what();
            if(code.empty()||code.size()>96||code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")!=std::string::npos)code="desktop_host_failed";
            report_event(code.c_str());
        }
    }return 0;
}
}
