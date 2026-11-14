#include "surfxr/surfxr.hpp"
#include "surfxr/vr_wire.hpp"
#include "socket.hpp"
#include "surfxr/haptics.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>

using namespace surfxr::transport;
namespace {
unsigned port(const Socket& socket){sockaddr_in a{};socklen_t size=sizeof(a);require(getsockname(socket.fd(),reinterpret_cast<sockaddr*>(&a),&size)==0,"test_port_reservation");return ntohs(a.sin_port);}
Socket connect(unsigned p){
    const auto deadline=Clock::now()+std::chrono::seconds(3);
    while(Clock::now()<deadline){Socket s(::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(static_cast<std::uint16_t>(p));inet_pton(AF_INET,"127.0.0.2",&a.sin_addr);
        if(::connect(s.fd(),reinterpret_cast<sockaddr*>(&a),sizeof(a))==0){fcntl(s.fd(),F_SETFL,O_NONBLOCK);return s;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    fail("test_client_listener_timeout");
}
struct Join {
    std::atomic_bool& stop;std::thread& thread;
    ~Join(){stop.store(true);if(thread.joinable())thread.join();}
};
Bytes config(unsigned p,bool tcp,bool v21=false,unsigned priority=0,bool video_haptics=false,bool gaze=false,bool frame=false,bool lifecycle=false){
    Writer w(v21);w.string("{\"server_version\":\""+std::string(v21?"21.0.0-dev14":"20.14.1")+"\",\"session_settings\":{\"video\":{\"encoder_config\":{\"use_10bit\":"+std::string(v21?"{\"set\":false,\"content\":false}":"false")+"}},\"connection\":{\"stream_protocol\":{\"variant\":\""+std::string(tcp?"Tcp":"Udp")+"\"},\"stream_port\":"+std::to_string(p)+",\"packet_size\":1400},\"audio\":{\"microphone\":{\"enabled\":true},\"game_audio\":{\"enabled\":true}}}}");
    if(v21){w.integer(gaze?128:32,4);w.integer(gaze?128:32,4);w.real(90);w.integer(48000,4);w.boolean(false);w.real(1);w.boolean(false);w.boolean(false);
        std::string ext=priority?"{\""+std::string(tcp?"frame_split_streams":"frame_split_streams_udp")+"\":{\"version\":1,\"priority_port\":"+std::to_string(priority)+"}}":"{}";
        if(video_haptics)ext.insert(ext.size()-1,std::string(priority?",":"")+"\"frame_video_haptics\":1");
        if(gaze)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_gaze_streaming\":{\"version\":1,\"base_width\":32,\"base_height\":32,\"focus_width\":32,\"focus_height\":32},\"frame_video_timestamps\":1");
        if(frame)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_controller_inputs\":1,\"frame_boundary_sync\":1");
        if(lifecycle)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_session_lifecycle\":1");
        w.string(ext);}
    else w.string("{\"view_resolution\":[32,32],\"refresh_rate_hint\":90,\"game_audio_sample_rate\":48000,\"enable_foveated_encoding\":false}");
    return w.bytes;
}
void scenario(bool tcp,bool restart=false,bool v21=false,bool split=false,bool video_haptics=false,bool gaze=false,bool frame=false,bool microphone_recovery=false){
    const bool pressure=tcp&&v21;
    const bool predicted=!restart&&!pressure&&!split;
    const bool owned=(pressure&&split)||(!tcp&&v21&&!split);
    // Reserve dynamic ports on the server loopback IP first. Frame binds the
    auto control_reservation=Socket::bind("127.0.0.1",0,true);
    auto stream_reservation=Socket::bind("127.0.0.1",0,tcp);
    auto priority_reservation=Socket::bind("127.0.0.1",0,tcp);
    const auto priority_port=split?port(priority_reservation):0;
    const auto control_port=port(control_reservation),stream_port=port(stream_reservation);
    surfxr::SessionOptions options;options.backend=surfxr::SessionBackend::Surfxr;options.host="127.0.0.1";options.local_address="127.0.0.2";
    options.surfxr_allow_deprecated_tcp=tcp; // Preserve historical cases; production is UDP-only.
    options.surfxr_legacy_server_connect=true; // Preserve original server-dial scenarios.
    options.surfxr_control_port=control_port;options.surfxr_view_width=options.surfxr_view_height=32;
    options.surfxr_version=v21?"21.0.0-dev14":"20.14.1";
    options.eye_gaze_available=options.initial_foveated_streaming=gaze;
    if(gaze)options.surfxr_view_width=options.surfxr_view_height=128;
    options.surfxr_refresh_rates={90};options.seconds=8;options.microphone_enabled=true;options.initial_microphone_volume=1;
    options.haptics_queue=std::make_shared<surfxr::HapticsQueue>();
    options.control_queue=std::make_shared<surfxr::SessionControlQueue>();
    std::atomic_bool stop{false},configured{false},audio_received{false},microphone_started{false},microphone_stopped{false};
    std::atomic<unsigned> videos_received{0},video_offers{0},audio_packets{0};
    std::atomic<unsigned> owned_offers{0},owned_idr{0},owned_predicted{0};
    std::atomic_bool system_capture{true},controllers_enabled{true};
    std::atomic_bool hold_video{microphone_recovery},video_waiting{false};
    unsigned mic_starts=0,mic_stops=0;bool capture_failed=false;
    auto next_microphone=Clock::now();
    unsigned mic_reads=0;std::uint64_t sequence=0,gaze_sample_ns=0;
    surfxr::SessionCallbacks callbacks;
    callbacks.vr_controllers_enabled=[&]{return controllers_enabled.load();};
    auto boundary=std::make_shared<surfxr::StageBoundary>();boundary->origin_epoch=1;boundary->width=2;boundary->depth=3;
    boundary->quads.assign(3,{1,0,2,1,2,2,3,2,4,3,0,4});
    callbacks.tracking_frame=[&](surfxr::TrackingFrame& f){
        f.sequence=++sequence;f.origin_epoch=1;f.origin=surfxr::TrackingOrigin::Stage;
        f.stage_supported=f.stage_locatable=f.optics_valid=true;
        if(frame)f.boundary=boundary;
        // Exercise valid head upload without presentation/focus in the TCP
        // case, matching a runtime in SYNCHRONIZED while the HMD is idle.
        f.visible=f.focused=!tcp;
        if(frame){f.system_capture_active=system_capture.load();for(auto& hand:f.hands){
            hand.profile=surfxr::TrackingProfile::SteamFrame;hand.buttons_active=hand.buttons=surfxr::ButtonSystem|surfxr::ButtonBumper;
            hand.grip_active=true;hand.grip.flags=surfxr::OrientationValid|surfxr::PositionValid;}}
        f.head.flags=surfxr::OrientationValid|surfxr::PositionValid;f.head.pose.position={1,2,3};f.ipd_meters=.064f;
        f.eye_fov={{{-.7f,.7f,.7f,-.7f},{-.7f,.7f,.7f,-.7f}}};gaze_sample_ns=f.sampled_monotonic_ns=now_ns();return true;
    };
    callbacks.gaze_sample=[&](surfxr::GazeSample& s){s.origin_epoch=1;s.sampled_monotonic_ns=gaze_sample_ns;s.valid={true,true};s.positions={{{.25f,-.5f},{-.25f,.5f}}};return true;};
    callbacks.vr_config=[&](const surfxr::VrConfiguration& c){require(c.stereo_packed&&c.stream_count==1&&c.codec==2&&c.streams[0].width==64&&c.foveated==gaze&&c.frame_controller_inputs==frame,"test_session_decoder_config");configured.store(true);};
    callbacks.vr_video_packet=[&](auto id,const auto* bytes,auto size,auto stamp,auto metadata){
        if(microphone_recovery){video_waiting.store(true);while(hold_video.load()&&!stop.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        require(id==0&&metadata&&metadata->timestamp==stamp&&metadata->linear_layout==!gaze,"test_session_video_association");
        if(gaze)require(metadata->surfxr_tracking_timestamp+17==stamp,"test_capture_and_tracking_identity_separated");
        const bool headers=size==3005&&std::equal(bytes,bytes+5,Bytes{0,0,0,1,0x40}.begin());
        require((headers||size==3000)&&std::all_of(bytes+(headers?5:0),bytes+size,[](auto byte){return byte==0x26;}),"test_exact_idr_or_predicted_payload");
        ++videos_received;
        if(gaze)require(bool(metadata->gaze_atlas)&&metadata->regions[0].x==20&&metadata->regions[0].y==40&&metadata->regions[1].x==60&&metadata->regions[1].y==80,"test_session_exact_crops");
        if(v21)require(metadata->eye_poses[0].position.x==2&&metadata->eye_poses[1].position.x==3,"test_exact_server_render_poses");
    };
    if(pressure)callbacks.try_vr_video_packet=[&](auto id,const auto* bytes,auto size,auto stamp,auto metadata){
        if(++video_offers==1)return false;
        callbacks.vr_video_packet(id,bytes,size,stamp,std::move(metadata));return true;
    };
    if(owned){
        callbacks.try_vr_video_packet_owned=[&](auto id,Bytes&& bytes,std::size_t offset,auto stamp,auto metadata){
            require(offset<bytes.size(),"test_owned_au_offset_bound");++owned_offers;
            if(offset==0){require(bytes.size()==3005,"test_owned_idr_contains_decoder_config");++owned_idr;}
            else{require(bytes.size()-offset==3000,"test_owned_predicted_keeps_metadata_prefix");++owned_predicted;}
            if(pressure&&++video_offers==1)return false;
            callbacks.vr_video_packet(id,bytes.data()+offset,bytes.size()-offset,stamp,std::move(metadata));return true;
        };
        callbacks.try_vr_video_packet=[](auto,const auto*,auto,auto,auto)->bool{fail("test_owned_au_must_precede_borrowed_callback");return false;};
    }
    callbacks.audio_pcm=[&](const auto* data,auto size){require((size==8||size==8000)&&data[0]==1,"test_session_audio");if(size==8000)require(std::all_of(data,data+size,[](auto b){return b==1;}),"test_session_reordered_pcm");++audio_packets;audio_received.store(true);};
    callbacks.microphone_start=[&]{mic_reads=0;++mic_starts;if(microphone_recovery&&mic_starts==1)return false;microphone_started.store(true);return true;};
    callbacks.microphone_pcm=[&](auto* p,auto size){
        if(microphone_recovery){
            if(!capture_failed){capture_failed=true;throw std::runtime_error("synthetic_capture_failure");}
            if(Clock::now()<next_microphone)return std::size_t{0};
            next_microphone=Clock::now()+std::chrono::milliseconds(20);
        }else if(mic_reads++)return std::size_t{0};
        std::fill(p,p+size,0x10);return size;
    };
    callbacks.microphone_stop=[&]{++mic_stops;microphone_stopped.store(true);};
    surfxr::SessionResult result;std::thread client([&]{result=surfxr::transport::run(options,callbacks,&stop);});Join join{stop,client};
    Connection server;server.modern=v21;server.control_socket=connect(control_port);Framer framer(2*1024*1024,v21);
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    auto caps=receive_control(server.control_socket,framer,deadline,[] {return false;});Reader cap(caps,v21);
    require(cap.integer(4)==0&&cap.integer(8)==path_id(v21?"21-dev14":"20"),"test_session_capabilities");
    if(restart){
        server.send_control(config(stream_port,tcp,v21));server.send_control(control(2,v21));server.control_socket=Socket{};
        server.control_socket=connect(control_port);
        const auto retry=receive_control(server.control_socket,framer,deadline,[]{return false;});
        require(retry==caps,"test_capabilities_stable_across_restart");
    }
    for(unsigned phase=0;phase<(restart?2u:1u);++phase){
        if(phase){
            // SURFXR's old driver may reconnect and stream before the dashboard's
            // delayed process shutdown. Recover the ensuing unannounced EOF.
            if(!tcp)stream_reservation=Socket::bind("127.0.0.1",stream_port,false);
            server.control_socket=connect(control_port);
            require(receive_control(server.control_socket,framer,deadline,[]{return false;})==caps,"test_capabilities_stable_after_disconnect");
        }
        server.send_control(config(stream_port,tcp,v21,priority_port,video_haptics,gaze,frame));server.send_control(control(0,v21));
        const auto ready_bytes=receive_control(server.control_socket,framer,deadline,[] {return false;});Reader ready(ready_bytes,v21);
        require(ready.integer(4)==3,"test_stream_ready_after_bind");
        server.tcp=tcp;
        if(tcp)server.stream_socket=connect(stream_port);
        else{stream_reservation.connect_udp("127.0.0.2",stream_port);server.stream_socket=std::move(stream_reservation);}
        if(split){if(tcp)server.priority_socket=connect(priority_port);
            else{priority_reservation.connect_udp("127.0.0.2",priority_port);server.priority_socket=std::move(priority_reservation);}}
        Writer decoder(v21);decoder.integer(1,4);decoder.integer(1,4);decoder.blob({0,0,0,1,0x40});if(v21)decoder.string("{}");server.send_control(decoder.bytes);
        Reassembler assembler(1400,v21);Framer stream_framer(1400);TcpPacketFramer modern_tcp;std::array<std::uint8_t,65536> buffer{};
        std::uint64_t timestamp=0;bool microphone_received=false,request_received=false;
        unsigned native_profiles=0;bool native_pressed=false,native_released=false,boundary_received=false,recenter_received=false;
        auto next_keepalive=Clock::now();
        while(Clock::now()<deadline&&(!timestamp||!microphone_received||!request_received||(frame&&(!native_released||native_profiles!=2||!boundary_received)))){
            const auto n=(split?server.priority_socket:server.stream_socket).read(buffer.data(),buffer.size(),tcp);
            const auto on_packet=[&](Packet p){
                if(p.stream==0){Reader track(p.bytes,v21);timestamp=track.duration();require(track.integer(8)==(frame?3u:1u)&&track.integer(8)==path_id("/user/head"),"test_uploaded_tracking");
                    if(gaze){track.skip(52);for(unsigned i=0;i<7;++i)require(!track.boolean(),"test_gaze_preserves_native_suffix");
                        require(track.boolean()&&track.integer(8)<=timestamp&&track.integer(8)==1,"test_gaze_original_sample_clock");
                        require(track.boolean()&&track.real()==.25f&&track.real()==-.5f&&track.boolean()&&track.real()==-.25f&&track.real()==.5f,"test_gaze_uploaded_positions");track.end();}}
                if(p.stream==2){require(p.bytes.size()==1920&&p.bytes[0]==0x10,"test_uploaded_microphone");microphone_received=true;}
            };
            const auto on_shard=[&](const Bytes& b){auto p=assembler.accept(b,now_ns());if(p)on_packet(std::move(*p));};
            if(n){if(tcp&&v21)modern_tcp.feed(buffer.data(),n,on_packet);
                else if(tcp)stream_framer.feed(buffer.data(),n,[&](Bytes b){Bytes wire;big(wire,static_cast<unsigned>(b.size()),4);wire.insert(wire.end(),b.begin(),b.end());on_shard(wire);});
                else on_shard(Bytes(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(n)));}
            const auto c=server.control_socket.read(buffer.data(),buffer.size(),true);
            if(c)framer.feed(buffer.data(),c,[&](Bytes b){Reader r(b,v21);const auto type=r.integer(4);if(type==1)request_received=true;
                if(frame){if(type==0)recenter_received=true;
                    if(type==11){const auto body=r.blob();Reader bounds(body);require(recenter_received&&bounds.integer(4)==0x56444244&&bounds.integer(4)==1&&bounds.integer(8)==1,"boundary follows original recenter on production reliable control");boundary_received=true;}}
                if(frame&&type==7){r.integer(8);require(r.integer(8)==path_id("/interaction_profiles/valve/frame_controller_valve")&&r.integer(8)==48,"negotiated native profiles uploaded");++native_profiles;}
                if(frame&&type==6){require(r.integer(8)==48,"native complete input upload");unsigned systems=0,bumpers=0;
                    for(unsigned i=0;i<48;++i){const auto id=r.integer(8),scalar=r.integer(4);const auto value=scalar?r.real():float(r.boolean());
                        for(const auto* hand:{"left","right"}){const auto prefix=std::string("/user/hand/")+hand+"/input/";
                            if(id==path_id(prefix+"system/click")&&value)++systems;
                            if(id==path_id(prefix+"bumper/click")&&value)++bumpers;}}
                    require(bumpers==2,"ordinary input continues during system capture and after exit");
                    if(systems==2){native_pressed=true;system_capture.store(false);}else if(native_pressed&&systems==0)native_released=true;
                }});
            if(Clock::now()>=next_keepalive){server.send_control(control(3,v21));next_keepalive=Clock::now()+std::chrono::milliseconds(500);}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(timestamp&&microphone_received&&request_received,"test_session_upload_timeout");
        if(frame)require(native_profiles==2&&native_pressed&&native_released&&boundary_received,"native upload releases captured system keys and synchronizes boundary");
        if(frame){
            // Exercise the production Upload worker with a populated pose cache.
            // Dynamic loopback ports are reserved above; no live Streamer is used.
            for(bool enabled:{false,true}){
                controllers_enabled.store(enabled);unsigned matched=0;
                const auto toggle_deadline=Clock::now()+std::chrono::seconds(1);
                while(Clock::now()<toggle_deadline&&matched<3){
                    const auto n=server.stream_socket.read(buffer.data(),buffer.size(),false);
                    if(n){auto p=assembler.accept(Bytes(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(n)),now_ns());
                        if(p&&p->stream==0){Reader track(p->bytes,true);timestamp=track.duration();
                            const auto count=track.integer(8);
                            require(track.integer(8)==path_id("/user/head"),"hot toggle preserves head tracking");
                            if(count==(enabled?3u:1u))++matched;else matched=0;
                        }}
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                require(matched==3,"hot controller toggle must disconnect and reconnect production tracking motions");
            }
        }
        const auto video_packet=[&](bool idr,float amplitude=.4f){Writer video(v21);video.duration(timestamp);
            if(v21){for(unsigned eye=0;eye<2;++eye){video.real(0);video.real(0);video.real(0);video.real(1);video.real(float(eye+2));video.real(2);video.real(3);video.real(-.7f);video.real(.7f);video.real(.7f);video.real(-.7f);}video.boolean(false);}
            video.boolean(idr);if(video_haptics){video.real(amplitude);video.real(.25f);}
            if(gaze)for(auto value:{20u,40u,60u,80u})video.raw_integer(value,4);
            if(gaze)video.raw_integer(timestamp+17,8);
            video.bytes.insert(video.bytes.end(),3000,0x26);return video.bytes;
        };
        const auto video=video_packet(true);const auto fragments=shards(3,0,video,1400,v21);
        if(tcp&&v21)server.send_stream(3,video);
        else if(tcp)for(const auto& b:fragments)server.stream_socket.send(b,true,{});
        else for(auto it=fragments.rbegin();it!=fragments.rend();++it)server.stream_socket.send(*it,false,{});
        server.send_stream(2,{1,0,2,0,3,0,4,0});
        if(microphone_recovery){
            bool uplink=false;const auto mic_deadline=Clock::now()+std::chrono::milliseconds(300);
            // Block the production video callback. Mono upload must still
            // arrive over the reserved priority socket while it is blocked.
            while(Clock::now()<mic_deadline&&(!uplink||!video_waiting.load())){
                const auto n=server.priority_socket.read(buffer.data(),buffer.size(),false);
                if(n){auto p=assembler.accept(Bytes(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(n)),now_ns());
                    if(p&&p->stream==2&&video_waiting.load()){require(p->bytes.size()==1920&&p->bytes[0]==0x10,"recovered_pcm_exact_bytes");uplink=true;}}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(uplink&&video_waiting.load(),"microphone_upload_blocked_by_video_callback");hold_video.store(false);
        }
        if(video_haptics){
            bool delivered=false;const auto haptic_deadline=Clock::now()+std::chrono::milliseconds(100);
            while(Clock::now()<haptic_deadline&&!delivered){const auto h=options.haptics_queue->consume(now_ns(),true);
                if(h[0].changed&&h[0].amplitude==.4f){require(h[1].changed&&h[1].amplitude==.25f&&h[0].duration_ns==30'000'000&&h[0].frequency==0,"test_video_haptics_same_surfxr_pulse");delivered=true;}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(delivered,"test_video_haptics_before_decoder_acceptance");
        }
        if(pressure){
            // The first IDR was rejected immediately after the handshake's
            // request. Recovery must not wait for its old 250 ms cooldown.
            const auto recovery_deadline=Clock::now()+std::chrono::milliseconds(150);
            request_received=false;
            while(Clock::now()<recovery_deadline&&!request_received){
                const auto c=server.control_socket.read(buffer.data(),buffer.size(),true);
                if(c)framer.feed(buffer.data(),c,[&](Bytes b){Reader r(b,v21);if(r.integer(4)==1)request_received=true;});
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(request_received,"test_new_decoder_gap_requests_idr_immediately");
            server.send_stream(3,video_packet(false));
            while(Clock::now()<deadline&&!audio_received.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(audio_received.load()&&video_offers.load()==1&&videos_received.load()==0,"test_audio_continues_while_video_waits_for_idr");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            // With no more video, retries must still make progress, while
            // prediction packets remain suppressed until an accepted IDR.
            request_received=false;
            while(Clock::now()<deadline&&!request_received){
                const auto c=server.control_socket.read(buffer.data(),buffer.size(),true);
                if(c)framer.feed(buffer.data(),c,[&](Bytes b){Reader r(b,v21);if(r.integer(4)==1)request_received=true;});
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(request_received&&video_offers.load()==1,"test_rejected_reference_chain_requests_idr");
            server.send_stream(3,video);
        }
        Writer pulse(v21);pulse.integer(path_id("/user/hand/left"),8);pulse.duration(50'000'000);pulse.real(100);pulse.real(.4f);if(!video_haptics)server.send_stream(1,pulse.bytes);
        while(Clock::now()<deadline&&(videos_received.load()<=phase||!audio_received.load()))std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(configured.load()&&videos_received.load()>phase&&audio_received.load()&&microphone_started.load(),"test_session_callbacks");
        if(split&&tcp){
            // Leave the next video packet incomplete until teardown. Independent
            // audio and control must progress without waiting for its tail.
            Writer prefix;prefix.integer(3,2);prefix.integer(3,4);prefix.integer(video.size(),4);
            prefix.bytes.insert(prefix.bytes.end(),video.begin(),video.begin()+100);
            server.stream_socket.send(prefix.bytes,true,{});
            audio_received.store(false);server.send_stream(2,{1,0,2,0,3,0,4,0});
            require(options.control_queue->force_keyframe(),"test_control_enqueue");request_received=false;
            const auto priority_deadline=Clock::now()+std::chrono::milliseconds(200);
            while(Clock::now()<priority_deadline&&(!audio_received.load()||!request_received)){
                const auto c=server.control_socket.read(buffer.data(),buffer.size(),true);
                if(c)framer.feed(buffer.data(),c,[&](Bytes b){Reader r(b,true);if(r.integer(4)==1)request_received=true;});
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(audio_received.load()&&request_received&&video_offers.load()==2,"test_audio_and_control_bypass_incomplete_video");
        }
        if(split&&!tcp){
            // Drop the rest of a video packet and an audio packet. A subsequent
            // reordered audio packet, haptic pulse, tracking and reliable IDR
            // must progress through their real independent sockets.
            server.stream_socket.send(shards(3,1,video,1400,true).front(),false,{});
            const auto incomplete=shards(2,1,Bytes(8000,1),1400,true);
            server.priority_socket.send(incomplete.front(),false,{});
            const auto reordered=shards(2,2,Bytes(8000,1),1400,true);
            const auto initial_audio=audio_packets.load();
            for(auto it=reordered.rbegin();it!=reordered.rend();++it)server.priority_socket.send(*it,false,{});
            if(video_haptics){const auto next=shards(3,2,video_packet(true,.7f),1400,true);for(auto it=next.rbegin();it!=next.rend();++it)server.stream_socket.send(*it,false,{});}
            else{pulse.bytes.clear();pulse.integer(path_id("/user/hand/left"),8);pulse.duration(50'000'000);pulse.real(100);pulse.real(.7f);server.send_stream(1,pulse.bytes);}
            require(options.control_queue->force_keyframe(),"test_udp_control_enqueue");request_received=false;
            bool tracking_received=false,haptic_received=false;
            const auto priority_deadline=Clock::now()+std::chrono::milliseconds(200);
            while(Clock::now()<priority_deadline&&(audio_packets.load()==initial_audio||!request_received||!tracking_received||!haptic_received)){
                const auto n=server.priority_socket.read(buffer.data(),buffer.size(),false);
                if(n){auto p=assembler.accept(Bytes(buffer.begin(),buffer.begin()+static_cast<std::ptrdiff_t>(n)),now_ns());if(p&&p->stream==0)tracking_received=true;}
                const auto c=server.control_socket.read(buffer.data(),buffer.size(),true);
                if(c)framer.feed(buffer.data(),c,[&](Bytes b){Reader r(b,true);if(r.integer(4)==1)request_received=true;});
                const auto h=options.haptics_queue->consume(now_ns(),true)[0];if(h.changed&&h.amplitude==.7f)haptic_received=true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(audio_packets.load()==initial_audio+1&&request_received&&tracking_received&&haptic_received&&videos_received.load()==(video_haptics?2u:1u),"test_udp_priority_bypasses_video_loss");
        }
        if(video_haptics){
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
            const auto h=options.haptics_queue->consume(now_ns(),true);require(h[0].changed&&h[0].amplitude==0&&h[0].duration_ns==0,"test_video_stall_stops_haptics");
            if(!tcp){
                // A queued source packet can carry newer amplitude state with
                // an old tracking timestamp. Decode it without reviving feedback.
                std::this_thread::sleep_for(std::chrono::milliseconds(110));
                const auto stale=shards(3,3,video_packet(true,.9f),1400,true);
                for(const auto& b:stale)server.stream_socket.send(b,false,{});
                while(Clock::now()<deadline&&videos_received.load()<3)std::this_thread::sleep_for(std::chrono::milliseconds(1));
                require(videos_received.load()==3&&!options.haptics_queue->consume(now_ns(),true)[0].changed,"test_stale_video_does_not_revive_haptics");
            }
        }
        if(restart&&!phase){server.control_socket=Socket{};server.stream_socket=Socket{};}
        if(predicted){
            // The next predicted picture has no decoder headers. Callback
            // consumers must receive its exact body on both protocol versions.
            const auto next=video_packet(false);
            if(tcp&&v21)server.send_stream(3,next);
            else for(const auto& fragment:shards(3,1,next,1400,v21))server.stream_socket.send(fragment,tcp,{});
            while(Clock::now()<deadline&&videos_received.load()<2)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(videos_received.load()==2,"test_predicted_picture_delivered");
        }
    }
    stop.store(true);client.join();
    require(mic_stops==mic_starts,"capture_attempts_stopped_before_teardown");
    if(microphone_recovery)require(mic_starts==3&&result.sanitized_json.find("\"capture_failures\":2")!=std::string::npos,"microphone_start_and_read_recovery");
    const auto expected_frames=video_haptics&&!tcp?3u:((restart||pressure||predicted)?2u:1u);
    require(result.exit_code==0&&result.connection_closed&&result.received_frames==expected_frames&&microphone_stopped.load(),"test_ordered_teardown");
    if(pressure)require(video_offers.load()==2&&result.sanitized_json.find("\"video_backpressure_drops\":1")!=std::string::npos,"test_backpressure_report_and_idr_recovery");
    if(owned)require(owned_offers.load()==expected_frames&&owned_idr.load()>=1&&(!predicted||owned_predicted.load()==1),"test_owned_au_idr_prediction_and_backpressure");
    require(result.sanitized_json.find(split?"\"split_streams\":true":"\"split_streams\":false")!=std::string::npos,"test_split_transport_report");
    require(result.sanitized_json.find(video_haptics?"\"video_haptics\":true":"\"video_haptics\":false")!=std::string::npos,"test_video_haptics_report");
    require(result.sanitized_json.find(gaze?"\"gaze_streaming\":true":"\"gaze_streaming\":false")!=std::string::npos,"test_gaze_report");
    require(result.sanitized_json.find(restart?"\"reconnects\":2":"\"reconnects\":0")!=std::string::npos,"test_restart_report");
}
}
Bytes client_connection_config(unsigned port,bool lifecycle){return config(port,false,true,0,false,false,false,lifecycle);}
Bytes client_unified_connection_config(unsigned media,unsigned priority,bool video_haptics){return config(media,false,true,priority,video_haptics,false,false,true);}
void client_connect_checks();
namespace surfxr::transport {void desktop_connection_checks();}
namespace surfxr::transport {void unified_session_checks();void settings_sync_checks();}
namespace surfxr::transport {void steamvr_start_checks();void controller_state_checks();}
namespace surfxr::transport {void pose_upload_checks();}
int main(){try{pose_upload_checks();controller_state_checks();settings_sync_checks();steamvr_start_checks();unified_session_checks();desktop_connection_checks();client_connect_checks();scenario(false);scenario(true);scenario(false,true);scenario(false,false,true);scenario(true,false,true);scenario(true,false,true,true);scenario(false,false,true,true);scenario(true,false,true,true,true);scenario(false,false,true,true,true);scenario(false,false,true,true,true,true);scenario(false,false,true,false,false,false,true);scenario(false,false,true,true,false,false,false,true);std::cout<<"SURFXR isolated UDP/TCP handshake, client initiation, stop, restart, split media, gaze, video haptics, native Frame input, microphone recovery, tracking and teardown passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
