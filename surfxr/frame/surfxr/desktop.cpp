#include "surfxr/surfxr.hpp"
#include "surfxr/surfxr_desktop.hpp"
#include "surfxr/surfxr_desktop_crypto.hpp"
#include "surfxr/surfxr_unified.hpp"
#include "surfxr/vr_wire.hpp"
#include "socket.hpp"
#include "desktop_audio.hpp"
#include "desktop_streams.hpp"
#include "surfxr/surfxr_desktop_capture.hpp"
#include "dashboard.hpp"
#include "desktop_tracking.hpp"
#include "unified.hpp"
#include <deque>
#include <poll.h>
#include <unistd.h>

namespace surfxr::transport {
namespace {
class DesktopControl {
public:
    Socket socket;
    std::unique_ptr<desktop::Cipher> outgoing,incoming;
    std::function<bool()> cancelled;
    void send(const Bytes& body){auto b=outgoing?outgoing->seal(body):body;Bytes framed;for(unsigned i=0;i<4;++i)framed.push_back(static_cast<std::uint8_t>(b.size()>>(8*i)));framed.insert(framed.end(),b.begin(),b.end());socket.send(framed,true,cancelled);}
    Bytes receive(Clock::time_point deadline){
        while(pending.empty()){
            if(cancelled())throw Cancelled{};
            require(Clock::now()<deadline,"surfxr_desktop_control_timeout");
            std::array<std::uint8_t,8192> b{};const auto n=socket.read(b.data(),b.size(),true);
            if(n)framer.feed(b.data(),n,[&](Bytes body){pending.push_back(std::move(body));});
            else{pollfd p{socket.fd(),POLLIN,0};::poll(&p,1,2);}
        }
        auto b=std::move(pending.front());pending.pop_front();return incoming?incoming->open(b):b;
    }
    bool readable(){return !pending.empty()||socket.readable();}
private:
    Framer framer{2*1024*1024,true};std::deque<Bytes> pending;
};
}
SessionResult run_desktop(const SessionOptions& o,const SessionCallbacks& callbacks,const std::atomic_bool* stop){
    SessionResult result;DesktopControl control;std::string error;const auto end=o.seconds?Clock::now()+std::chrono::seconds(o.seconds):Clock::time_point::max();
    control.cancelled=[&]{return (stop&&stop->load())||Clock::now()>=end;};
    bool configured=false,corrupted=true;
    try{
        require(o.surfxr_desktop&&o.surfxr_version=="21.0.0-dev14"&&o.surfxr_desktop_port>=1024&&o.surfxr_desktop_port<65535,"surfxr_desktop_options");
        auto root=desktop::Key{};desktop::Key server{},client{};auto port=o.surfxr_desktop_port;
        std::optional<desktop::Ticket> ticket;
        const auto caps=capabilities(o.host,1728,1728,{60},o.surfxr_version,false,true,false,false,"",true);
        if(o.surfxr_unified){control.socket=o.surfxr_unified->begin(true,caps);}
        else {
        if(o.surfxr_dashboard_desktop){ticket=request_desktop(o,control.cancelled);root=ticket->key;port=ticket->port;}
        else root=desktop::read_key(o.surfxr_desktop_key_file);
        control.socket=Socket::connect_tcp(o.host,port,o.local_address,Clock::now()+std::chrono::seconds(10),control.cancelled);
        if(ticket){Bytes claim(desktop::ticket_magic.begin(),desktop::ticket_magic.end());claim.insert(claim.end(),ticket->id.begin(),ticket->id.end());control.send(claim);}
        const auto challenge=control.receive(Clock::now()+std::chrono::seconds(5));
        require(challenge.size()==40&&std::equal(desktop::magic.begin(),desktop::magic.end(),challenge.begin()),"surfxr_desktop_auth_protocol");
        client=desktop::random();std::copy_n(challenge.data()+8,32,server.begin());
        const auto response=desktop::proof(root,server,client,"ALVR-DESKTOP-client-proof-v1");Bytes auth(client.begin(),client.end());auth.insert(auth.end(),response.begin(),response.end());control.send(auth);
        const auto ack=control.receive(Clock::now()+std::chrono::seconds(5));desktop::Key received{};
        require(ack.size()==32,"surfxr_desktop_auth_failed");std::copy_n(ack.data(),32,received.begin());
        require(desktop::equal(received,desktop::proof(root,server,client,"ALVR-DESKTOP-server-proof-v1")),"surfxr_desktop_auth_failed");
        control.outgoing=std::make_unique<desktop::Cipher>(desktop::proof(root,server,client,"ALVR-DESKTOP-control-c2s-v1"));
        control.incoming=std::make_unique<desktop::Cipher>(desktop::proof(root,server,client,"ALVR-DESKTOP-control-s2c-v1"));
        }
        std::unique_ptr<desktop::Cipher> media_cipher;
        if(!o.surfxr_unified){
            media_cipher=std::make_unique<desktop::Cipher>(desktop::proof(root,server,client,"ALVR-DESKTOP-media-s2c-v1"));
            control.send(caps);
        }
        const auto config=stream_config(control.receive(Clock::now()+std::chrono::seconds(15)),o.surfxr_version,false);
        require(config.desktop&&!config.gaze_atlas&&!config.microphone&&config.port!=port&&config.priority_port!=port&&(!config.game_audio||config.priority_port),"surfxr_desktop_negotiation_failed");
        const auto start=server_control(control.receive(Clock::now()+std::chrono::seconds(5)),true);require(start.type==0,"surfxr_start_stream_expected");
        auto media=o.surfxr_unified?o.surfxr_unified->video():Socket::bind(o.local_address,config.port,false);media.connect_udp(o.host,config.port);
        std::unique_ptr<DesktopAudio> audio;
        if(config.game_audio)audio=std::make_unique<DesktopAudio>(o,config,desktop::proof(root,server,client,"ALVR-DESKTOP-audio-s2c-v1"),callbacks);
        control.send(desktop::control(3));result.connection_started=true;if(callbacks.connection_started)callbacks.connection_started();
        std::unique_ptr<DesktopTracking> tracking;
        if(o.surfxr_dashboard_desktop&&is_v21(o.surfxr_version))tracking=std::make_unique<DesktopTracking>(o,callbacks,stop);
        if(callbacks.desktop_input_release)callbacks.desktop_input_release();
        Reassembler reassembler(config.packet_size,true,8*1024*1024,config.desktop_multimonitor?0x1b:1u<<3,8'000'000);Bytes decoder_config;unsigned codec=0;DesktopStreams streams(callbacks);
        desktop::CaptureSync capture;
        if(callbacks.desktop_capture_ack)callbacks.desktop_capture_ack(config.desktop_capture_control,15);
        auto keepalive=Clock::now(),last_received=Clock::now(),next_keyboard=Clock::now();bool keyboard_visible=false;
        auto last_idr=Clock::time_point::min();
        const auto idr=[&]{const auto now=Clock::now();if(last_idr!=Clock::time_point::min()&&now-last_idr<std::chrono::milliseconds(100))return;control.send(desktop::control(1));last_idr=now;};idr();
        Bytes datagrams(32*2048);
        const auto on_packet=[&](Packet packet){
            last_received=Clock::now();
            if(config.desktop_multimonitor){if(streams.receive(packet,idr))++result.received_frames;return;}
            Reader r(packet.bytes,true);const auto header=video_header(r,true);
            if(packet.loss){corrupted=true;idr();}if(!configured)return;if(header.idr)corrupted=false;if(corrupted){idr();return;}
            require(r.remaining()>0,"surfxr_empty_video_packet");Bytes access;
            if(header.idr){access=decoder_config;access.insert(access.end(),r.data(),r.data()+r.remaining());}
            if(callbacks.video_packet)callbacks.video_packet(0,header.idr?access.data():r.data(),header.idr?access.size():r.remaining(),header.timestamp);
            ++result.received_frames;
        };
        while(!control.cancelled()){
            if(o.surfxr_unified)o.surfxr_unified->check();
            if(audio)audio->check();
            if(Clock::now()-last_received>(o.surfxr_unified?unified::stall_timeout:std::chrono::seconds(5)))fail("surfxr_desktop_keepalive_timeout");
            if(Clock::now()>=keepalive){control.send(desktop::control(2));keepalive=Clock::now()+std::chrono::seconds(1);}
            if(config.desktop_capture_control&&callbacks.desktop_capture_mask)
                if(const auto request=capture.request(streams.generation(),callbacks.desktop_capture_mask()))control.send(desktop::capture_record(*request));
            const bool keyboard=o.surfxr_unified&&o.surfxr_unified->desktop_keyboard();
            if(keyboard){const bool visible=o.keyboard_overlay&&o.keyboard_overlay->active.load();
                if(visible!=keyboard_visible||Clock::now()>=next_keyboard){
                    o.surfxr_unified->keyboard_visibility(visible);keyboard_visible=visible;next_keyboard=Clock::now()+std::chrono::milliseconds(200);
                }
            }
            if(callbacks.desktop_input)for(unsigned i=0;i<64;++i){desktop::Event event;if(!callbacks.desktop_input(event))break;
                if(event.candidate){if(keyboard&&o.keyboard_overlay&&o.keyboard_overlay->clickable.load())control.send(desktop::input(event));continue;}
                control.send(config.desktop_multimonitor?desktop::input(event,streams.generation()):desktop::input(event));
            }
            if(o.control_queue){SessionControlCommand command;for(unsigned i=0;i<16&&o.control_queue->pop(command);++i){
                if(command.type==SessionCommand::Keyframe||command.type==SessionCommand::Restart)idr();
                if(config.desktop_multimonitor&&(command.type==SessionCommand::AddMonitor||command.type==SessionCommand::RemoveMonitor))control.send(desktop::monitor_request(command.type==SessionCommand::AddMonitor));
                if(callbacks.control_command_sent)callbacks.control_command_sent(command);
            }}
            if(control.readable()){
                const auto bytes=control.receive(Clock::now()+std::chrono::seconds(2));Reader type(bytes,true);last_received=Clock::now();
                const auto control_type=type.integer(4);
                if(config.desktop_capture_control&&control_type==desktop::capture_ack_type){
                    const auto ack=desktop::capture_record(bytes,true);
                    if(capture.acknowledge(ack)&&callbacks.desktop_capture_ack)callbacks.desktop_capture_ack(true,ack.mask);
                    continue;
                }
                if(config.desktop_multimonitor&&control_type==desktop::layout_type){streams.configure(bytes);idr();continue;}
                const auto message=server_control(bytes,true);
                if(message.type==1){codec=message.codec==0?1u:2u;decoder_config=message.config;configured=true;corrupted=true;
                    DesktopConfiguration layout{static_cast<unsigned>(config.refresh_rate),{{0,codec,config.width,config.height,config.desktop_x,config.desktop_y,config.desktop_width,config.desktop_height}}};
                    if(callbacks.desktop_config)callbacks.desktop_config(layout);
                    if(callbacks.source_changed)callbacks.source_changed(0);
                    idr();
                }else if(message.type==5&&message.reserved=="frame_session_stopped"){result.server_stopped=true;break;}
                else require(message.type==3,"surfxr_desktop_unexpected_control");
            }
            const auto count=media.read_datagrams(datagrams.data(),2048,32,[&](const auto* bytes,std::size_t size){
                Bytes plain;
                // Forged, replayed and too-old UDP records are ignored without
                // reviving keepalive or touching decoder/reference state.
                if(media_cipher){try{plain=media_cipher->open(bytes,size,false);}catch(const std::runtime_error&){return;}
                    bytes=plain.data();size=plain.size();}
                if(auto packet=reassembler.accept(bytes,size,now_ns()))on_packet(std::move(*packet));
            });
            while(auto packet=reassembler.pop_ready(now_ns()))on_packet(std::move(*packet));
            if(callbacks.video_tick)callbacks.video_tick();
            if(!count){pollfd p[2]{{control.socket.fd(),POLLIN,0},{media.fd(),POLLIN,0}};::poll(p,2,1);}
        }
        // The host independently releases on EOF/timeout even if this final
        // reliable message cannot complete after cancellation.
        if(callbacks.desktop_input_release)callbacks.desktop_input_release();
        control.cancelled=[] {return false;};
        try{control.send(config.desktop_multimonitor?desktop::input({},streams.generation()):desktop::input({}));}catch(const std::exception&){}
        result.exit_code=0;
    }catch(const Cancelled&){result.exit_code=0;}
    catch(const std::exception& e){error=e.what();result.exit_code=1;}
    if(callbacks.desktop_input_release)callbacks.desktop_input_release();
    if(callbacks.desktop_capture_ack)callbacks.desktop_capture_ack(false,15);
    if(callbacks.connection_ended)callbacks.connection_ended();
    result.sanitized_json="{\"backend\":\"surfxr\",\"status\":\""+std::string(result.exit_code?"session_error":"ok")+"\",\"stage\":\"surfxr_desktop\",\"error\":\""+error+"\",\"connection_started\":"+(result.connection_started?"true":"false")+",\"server_stopped\":"+(result.server_stopped?"true":"false")+",\"received_frames\":"+std::to_string(result.received_frames)+"}";
    return result;
}
}
