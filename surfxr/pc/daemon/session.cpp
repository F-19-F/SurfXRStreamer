#include "runtime.hpp"
#include "backend.hpp"
#include "../desktop/dashboard.hpp"
#include "../desktop/diagnostics.hpp"
#include "surfxr/surfxr_unified.hpp"
#include "surfxr/surfxr_unified_media.hpp"
#include "surfxr/surfxr_stream.hpp"
#include "surfxr/windows_timer_period.hpp"
#include <iostream>

namespace surfxr::daemon {
using namespace surfxr::transport;using namespace surfxr::transport::desktop;
using unified::Kind;
namespace {
class Session {
public:
    Session(const Options& options,Socket accepted,sockaddr_in peer,Socket& video,Socket& priority,Runtime& runtime,Worker& worker,const std::atomic_bool& stop):
        options_(options),control_(std::move(accepted)),peer_(peer),video_(video),priority_(priority),runtime_(runtime),worker_(worker),external_stop_(stop){}
    ~Session(){closing_.store(true);keyboard_.reset();if(tracker_)tracker_->stop();if(backend_)backend_->stop();if(policy_.joinable())policy_.join();if(receiver_.joinable())receiver_.join();audio_.reset();runtime_.qos_stop(video_qos_);runtime_.qos_stop(audio_qos_);}
    void run(){
        DWORD timeout=2000;setsockopt(control_.socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(control_.socket.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        const BOOL nodelay=TRUE;setsockopt(control_.socket.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&nodelay),sizeof(nodelay));
        ticket_=consume_ticket(control_,peer_);require(ticket_.unified,"daemon_unified_ticket_required");
        DashboardWatch dashboard_watch(ticket_,peer_);
        require(!dashboard_watch.revoked(),"daemon_dashboard_lease_revoked");
        prefer_pc_mouse_.store(dashboard_pointer_policy(ticket_,peer_));
        monitor_count_.store(std::max(1u,dashboard_monitors(ticket_,peer_)));
        Key server{},client{};authenticate(control_,ticket_.key,server,client);
        auto destination=peer_;destination.sin_port=htons(static_cast<u_short>(options_.port+1));require(::connect(video_.value,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==0,"daemon_video_destination");
        destination.sin_port=htons(static_cast<u_short>(options_.port+2));require(::connect(priority_.value,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==0,"daemon_audio_destination");
        video_qos_=runtime_.qos(video_.value,false);audio_qos_=runtime_.qos(priority_.value,true);
        video_cipher_=std::make_unique<unified::MediaCipher>(proof(ticket_.key,server,client,"ALVR-UNIFIED-video-s2c-v1"));
        audio_cipher_=std::make_unique<unified::MediaCipher>(proof(ticket_.key,server,client,"ALVR-UNIFIED-audio-s2c-v1"));
        incoming_=std::make_unique<unified::MediaCipher>(proof(ticket_.key,server,client,"ALVR-UNIFIED-priority-c2s-v1"));
        const auto config=dashboard_audio(ticket_,peer_);
        audio_=std::make_unique<AudioSession>(config,runtime_.audio,runtime_.microphone,[this](const Bytes& pcm){
            if(stopped()||!media_ready_.load())return true;
            for(const auto& shard:shards(2,audio_index_++,pcm,packet_size,true))if(!send(priority_,*audio_cipher_,audio_mutex_,audio_record_,0,shard.data(),shard.size()))return false;return true;
        },[this]{return dashboard_audio(ticket_,peer_);});
        report_event("daemon_session_authenticated",audio_->flags(),EVENTLOG_INFORMATION_TYPE);
        const auto media_flags=ticket_.media_modes?(unified::media_negotiation_flag|(audio_->media_aes()?unified::media_aes_flag:0)):0;
        Writer hello;hello.integer(options_.port+1,2);hello.integer(options_.port+2,2);hello.integer(audio_->flags()|media_flags,1);control_.send(unified::record(Kind::Hello,0,hello.bytes));
        receiver_=std::thread([this]{receive_priority();});
        policy_=std::thread([this]{
            try{auto healthy=Clock::now();bool stalled=false;while(!stopped()){
                const auto mode=mode_.load();
                dashboard_announce(true,options_.port,true);const auto status=dashboard_lease_status(ticket_,peer_,mode!=0,mode==2);
                if(status==0){if(stalled)report_duration("daemon_dashboard_lease_recovered_ms",healthy);healthy=Clock::now();stalled=false;}
                else if(status==2||Clock::now()-healthy>=unified::stall_timeout){
                    report_event(status==2?"daemon_dashboard_lease_revoked":"daemon_dashboard_lease_expired",status);
                    closing_.store(true);break;
                }
                else if(!stalled){report_event("daemon_dashboard_lease_grace",status,EVENTLOG_WARNING_TYPE);stalled=true;}
                control_.send(unified::record(Kind::KeepAlive,0));
                prefer_pc_mouse_.store(dashboard_pointer_policy(ticket_,peer_,prefer_pc_mouse_.load()));
                Bytes request;{std::lock_guard<std::mutex> lock(settings_mutex_);request.swap(settings_request_);}
                if(!request.empty()){const auto reply=dashboard_settings(ticket_,peer_,request);if(!reply.empty())control_.send(unified::record(Kind::Settings,0,reply));}
                for(unsigned i=0;i<25&&!stopped();++i)Sleep(20);
            }}catch(const std::exception& error){report_failure(error,"daemon_policy_failed");closing_.store(true);}
            catch(...){report_event("daemon_policy_failed");closing_.store(true);}
        });
        auto last=Clock::now();bool reported=false;
        while(!stopped()){
            if(dashboard_watch.revoked()){
                closing_.store(true);shutdown(control_.socket.value,SD_BOTH);
                report_event("daemon_dashboard_lease_revoked");break;
            }
            // Keep partial authenticated records in the existing bounded
            // framer, so a suspended sender cannot hit the handshake's 2s read.
            if(const auto bytes=control_.poll_receive()){
                const auto record=unified::parse(*bytes);last=Clock::now();
                if(record.generation&&record.generation<generation_.load())continue;
                if(record.kind==Kind::Settings){
                    require(record.generation==0&&record.body.size()==25,"daemon_settings_request_invalid");
                    std::lock_guard<std::mutex> lock(settings_mutex_);settings_request_=record.body;
                }else if(record.kind==Kind::Hello){
                    require(record.generation==0&&desktop_caps_.empty()&&vr_caps_.empty(),"daemon_capabilities_already_negotiated");Reader r(record.body);desktop_caps_=r.blob(65536);vr_caps_=r.blob(65536);
                    auto protection=unified::Protection::AesGcm;
                    if(r.remaining()){
                        require(ticket_.media_modes&&r.remaining()==2&&r.integer(1)==1,"daemon_media_negotiation_invalid");const auto mode=r.integer(1);
                        require(mode<=1&&(!audio_->media_aes()||mode==1),"daemon_media_negotiation_invalid");protection=static_cast<unified::Protection>(mode);
                    }
                    r.end();verify_capabilities(desktop_caps_,&keyboard_capable_,&candidate_click_,&desktop_keyboard_);
                    keyboard_capable_=keyboard_capable_&&ticket_.input;
                    desktop_keyboard_=desktop_keyboard_&&keyboard_capable_;
                    if(keyboard_capable_){Bytes capabilities{static_cast<std::uint8_t>(candidate_click_&&runtime_.candidate_update?2:1)};
                        if(desktop_keyboard_)capabilities.push_back(1);
                        control_.send(unified::record(Kind::Keyboard,0,capabilities));}video_cipher_->protection(protection);audio_cipher_->protection(protection);incoming_->protection(protection);media_ready_.store(true);
                    report_event("daemon_media_protection",static_cast<unsigned>(protection),EVENTLOG_INFORMATION_TYPE);
                }else if(record.kind==Kind::Begin){
                    require(record.generation>generation_.load()&&record.body.size()==1&&!desktop_caps_.empty()&&!vr_caps_.empty(),"daemon_mode_generation_invalid");
                    mode_.store(0);keyboard_.reset();generation_.store(record.generation);auto phase=Clock::now();if(tracker_){tracker_->stop();tracker_.reset();}report_duration("daemon_tracker_stop_ms",phase);
                    phase=Clock::now();auto old=take_backend();if(old){if(!old->is_desktop())worker_.destination(0);old->stop();}report_duration("daemon_backend_stop_ms",phase);reported=false;
                    const bool desktop=record.body[0]==0;require(record.body[0]<=1,"daemon_mode_invalid");
                    auto backend=std::make_shared<Backend>(desktop,record.generation,[this,generation=record.generation](const Bytes& bytes){
                        if(stopped()||generation!=generation_.load())return true;control_.send(unified::record(Kind::Control,generation,bytes));return true;
                    },[this,generation=record.generation](const std::uint8_t* bytes,std::size_t size){return generation!=generation_.load()||send(video_,*video_cipher_,video_mutex_,video_record_,generation,bytes,size);});
                    set_backend(backend);audio_->playback_allowed(!desktop||ticket_.audio);
                    report_event("daemon_mode_begin",record.generation,EVENTLOG_INFORMATION_TYPE,desktop?"desktop":"vr");
                    if(desktop&&desktop_keyboard_)keyboard_=std::make_unique<Keyboard>(
                        [this,generation=record.generation](const std::uint8_t* data,std::size_t size,bool enabled){
                            // One latest image; capture never waits on the network/control owner.
                            Bytes body{1};if(enabled&&size)body.insert(body.end(),data,data+size);
                            std::lock_guard<std::mutex> lock(candidate_mutex_);candidate_pending_=std::make_pair(generation,std::move(body));
                        },candidate_click_,true);
                    if(desktop){backend->desktop(runtime_.desktop,ticket_.fps,ticket_.bitrate,ticket_.input,[this](unsigned count){const auto updated=dashboard_monitors(ticket_,peer_,count);if(updated)monitor_count_.store(updated);return updated?updated:count?0:monitor_count_.load();},[this]{return worker_.idle();},[this]{return prefer_pc_mouse_.load();});}
                    else {if(keyboard_capable_)keyboard_=std::make_unique<Keyboard>(runtime_.candidate_update,candidate_click_);backend->vr(capabilities(vr_caps_),worker_);}
                    mode_.store(desktop?1:2);
                }else if(record.kind==Kind::End){if(record.generation==generation_.load()){
                    mode_.store(0);
                    keyboard_.reset();auto phase=Clock::now();if(tracker_){tracker_->stop();tracker_.reset();}report_duration("daemon_end_tracker_stop_ms",phase);
                    phase=Clock::now();auto backend=take_backend();if(backend){if(!backend->is_desktop())worker_.destination(0);backend->stop();}report_duration("daemon_end_backend_stop_ms",phase);
                }}
                else if(record.kind==Kind::TrackingBegin&&record.generation==generation_.load()){
                    auto backend=get_backend();require(backend&&backend->is_desktop(),"daemon_tracking_requires_desktop");if(tracker_)tracker_->stop();
                    try{
                        const auto caps=capabilities(record.body);
                        tracker_=std::make_shared<Backend>(false,record.generation,[this,generation=record.generation](const Bytes& bytes){if(generation==generation_.load()&&!stopped())control_.send(unified::record(Kind::Tracking,generation,bytes));return true;},[](const std::uint8_t*,std::size_t){return true;});tracker_->tracking(caps);
                    }catch(...){tracker_.reset();control_.send(unified::record(Kind::Tracking,record.generation,reserved(5,"frame_session_stopped")));}
                }else if(record.kind==Kind::Tracking&&record.generation==generation_.load()){if(tracker_)require(tracker_->push(record.body),"daemon_tracking_queue_full");}
                else if(record.kind==Kind::Keyboard&&record.generation==generation_.load()){
                    require(keyboard_capable_&&record.body.size()==2&&record.body[0]==0&&record.body[1]<=1,"daemon_keyboard_state");
                    if(keyboard_)keyboard_->visible(record.body[1]!=0);
                }
                else if(record.kind==Kind::Control){auto backend=get_backend();if(backend&&record.generation==backend->generation()){
                    if(!keyboard_||!keyboard_->input(record.body))require(backend->push(record.body),"daemon_control_backpressure_timeout");
                }}
                else require(record.kind==Kind::KeepAlive,"daemon_unexpected_control");
            }else control_.socket.ready(20);
            require(Clock::now()-last<unified::stall_timeout,"daemon_client_keepalive_expired");
            std::optional<std::pair<unsigned,Bytes>> candidate;
            {std::lock_guard<std::mutex> lock(candidate_mutex_);candidate.swap(candidate_pending_);}
            if(candidate&&candidate->first==generation_.load())control_.send(unified::record(Kind::Keyboard,candidate->first,candidate->second));
            auto backend=get_backend();if(backend&&backend->finished()&&!reported){mode_.store(0);keyboard_.reset();control_.send(unified::record(Kind::Ended,backend->generation(),Bytes{static_cast<std::uint8_t>(backend->reason())}));reported=true;}
        }
    }
private:
    std::unique_ptr<Keyboard> keyboard_;bool keyboard_capable_=false,candidate_click_=false,desktop_keyboard_=false;
    std::mutex candidate_mutex_;std::optional<std::pair<unsigned,Bytes>> candidate_pending_;
    Bytes capabilities(const Bytes& caps){
        auto config=dashboard_audio(ticket_,peer_);Bytes output(65536);
        require(!config.empty()&&config.back()=='}',"daemon_audio_config_invalid");config.pop_back();
        const auto field=std::string(",\"frame_daemon_media_overhead\":")+std::to_string(video_cipher_->overhead())+"}";config.insert(config.end(),field.begin(),field.end());
        const auto count=runtime_.capabilities(config.data(),config.size(),caps.data(),caps.size(),reinterpret_cast<const std::uint8_t*>(&peer_.sin_addr),output.data(),output.size());
        require(count>0&&static_cast<std::size_t>(count)<=output.size(),"daemon_capabilities_invalid");output.resize(static_cast<std::size_t>(count));return output;
    }
    bool stopped()const{return closing_.load()||external_stop_.load();}
    std::shared_ptr<Backend> get_backend(){std::lock_guard<std::mutex> lock(backend_mutex_);return backend_;}
    std::shared_ptr<Backend> take_backend(){std::lock_guard<std::mutex> lock(backend_mutex_);auto result=std::move(backend_);return result;}
    void set_backend(std::shared_ptr<Backend> backend){std::lock_guard<std::mutex> lock(backend_mutex_);backend_=std::move(backend);}
    bool send_datagram(SOCKET socket,WSABUF* parts,DWORD count,std::size_t size){
        DWORD written=0;
        if(WSASend(socket,parts,count,&written,0,nullptr,nullptr)==0)return written==size;
        if(WSAGetLastError()!=WSAEWOULDBLOCK)return false;
        // A blocking send's timeout can itself hang while cancelling a failed
        // Wi-Fi driver's I/O. Keep the original 100 ms backpressure allowance
        // in userspace so session/worker retirement can always cancel it.
        const auto deadline=Clock::now()+std::chrono::milliseconds(100);
        while(!stopped()&&Clock::now()<deadline){
            Sleep(1);
            if(WSASend(socket,parts,count,&written,0,nullptr,nullptr)==0)return written==size;
            if(WSAGetLastError()!=WSAEWOULDBLOCK)return false;
        }
        return false;
    }
    bool send(Socket& socket,unified::MediaCipher& cipher,std::mutex& mutex,Bytes& record,unsigned generation,const std::uint8_t* shard,std::size_t size){
        if(stopped())return false;std::lock_guard<std::mutex> lock(mutex);
        require(shard&&size>=14&&size<=2048,"surfxr_unified_datagram_bound");
        if(cipher.protection()==unified::Protection::Plain){
            std::array<std::uint8_t,20> prefix{};cipher.plain_prefix(generation,prefix);
            WSABUF parts[2]{{static_cast<ULONG>(prefix.size()),reinterpret_cast<char*>(prefix.data())},{static_cast<ULONG>(size),reinterpret_cast<char*>(const_cast<std::uint8_t*>(shard))}};
            return send_datagram(socket.value,parts,2,prefix.size()+size);
        }
        cipher.seal(generation,shard,size,record);WSABUF part{static_cast<ULONG>(record.size()),reinterpret_cast<char*>(record.data())};
        return send_datagram(socket.value,&part,1,record.size());
    }
    void receive_priority(){
        try{
            Reassembler microphone(packet_size,true,192000,1u<<2,8'000'000);
            while(!stopped()){
                if(!media_ready_.load()){Sleep(20);continue;}
                if(!priority_.ready(20))continue;std::array<std::uint8_t,4096> bytes{};sockaddr_in peer{};int size=sizeof(peer);
                const auto count=recvfrom(priority_.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<sockaddr*>(&peer),&size);
                if(count<=0||peer.sin_addr.s_addr!=peer_.sin_addr.s_addr||ntohs(peer.sin_port)!=options_.port+2)continue;
                unified::MediaView view;try{view=incoming_->open(bytes.data(),static_cast<std::size_t>(count),incoming_plain_);}catch(...){continue;}
                Reader r(view.data,view.size);const auto generation=r.integer(4);require(r.remaining()>=14&&r.remaining()<=2048,"daemon_priority_bound");Reader header(r.data(),r.remaining());const auto stream=header.integer(2);
                if(generation==0&&stream==2){if(auto packet=microphone.accept(r.data(),r.remaining(),static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count())))audio_->microphone(std::move(packet->bytes),packet->loss);}
                else if(generation==generation_.load()&&(stream==0||stream==4)){auto backend=get_backend();if(backend&&!backend->is_desktop())require(backend->uplink(r.data(),r.remaining()),"daemon_vr_uplink_failed");}
            }
        }catch(const std::exception& error){report_failure(error,"daemon_priority_failed");closing_.store(true);}
        catch(...){report_event("daemon_priority_failed");closing_.store(true);}
    }
    const Options& options_;Control control_;sockaddr_in peer_;Socket& video_;Socket& priority_;Runtime& runtime_;Worker& worker_;const std::atomic_bool& external_stop_;
    Ticket ticket_;Bytes desktop_caps_,vr_caps_,video_record_,audio_record_,incoming_plain_;std::unique_ptr<unified::MediaCipher> video_cipher_,audio_cipher_,incoming_;std::unique_ptr<AudioSession> audio_;
    std::atomic_bool closing_{false},media_ready_{false};std::atomic_uint generation_{0},monitor_count_{1},mode_{0};std::uint32_t audio_index_=0;
    void* video_qos_=nullptr;void* audio_qos_=nullptr;
    std::atomic_bool prefer_pc_mouse_{false};
    Bytes settings_request_;std::mutex settings_mutex_;
    std::mutex video_mutex_,audio_mutex_,backend_mutex_;std::shared_ptr<Backend> backend_,tracker_;std::thread policy_,receiver_;
};
}
int run(const Options& options,const std::atomic_bool& stop){
    require(options.dashboard_managed&&system_identity(),"daemon_requires_managed_local_system");
    WindowsTimerPeriod timer;WSADATA startup{};require(WSAStartup(MAKEWORD(2,2),&startup)==0,"daemon_winsock_start");struct Cleanup{~Cleanup(){WSACleanup();}}cleanup;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Runtime runtime;Worker worker(runtime.worker);
    auto listener=Socket::bind(options.bind,options.port,true),video=Socket::bind(options.bind,options.port+1,false),priority=Socket::bind(options.bind,options.port+2,false,128*1024);
    auto discovery=Socket::bind(options.bind,options.port,false);std::atomic_bool done{false};
    std::thread announcements([&]{try{while(!done.load()&&!stop.load())if(discovery.ready(20))answer_discovery(discovery,options);}catch(...){}});
    struct Join {std::atomic_bool& done;std::thread& thread;~Join(){done.store(true);thread.join();}} join{done,announcements};
    u_long nonblocking=1;for(auto socket:{video.value,priority.value})
        require(ioctlsocket(socket,FIONBIO,&nonblocking)==0,"daemon_media_nonblocking_failed");
    while(!stop.load()){
        dashboard_announce(true,options.port,true);
        if(!listener.ready(100))continue;sockaddr_in peer{};int length=sizeof(peer);Socket accepted(accept(listener.value,reinterpret_cast<sockaddr*>(&peer),&length));if(accepted.value==INVALID_SOCKET)continue;
        try{Session(options,std::move(accepted),peer,video,priority,runtime,worker,stop).run();}catch(const std::exception& error){report_failure(error,"daemon_session_failed");}
    }
    return 0;
}
}
