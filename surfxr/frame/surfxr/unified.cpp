#include "unified.hpp"
#include "microphone.hpp"
#include "surfxr/surfxr_modes.hpp"
#include "surfxr/surfxr_settings.hpp"
#include "surfxr/surfxr_unified.hpp"
#include "surfxr/surfxr_unified_media.hpp"
#include "unified_queue.hpp"
#include <deque>
#include <poll.h>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace surfxr::transport {
using unified::Kind;
namespace {
void little(Bytes& bytes,std::uint64_t value,unsigned size){Writer w;w.integer(value,size);bytes.insert(bytes.end(),w.bytes.begin(),w.bytes.end());}
void wake(int fd){const std::uint64_t value=1;while(::write(fd,&value,sizeof(value))<0&&errno==EINTR){}}
}
struct UnifiedTransport::State:std::enable_shared_from_this<State> {
    struct Adapter:SocketAdapter {
        std::weak_ptr<State> reference;unsigned lane=0,generation=0;
        Adapter(std::shared_ptr<State> s,unsigned l,unsigned g):reference(s),lane(l),generation(g){}
        std::shared_ptr<State> owner()const{auto state=reference.lock();require(bool(state),"surfxr_unified_transport_closed");return state;}
        int fd()const override{auto state=owner();return lane==1?state->video.fd():state->queues[lane].fd();}
        bool readable()const override{auto state=owner();state->check();if(lane==0)state->check_source(generation);return lane==1?
            generation==state->generation.load()&&state->video.readable():state->queues[lane].readable(generation);}
        std::size_t read(std::uint8_t* data,std::size_t size)override{
            auto state=owner();state->check();if(lane==0)state->check_source(generation);if(lane!=1)return state->queues[lane].read(data,size,lane==0||lane==3,generation);
            std::size_t copied=0;state->read_video(size,1,generation,[&](const auto* bytes,std::size_t count){std::memcpy(data,bytes,count);copied=count;});return copied;
        }
        unsigned read_datagrams(std::uint8_t*,std::size_t stride,unsigned count,
                                const std::function<void(const std::uint8_t*,std::size_t)>& sink)override{
            auto state=owner();state->check();require(lane==1||lane==2,"surfxr_unified_datagram_lane");
            return lane==1?state->read_video(stride,count,generation,sink):state->queues[lane].read_datagrams(stride,count,generation,sink);
        }
        void send(const Bytes& bytes)override{
            auto state=owner();state->check();
            if(lane==0||lane==3){Reader r(bytes);const auto size=r.integer(4);require(size==r.remaining(),"surfxr_unified_control_frame");state->send(lane==0?Kind::Control:Kind::Tracking,generation,Bytes(r.data(),r.data()+r.remaining()));}
            else {std::lock_guard<std::mutex> lock(state->priority_mutex);Reader header(bytes);const auto stream=header.integer(2);
                require(bytes.size()>=14&&bytes.size()<=2048,"surfxr_unified_datagram_bound");
                const auto gen=stream==2?0:generation;
                if(state->out_priority->protection()==unified::Protection::Plain){
                    std::array<std::uint8_t,20> prefix{};state->out_priority->plain_prefix(gen,prefix);
                    state->priority.send_parts(prefix.data(),prefix.size(),bytes.data(),bytes.size(),[&]{return state->cancelled();});
                }else{state->out_priority->seal(gen,bytes.data(),bytes.size(),state->priority_record);
                    state->priority.send(state->priority_record,false,[&]{return state->cancelled();});}}
        }
    };
    std::shared_ptr<SettingsSync> settings;
    std::shared_ptr<KeyboardOverlay> keyboard_overlay;std::atomic_bool keyboard_ready{false},desktop_keyboard_ready{false};
    Socket control,video,priority;SessionCallbacks callbacks;std::shared_ptr<Modes> modes;const std::atomic_bool* external_stop=nullptr;
    std::unique_ptr<desktop::Cipher> out_control,in_control;
    std::unique_ptr<unified::MediaCipher> in_video,in_audio,out_priority;
    std::mutex control_mutex,priority_mutex,video_mutex;std::atomic_bool stopping{false},failed{false},active_desktop{true};std::atomic_uint generation{0};std::atomic<std::uint64_t> audio_bytes{0};unsigned flags=0;
    std::atomic_uint source_failed_generation{0},source_stopped_generation{0};
    Bytes video_batch=Bytes(32*4096),video_plain,priority_record;
    const int media_wake=::eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    std::mutex failure_mutex;std::string failure="surfxr_unified_transport_failed";
    UnifiedQueue queues[4];Framer control_framer{2*1024*1024,true};std::deque<Bytes> pending_control;
    std::thread control_reader,media_reader;std::unique_ptr<MicrophoneUpload> microphone_upload;Connection microphone_connection;MicrophoneUploadStats microphone_stats;
    bool cancelled()const{return stopping.load()||(external_stop&&external_stop->load());}
    void check(){if(failed.load()){std::lock_guard<std::mutex> lock(failure_mutex);fail(failure.c_str());}if(cancelled())throw Cancelled{};}
    void check_source(unsigned gen){require(gen!=generation.load()||source_failed_generation.load()!=gen,"surfxr_unified_backend_failed");}
    unsigned read_video(std::size_t capacity,unsigned count,unsigned gen,
            const std::function<void(const std::uint8_t*,std::size_t)>& sink){
        // Only the active source reads this socket. Its reassembler borrows
        // the received shard directly; AES reuses one plaintext scratch owner.
        // Begin serializes with this reader after old source owners join, so
        // stale adapters can neither drain nor publish the new generation.
        std::lock_guard<std::mutex> lock(video_mutex);check();if(gen!=generation.load())return 0;
        return video.read_datagrams(video_batch.data(),4096,count,[&](const auto* bytes,std::size_t size){
            unified::MediaView view;try{view=in_video->open(bytes,size,video_plain);}catch(const std::runtime_error&){return;}
            Reader r(view.data,view.size);if(r.integer(4)!=gen)return;
            require(r.remaining()<=capacity,"surfxr_unified_datagram_bound");sink(r.data(),r.remaining());
        });
    }
    void stop(){stopping.store(true);wake(media_wake);for(auto& queue:queues)queue.wake();}
    void record_failure(const std::exception* error){
        std::lock_guard<std::mutex> lock(failure_mutex);if(failed.load())return;
        // Keep only exact local check codes, never arbitrary callback text.
        if(error)for(const auto* code:{"surfxr_unified_control_queue_full","surfxr_unified_tracking_queue_full",
            "surfxr_unified_video_queue_full","surfxr_unified_priority_queue_full","surfxr_unified_control_timeout",
            "surfxr_unified_keepalive_timeout","surfxr_unified_unexpected_control","surfxr_unified_backend_failed",
            "surfxr_peer_closed","surfxr_receive_failed","surfxr_send_failed","surfxr_send_timeout","surfxr_invalid_audio_packet"})
            if(std::strcmp(error->what(),code)==0){failure=code;break;}
        failed.store(true);wake(media_wake);for(auto& queue:queues)queue.wake();
    }
    void send_plain(const Bytes& bytes){Bytes framed;little(framed,bytes.size(),4);framed.insert(framed.end(),bytes.begin(),bytes.end());control.send(framed,true,[this]{return cancelled();});}
    void send(Kind kind,unsigned gen,const Bytes& body={}){std::lock_guard<std::mutex> lock(control_mutex);send_plain(out_control->seal(unified::record(kind,gen,body)));}
    Bytes receive_plain(Framer& framer,std::deque<Bytes>& pending,Clock::time_point deadline){
        while(pending.empty()){check();require(Clock::now()<deadline,"surfxr_unified_control_timeout");std::array<std::uint8_t,8192> bytes{};const auto count=control.read(bytes.data(),bytes.size(),true);
            if(count)framer.feed(bytes.data(),count,[&](Bytes body){require(pending.size()<64,"surfxr_unified_control_queue_full");pending.push_back(std::move(body));});
            else{pollfd fd{control.fd(),POLLIN,0};::poll(&fd,1,2);}}
        auto body=std::move(pending.front());pending.pop_front();return body;
    }
    void start(){
        control_reader=std::thread([this]{
            try{auto& framer=control_framer;auto& pending=pending_control;auto last=Clock::now(),keepalive=last;
                while(!cancelled()){
                    if(Clock::now()>=keepalive){send(Kind::KeepAlive,0);if(settings)send(Kind::Settings,0,settings->poll());keepalive=Clock::now()+std::chrono::milliseconds(500);}
                    pollfd fd{control.fd(),POLLIN,0};if(pending.empty())::poll(&fd,1,20);
                    if(!pending.empty()||fd.revents){const auto record=unified::parse(in_control->open(receive_plain(framer,pending,last+unified::stall_timeout)));last=Clock::now();
                        if(record.kind==Kind::Keyboard&&record.generation==0){
                            require(record.body==Bytes{1}||record.body==Bytes{2}||record.body==Bytes({1,1})||record.body==Bytes({2,1}),"surfxr_keyboard_negotiation");
                            desktop_keyboard_ready.store(record.body.size()==2);keyboard_ready.store(true);if(keyboard_overlay){keyboard_overlay->supported.store(true);keyboard_overlay->clickable.store(record.body[0]==2);}}
                        else if(record.kind==Kind::Keyboard&&record.generation!=0){
                            std::lock_guard<std::mutex> lock(video_mutex);
                            if(record.generation!=generation.load())continue;
                            require(desktop_keyboard_ready.load()&&active_desktop.load()&&!record.body.empty()&&record.body[0]==1,"surfxr_desktop_keyboard_image");
                            auto image=keyboard_image::decode(record.body.data()+1,record.body.size()-1);
                            if(keyboard_overlay)keyboard_overlay->publish(std::move(image));
                        }
                        else if(record.kind==Kind::Settings&&record.generation==0){if(settings)settings->receive(record.body);}
                        else if(record.kind==Kind::Control&&record.generation==generation.load()){
                            try{const auto packet=server_control(record.body,true);if(packet.type==5&&packet.reserved=="frame_session_stopped"){
                                source_stopped_generation.store(record.generation);if(!active_desktop.load()&&modes)modes->request(true);
                            }}catch(const std::runtime_error&){}
                            Bytes framed;little(framed,record.body.size(),4);framed.insert(framed.end(),record.body.begin(),record.body.end());queues[0].push(std::move(framed),4*1024*1024,record.generation,"surfxr_unified_control_queue_full");
                        }else if(record.kind==Kind::Tracking&&record.generation==generation.load()){
                            Bytes framed;little(framed,record.body.size(),4);framed.insert(framed.end(),record.body.begin(),record.body.end());queues[3].push(std::move(framed),4096,record.generation,"surfxr_unified_tracking_queue_full");
                        }else if(record.kind==Kind::Ended&&record.generation==generation.load()){
                            if(record.body==Bytes{2})continue;
                            if(source_stopped_generation.load()==record.generation)continue;
                            if(record.body==Bytes{0}){
                                if(modes&&modes->pending())continue;
                                source_failed_generation.store(record.generation);queues[0].wake(record.generation);continue;
                            }
                            if(modes&&!modes->desktop())modes->request(true);else fail("surfxr_unified_backend_failed");
                        }else require(record.kind==Kind::KeepAlive||record.generation!=generation.load(),"surfxr_unified_unexpected_control");
                    }
                    require(Clock::now()-last<unified::stall_timeout,"surfxr_unified_keepalive_timeout");
                }
            }catch(const Cancelled&){}catch(const std::exception& error){record_failure(&error);}catch(...){record_failure(nullptr);}
        });
        media_reader=std::thread([this]{
            try{
                Reassembler audio(1200,true,192000,1u<<2,8'000'000);
                Bytes batch(32*4096),plain;
                while(!cancelled()){
                    const auto count=priority.read_datagrams(batch.data(),4096,32,[&](const auto* bytes,std::size_t size){
                        unified::MediaView view;try{view=in_audio->open(bytes,size,plain);}catch(const std::runtime_error&){return;}
                        Reader r(view.data,view.size);const auto gen=r.integer(4);
                        if(gen==0){if(auto packet=audio.accept(r.data(),r.remaining(),now_ns())){require(packet->bytes.size()%4==0,"surfxr_invalid_audio_packet");audio_bytes+=packet->bytes.size();if(callbacks.audio_pcm)callbacks.audio_pcm(packet->bytes.data(),packet->bytes.size());}}
                        else if(gen==generation.load())queues[2].push(Bytes(r.data(),r.data()+r.remaining()),192000,static_cast<unsigned>(gen),"surfxr_unified_priority_queue_full");
                    });
                    while(auto packet=audio.pop_ready(now_ns())){audio_bytes+=packet->bytes.size();if(callbacks.audio_pcm)callbacks.audio_pcm(packet->bytes.data(),packet->bytes.size());}
                    if(count==32)continue;
                    const auto now=now_ns();const auto deadline=audio.next_ready_ns(now);
                    const int timeout=deadline?static_cast<int>(std::min<std::uint64_t>(20,*deadline>now?(*deadline-now+999999)/1000000:0)):20;
                    pollfd fds[2]{{priority.fd(),POLLIN,0},{media_wake,POLLIN,0}};::poll(fds,2,timeout);
                    if(fds[1].revents)break;
                }
            }catch(const Cancelled&){}catch(const std::exception& error){record_failure(&error);}catch(...){record_failure(nullptr);}
        });
    }
    ~State(){stop();microphone_upload.reset();if(control_reader.joinable())control_reader.join();if(media_reader.joinable())media_reader.join();::close(media_wake);}
};
bool UnifiedTransport::available(const SessionOptions& o,const std::function<bool()>& cancelled){
    try{
        auto socket=Socket::connect_tcp(o.host,o.surfxr_dashboard_port,o.local_address,Clock::now()+std::chrono::milliseconds(500),cancelled);
        Bytes query(desktop::dashboard_magic.begin(),desktop::dashboard_magic.end());query.push_back(8);socket.send(query,true,cancelled);
        std::uint8_t value=0;const auto end=Clock::now()+std::chrono::milliseconds(500);
        while(Clock::now()<end){if(socket.read(&value,1,true))return value==5;if(cancelled())throw Cancelled{};pollfd fd{socket.fd(),POLLIN,0};::poll(&fd,1,2);}
    }catch(const Cancelled&){throw;}catch(...){}return false;
}
UnifiedTransport::UnifiedTransport(const SessionOptions& o,const SessionCallbacks& callbacks,const desktop::Ticket& ticket,const std::atomic_bool* stop):state_(std::make_shared<State>()){
    auto& s=*state_;s.callbacks=callbacks;s.modes=o.surfxr_modes;s.keyboard_overlay=o.keyboard_overlay;s.settings=o.surfxr_settings;s.external_stop=stop;
    require(s.media_wake>=0,"surfxr_unified_queue_event_failed");
    s.control=Socket::connect_tcp(o.host,ticket.port,o.local_address,Clock::now()+std::chrono::seconds(5),[&]{return s.cancelled();});
    Bytes claim(desktop::ticket_magic.begin(),desktop::ticket_magic.end());claim.insert(claim.end(),ticket.id.begin(),ticket.id.end());if(ticket.media_modes)claim.push_back(1);s.send_plain(claim);
    auto& framer=s.control_framer;auto& pending=s.pending_control;auto challenge=s.receive_plain(framer,pending,Clock::now()+std::chrono::seconds(5));
    require(challenge.size()==40&&std::equal(desktop::magic.begin(),desktop::magic.end(),challenge.begin()),"surfxr_unified_auth_protocol");desktop::Key server{},client=desktop::random();std::copy_n(challenge.data()+8,32,server.begin());
    auto mac=desktop::proof(ticket.key,server,client,"ALVR-DESKTOP-client-proof-v1");Bytes response(client.begin(),client.end());response.insert(response.end(),mac.begin(),mac.end());s.send_plain(response);
    auto ack=s.receive_plain(framer,pending,Clock::now()+std::chrono::seconds(5));desktop::Key received{};require(ack.size()==32,"surfxr_unified_auth_failed");std::copy_n(ack.data(),32,received.begin());require(desktop::equal(received,desktop::proof(ticket.key,server,client,"ALVR-DESKTOP-server-proof-v1")),"surfxr_unified_auth_failed");
    s.out_control=std::make_unique<desktop::Cipher>(desktop::proof(ticket.key,server,client,"ALVR-DESKTOP-control-c2s-v1"));s.in_control=std::make_unique<desktop::Cipher>(desktop::proof(ticket.key,server,client,"ALVR-DESKTOP-control-s2c-v1"));
    s.in_video=std::make_unique<unified::MediaCipher>(desktop::proof(ticket.key,server,client,"ALVR-UNIFIED-video-s2c-v1"));s.in_audio=std::make_unique<unified::MediaCipher>(desktop::proof(ticket.key,server,client,"ALVR-UNIFIED-audio-s2c-v1"));s.out_priority=std::make_unique<unified::MediaCipher>(desktop::proof(ticket.key,server,client,"ALVR-UNIFIED-priority-c2s-v1"));
    auto hello=unified::parse(s.in_control->open(s.receive_plain(framer,pending,Clock::now()+std::chrono::seconds(5))));require(hello.kind==Kind::Hello&&hello.generation==0,"surfxr_unified_hello_required");Reader r(hello.body);const auto video_port=r.integer(2),priority_port=r.integer(2);s.flags=static_cast<unsigned>(r.integer(1));r.end();
    require(video_port==ticket.port+1&&priority_port==ticket.port+2&&(s.flags&~15)==0&&(!(s.flags&8)||(s.flags&4))&&(!(s.flags&4)||ticket.media_modes),"surfxr_unified_ports_invalid");
    s.video=Socket::bind(o.local_address,static_cast<unsigned>(video_port),false);s.video.connect_udp(o.host,static_cast<unsigned>(video_port));s.priority=Socket::bind(o.local_address,static_cast<unsigned>(priority_port),false,128*1024);s.priority.connect_udp(o.host,static_cast<unsigned>(priority_port));
    std::array<char,64> hostname{};require(::gethostname(hostname.data(),hostname.size()-1)==0,"surfxr_client_hostname_failed");for(auto& c:hostname)if(c&&!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))c='-';
    auto rates=o.surfxr_refresh_rates;if(rates.empty())rates.push_back(float(o.initial_vr_framerate));
    Writer negotiated;negotiated.blob(capabilities(o.host,1728,1728,{60},o.surfxr_version,false,true,false,false,"",true));
    negotiated.blob(capabilities(o.host,o.surfxr_view_width,o.surfxr_view_height,rates,o.surfxr_version,true,true,o.eye_gaze_available&&o.initial_foveated_streaming,true,hostname.data(),false,false,o.pose_time_available));
    if(s.flags&4){
        const auto protection=(s.flags&8)||o.surfxr_media_aes?unified::Protection::AesGcm:unified::Protection::Plain;
        s.in_video->protection(protection);s.in_audio->protection(protection);s.out_priority->protection(protection);
        negotiated.integer(1,1);negotiated.integer(static_cast<unsigned>(protection),1);
    }
    s.send(Kind::Hello,0,negotiated.bytes);
    s.start();
    if((s.flags&2)&&o.microphone_enabled&&callbacks.microphone_start&&callbacks.microphone_pcm&&callbacks.microphone_stop){
        s.microphone_connection.modern=true;s.microphone_connection.packet_size=1200;s.microphone_connection.cancelled=[&s]{return s.cancelled();};
        s.microphone_connection.priority_socket=Socket(std::make_shared<State::Adapter>(state_,2,0));
        s.microphone_upload=std::make_unique<MicrophoneUpload>(s.microphone_connection,o,callbacks,s.microphone_stats);
    }
}
UnifiedTransport::~UnifiedTransport(){close();}
void UnifiedTransport::close(){state_->stop();state_->microphone_upload.reset();if(state_->control_reader.joinable())state_->control_reader.join();if(state_->media_reader.joinable())state_->media_reader.join();if(state_->settings)state_->settings->disconnected();state_->keyboard_ready.store(false);if(state_->keyboard_overlay){state_->keyboard_overlay->supported.store(false);state_->keyboard_overlay->clickable.store(false);state_->keyboard_overlay->publish({});}}
std::string UnifiedTransport::report()const {
    require(state_->stopping.load(),"surfxr_unified_report_requires_join");return "{\"authenticated_connections\":1,\"mode_generations\":"+std::to_string(state_->generation.load())+",\"audio_bytes\":"+std::to_string(state_->audio_bytes.load())+
        ",\"media_aes\":"+(state_->in_video->protection()==unified::Protection::AesGcm?"true":"false")+
        ",\"microphone_upload\":{\"start_attempts\":"+std::to_string(state_->microphone_stats.start_attempts)+",\"capture_failures\":"+std::to_string(state_->microphone_stats.capture_failures)+",\"packets\":"+std::to_string(state_->microphone_stats.packets)+"}}";
}
Socket UnifiedTransport::begin(bool desktop,const Bytes&){
    auto& s=*state_;std::lock_guard<std::mutex> reader(s.video_mutex);s.check();const auto gen=s.generation.fetch_add(1)+1;require(gen,"surfxr_unified_generation_exhausted");s.active_desktop.store(desktop);if(s.keyboard_overlay)s.keyboard_overlay->publish({});s.source_failed_generation.store(0);s.source_stopped_generation.store(0);for(auto& queue:s.queues)queue.clear(gen);Bytes begin{static_cast<std::uint8_t>(desktop?0:1)};s.send(Kind::Begin,gen,begin);return Socket(std::make_shared<State::Adapter>(state_,0,gen));
}
Socket UnifiedTransport::video(){return Socket(std::make_shared<State::Adapter>(state_,1,state_->generation.load()));}
Socket UnifiedTransport::priority(){return Socket(std::make_shared<State::Adapter>(state_,2,state_->generation.load()));}
Socket UnifiedTransport::tracking(const Bytes& caps){state_->check();const auto gen=state_->generation.load();state_->queues[3].clear(gen);state_->send(Kind::TrackingBegin,gen,caps);return Socket(std::make_shared<State::Adapter>(state_,3,gen));}
void UnifiedTransport::end(){try{if(!state_->cancelled()&&!state_->failed.load())state_->send(Kind::End,state_->generation.load());}catch(const Cancelled&){}catch(...){state_->failed.store(true);}}
void UnifiedTransport::check(){state_->check();if(state_->microphone_upload)state_->microphone_upload->check();}
void UnifiedTransport::keyboard_visibility(bool visible){
    if(state_->keyboard_ready.load()&&(!state_->active_desktop.load()||state_->desktop_keyboard_ready.load()))
        state_->send(Kind::Keyboard,state_->generation.load(),Bytes{0,static_cast<std::uint8_t>(visible)});
}
bool UnifiedTransport::desktop_keyboard()const{return state_->desktop_keyboard_ready.load();}
bool UnifiedTransport::keyboard()const{return state_->keyboard_ready.load();}
bool UnifiedTransport::audio()const{return state_->flags&1;}
bool UnifiedTransport::microphone()const{return state_->flags&2;}
}
