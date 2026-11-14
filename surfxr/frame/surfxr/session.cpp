#include "surfxr/surfxr.hpp"
#include "surfxr/haptics.hpp"
#include "upload.hpp"
#include "priority.hpp"
#include "microphone.hpp"
#include "start.hpp"
#include "session_deadline.hpp"
#include "unified.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <poll.h>
#include <sstream>
#include <unistd.h>

namespace surfxr::transport {
namespace {
struct Restarting {};
struct ServerStopped {};
struct Totals {
    std::uint64_t frames=0,lost=0,unmatched=0,backpressure_drops=0;
    std::atomic<std::uint64_t> audio_bytes{0};
    bool connected=false,launch_requested=false;
    MicrophoneUploadStats microphone;
    std::unique_ptr<SteamVrStartupMonitor> startup;
};
bool retryable(const std::string& error){
    return error=="surfxr_peer_closed"||error=="surfxr_receive_failed"||error=="surfxr_send_failed"||
        error=="surfxr_send_timeout"||error=="surfxr_handshake_timeout"||error=="surfxr_keepalive_timeout"||error=="surfxr_accept_timeout"||
        error=="surfxr_connect_failed"||error=="surfxr_connect_timeout"||error=="surfxr_first_video_timeout";
}
SessionResult attempt(const SessionOptions& options,const SessionCallbacks& callbacks,const std::atomic_bool* stop,
        Clock::time_point end,Clock::time_point recovery,Totals& totals){
    SessionResult result;result.received_frames=totals.frames;result.connection_started=totals.connected;
    auto& lost=totals.lost;auto& unmatched=totals.unmatched;auto& audio_bytes=totals.audio_bytes;
    const char* stage="surfxr_preflight";std::string error;bool video_haptics=false,gaze_streaming=false,server_stopped=false,priority_receiver_started=false;
    Connection connection;std::atomic_bool closing{false};
    SessionDeadline deadline(end,recovery);
    ReceiveCounters local_receive;auto& receive=options.surfxr_feedback?options.surfxr_feedback->receive:local_receive;
    connection.cancelled=[&]{
        if(closing.load()||(stop&&stop->load()))return true;
        if(totals.startup)totals.startup->check();
        return deadline.expired(Clock::now(),totals.startup&&totals.startup->pending());
    };
    std::unique_ptr<Upload> upload;
    std::unique_ptr<MicrophoneUpload> microphone;
    std::unique_ptr<PriorityReceiver> priority;
    TrackingLedger ledger;
    std::shared_ptr<PoseClock> pose_clock;
    if(options.keyboard_overlay)options.keyboard_overlay->publish({});
    try{
        if(connection.cancelled())throw Cancelled{};
        require(options.seconds<=3600&&options.surfxr_control_port>=1024&&options.surfxr_control_port<=65535,"surfxr_invalid_options");
        auto rates=options.surfxr_refresh_rates;
        if(rates.empty())rates.push_back(float(options.initial_vr_framerate));
        const bool v21=is_v21(options.surfxr_version);
        const bool client_connect=v21&&!options.surfxr_legacy_server_connect;
        std::array<char,64> hostname{};
        if(client_connect){
            require(::gethostname(hostname.data(),hostname.size()-1)==0,"surfxr_client_hostname_failed");
            for(auto& c:hostname)if(c&&!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))c='-';
        }
        connection.modern=v21;
        const bool gaze_requested=v21&&options.eye_gaze_available&&options.initial_foveated_streaming;
        const bool pose_requested=v21&&options.pose_time_available&&bool(callbacks.sample_tracking)&&bool(callbacks.timestamped_tracking);
        const auto caps=capabilities(options.host,options.surfxr_view_width,options.surfxr_view_height,rates,options.surfxr_version,v21,!options.surfxr_allow_deprecated_tcp,gaze_requested,v21,hostname.data(),false,false,pose_requested);
        stage="surfxr_wait_server";
        if(client_connect){
            if(!totals.startup){
                if(!totals.launch_requested&&!totals.connected)totals.launch_requested=request_steamvr(options,hostname.data(),connection.cancelled);
                totals.startup=std::make_unique<SteamVrStartupMonitor>(options,hostname.data(),callbacks,!totals.launch_requested);
            }
            totals.startup->wait_ready([&]{if(options.surfxr_unified)options.surfxr_unified->check();return connection.cancelled();});
        }
        if(options.surfxr_unified){
            connection.control_socket=options.surfxr_unified->begin(false,caps);
        }else if(client_connect){
            stage="surfxr_connect_server";
            connection.control_socket=Socket::connect_tcp(options.host,options.surfxr_control_port,options.local_address,
                std::min(end,Clock::now()+std::chrono::seconds(2)),connection.cancelled);
        }else{
            auto listener=Socket::bind(options.local_address,options.surfxr_control_port,true);
            connection.control_socket=listener.accept(options.host,end,connection.cancelled);
        }
        if(!options.surfxr_unified)connection.send_control(caps);
        stage="surfxr_handshake";Framer control_framer(2*1024*1024,v21);
        auto config=stream_config(receive_control(connection.control_socket,control_framer,Clock::now()+std::chrono::seconds(10),connection.cancelled),options.surfxr_version,options.surfxr_allow_deprecated_tcp);
        require(!config.desktop,"surfxr_desktop_requires_explicit_mode");
        validate_pose_clock(config,pose_requested);
        if(config.pose_time)pose_clock=std::make_shared<PoseClock>();
        if(callbacks.timestamped_tracking)callbacks.timestamped_tracking(config.pose_time);
        if(options.surfxr_unified){config.game_audio=false;config.microphone=false;}
        video_haptics=config.video_haptics;
        gaze_streaming=bool(config.gaze_atlas);
        require(!gaze_streaming||gaze_requested,"surfxr_unsolicited_gaze_config");
        require(!config.image_timestamps||gaze_streaming,"surfxr_unsolicited_image_timestamp");
        const auto start=server_control(receive_control(connection.control_socket,control_framer,Clock::now()+std::chrono::seconds(10),connection.cancelled),v21);
        if(start.type==2)throw Restarting{};
        if(config.session_lifecycle&&start.type==5&&start.reserved=="frame_session_stopped")throw ServerStopped{};
        require(start.type==0,"surfxr_start_stream_expected");
        require(config.port!=options.surfxr_control_port,"surfxr_port_collision");
        require(config.priority_port!=options.surfxr_control_port,"surfxr_port_collision");
        connection.tcp=config.tcp;connection.packet_size=config.packet_size;
        // A 256 KiB request advertises only ~182 KiB on Frame, smaller than a
        // single 200 Mbps video packet. Leave bounded room for a packet plus
        // wireless RTT variation, instead of stalling the shared audio sender.
        auto stream=options.surfxr_unified?options.surfxr_unified->video():Socket::bind(options.local_address,config.port,config.tcp,config.tcp?1024*1024:0);
        Socket priority_listener;
        if(config.priority_port)priority_listener=options.surfxr_unified?options.surfxr_unified->priority():Socket::bind(options.local_address,config.priority_port,config.tcp,128*1024);
        connection.send_control(control(3,v21)); // StreamReady after binding.
        stage="surfxr_stream_accept";
        if(config.tcp)connection.stream_socket=stream.accept(options.host,Clock::now()+std::chrono::seconds(10),connection.cancelled);
        else{stream.connect_udp(options.host,config.port);connection.stream_socket=std::move(stream);}
        if(config.priority_port){
            if(config.tcp)connection.priority_socket=priority_listener.accept(options.host,Clock::now()+std::chrono::seconds(10),connection.cancelled);
            else{priority_listener.connect_udp(options.host,config.priority_port);connection.priority_socket=std::move(priority_listener);}
            priority_listener=Socket{};
            // Unified audio is owned by the persistent outer receiver. When
            // haptics ride video there is no inner priority stream to consume.
            if(config.game_audio||!config.video_haptics){
                priority=std::make_unique<PriorityReceiver>(connection,options,callbacks,config.game_audio,config.video_haptics,audio_bytes,receive);
                priority_receiver_started=true;
            }
        }
        result.connection_started=true;if(callbacks.connection_started)callbacks.connection_started();
        // Timed sessions locate all devices at each quarter-frame upload.
        // Legacy sessions retain SURFXR's three uploads per frame and snapshot
        // sequence. Both paths register their identity before sending.
        upload=std::make_unique<Upload>(connection,ledger,options,callbacks,config.refresh_rate*(pose_clock?4:3),gaze_streaming,config.frame_inputs,config.frame_skeletons,config.boundary_sync,config.controller_state,pose_clock);
        if(config.microphone&&options.microphone_enabled&&callbacks.microphone_start&&callbacks.microphone_pcm&&callbacks.microphone_stop)
            microphone=std::make_unique<MicrophoneUpload>(connection,options,callbacks,totals.microphone);
        receive.video_fec.store(config.video_fec);
        stage="surfxr_streaming";Reassembler assembler(config.packet_size,v21,8*1024*1024,config.priority_port?1u<<3:0x1f,config.tcp?0:2'000'000,config.video_fec);Framer stream_framer(config.packet_size);
        TcpPacketFramer modern_tcp(8*1024*1024,config.priority_port?1u<<3:0x1f);
        Bytes decoder_config;unsigned codec=0;bool configured=false,corrupted=true,decoder_backpressure=false;
        auto next_keepalive=Clock::now(),last_control=next_keepalive,last_idr=Clock::time_point::min();
        const auto request_idr=[&]{
            if(last_idr==Clock::time_point::min()||Clock::now()-last_idr>=std::chrono::milliseconds(config.tcp?250:50)){
                connection.send_control(control(1,v21));last_idr=Clock::now();++receive.idr_requests;}
        };
        const auto on_control=[&](Bytes body){
            const auto packet=server_control(body,v21);last_control=Clock::now();
            if(packet.type==6&&pose_clock){pose_clock->observe(packet.config,now_ns());return;}
            if(packet.type==2)throw Restarting{};
            if(config.session_lifecycle&&packet.type==5&&packet.reserved=="frame_session_stopped")throw ServerStopped{};
            if(packet.type!=1)return;
            if(configured&&codec==packet.codec&&decoder_config==packet.config)return;
            decoder_config=packet.config;codec=packet.codec;configured=true;corrupted=true;decoder_backpressure=false;
            last_idr=Clock::time_point::min();
            VrConfiguration vr;vr.codec=codec==0?1:2;vr.frame_rate=static_cast<unsigned>(std::lround(config.refresh_rate));
            vr.stream_count=1;vr.render_width=config.width;vr.render_height=config.height;vr.stereo_packed=true;
            vr.frame_controller_inputs=config.frame_inputs;vr.gaze_atlas=config.gaze_atlas;vr.foveated=gaze_streaming;
            vr.streams.push_back({0,0,gaze_streaming?config.gaze_atlas.base_width*2:config.width*2,
                gaze_streaming?config.gaze_atlas.base_height+config.gaze_atlas.focus_height:config.height,false});
            if(options.haptics_queue)options.haptics_queue->clear();
            if(callbacks.vr_config)callbacks.vr_config(vr);
            if(callbacks.source_changed)callbacks.source_changed(1);
            if(totals.startup)totals.startup->driver_ready();
            request_idr();
        };
        const auto on_packet=[&](Packet value){auto* packet=&value;
            if(packet->stream==3){
                receive.video.packet(now_ns(),packet->loss);
                Reader r(packet->bytes,v21);const auto header=video_header(r,v21,config.video_haptics,gaze_streaming,config.image_timestamps);
                const auto stamp=header.image_timestamp,tracking_stamp=header.timestamp;const bool idr=header.idr;
                if(gaze_streaming)for(const auto& crop:header.crops)require(crop[0]<=config.width-config.gaze_atlas.focus_width&&
                    crop[1]<=config.height-config.gaze_atlas.focus_height&&crop[0]%2==0&&crop[1]%2==0,"surfxr_invalid_gaze_crop");
                const auto video_offset=static_cast<std::size_t>(r.data()-packet->bytes.data());
                if(options.surfxr_unified&&options.surfxr_unified->keyboard()&&options.keyboard_overlay)
                    keyboard_image::extract(packet->bytes,video_offset,*options.keyboard_overlay);
                r=Reader(packet->bytes.data()+video_offset,packet->bytes.size()-video_offset,v21);
                require(r.remaining()>0,"surfxr_empty_video_packet");
                const auto received=now_ns();
                // Source metadata is processed before decoder/reference-chain
                if(config.video_haptics&&configured&&options.haptics_queue){
                    if(tracking_stamp&&received>=tracking_stamp&&received-tracking_stamp<surfxr::HapticsQueue::freshness_ns)
                        options.haptics_queue->ingest_video_amplitudes(stamp,1,header.haptics,received);
                    else options.haptics_queue->reject_stale_video(header.haptics[0]>0||header.haptics[1]>0);
                }
                if(packet->loss){++lost;corrupted=true;if(!idr)request_idr();}
                if(!configured){request_idr();return;}
                if(idr)corrupted=false;
                if(corrupted){++receive.reference_skips;request_idr();return;}
                auto meta=ledger.metadata(tracking_stamp,config.width,config.height,now_ns(),stamp);
                if(meta&&header.has_views){auto exact=std::make_shared<VrVideoMetadata>(*meta);exact->eye_poses=header.poses;exact->fov=header.fov;meta=std::move(exact);}
                if(meta&&gaze_streaming){auto exact=std::make_shared<VrVideoMetadata>(*meta);exact->linear_layout=false;exact->gaze_atlas=config.gaze_atlas;
                    for(unsigned eye=0;eye<2;++eye){exact->regions[eye].x=static_cast<std::int32_t>(header.crops[eye][0]);exact->regions[eye].y=static_cast<std::int32_t>(header.crops[eye][1]);}meta=std::move(exact);}
                if(!meta)++unmatched; // Decode references, but never invent a pose.
                Bytes access_unit;
                const auto* encoded=r.data();auto encoded_size=r.remaining();
                if(idr){
                    access_unit.reserve(decoder_config.size()+encoded_size);
                    access_unit.insert(access_unit.end(),decoder_config.begin(),decoder_config.end());
                    access_unit.insert(access_unit.end(),encoded,encoded+encoded_size);
                    encoded=access_unit.data();encoded_size=access_unit.size();
                }
                // Transfer the assembled owner to an asynchronous decoder when
                // supported. The encoded offset leaves metadata bytes outside
                // the AU without copying ordinary pictures; IDR config keeps
                // the existing combined access unit. Borrowed callers retain
                // the synchronous callback contract.
                bool accepted=true;
                if(callbacks.try_vr_video_packet_owned){
                    const auto offset=idr?0u:static_cast<std::size_t>(encoded-packet->bytes.data());
                    accepted=callbacks.try_vr_video_packet_owned(0,idr?std::move(access_unit):std::move(packet->bytes),offset,stamp,std::move(meta));
                }else if(callbacks.try_vr_video_packet)
                    accepted=callbacks.try_vr_video_packet(0,encoded,encoded_size,stamp,std::move(meta));
                else if(callbacks.vr_video_packet)callbacks.vr_video_packet(0,encoded,encoded_size,stamp,std::move(meta));
                if(!accepted){
                    // Keep socket ingestion nonblocking (compatibility
                    // sessions still share audio) and reject a broken chain.
                    ++totals.backpressure_drops;corrupted=true;
                    // A new decoder gap needs its own request immediately,
                    // even if the previous handshake/loss request was recent.
                    // Rejected recovery IDRs retain the retry rate limit.
                    if(!decoder_backpressure)last_idr=Clock::time_point::min();
                    decoder_backpressure=true;request_idr();
                }
                if(idr&&accepted){last_idr=Clock::time_point::min();decoder_backpressure=false;}
                if(accepted){deadline.connected();if(totals.startup)totals.startup->video_ready();}
                ++result.received_frames;
            }else if(packet->stream==2&&config.game_audio){
                require(packet->bytes.size()%4==0&&packet->bytes.size()<=192000,"surfxr_invalid_audio_packet");
                audio_bytes+=packet->bytes.size();if(callbacks.audio_pcm)callbacks.audio_pcm(packet->bytes.data(),packet->bytes.size());
            }else if(packet->stream==1){
                require(!config.video_haptics,"surfxr_unexpected_haptic_stream");
                const auto pulse=haptic(packet->bytes,v21);
                if(options.haptics_queue)options.haptics_queue->ingest_pulse(pulse.hand,pulse.amplitude,pulse.duration_ns,pulse.frequency,now_ns());
            }
        };
        const auto on_shard=[&](const Bytes& bytes){auto packet=assembler.accept(bytes,now_ns());if(packet)on_packet(std::move(*packet));};
        std::array<std::uint8_t,65536> read_buffer{};
        Bytes datagram_batch(32*(config.packet_size+(v21?0:4)));std::uint64_t reordered=0,late_shards=0,reassembly_drops=0;
        std::uint64_t repaired_shards=0,repaired_packets=0;
        const auto first_video_deadline=Clock::now()+std::chrono::seconds(30);
        const auto initial_frames=totals.frames;
        while(!connection.cancelled()){
            require(result.received_frames>initial_frames||Clock::now()<first_video_deadline,"surfxr_first_video_timeout");
            if(options.surfxr_unified)options.surfxr_unified->check();
            upload->check();
            if(microphone)microphone->check();
            if(priority)priority->check();
            for(unsigned i=0;i<16;++i){const auto n=connection.control_socket.read(read_buffer.data(),read_buffer.size(),true);if(!n)break;control_framer.feed(read_buffer.data(),n,on_control);}
            if(config.tcp)for(unsigned i=0;i<32;++i){const auto n=connection.stream_socket.read(read_buffer.data(),read_buffer.size(),true);if(!n)break;
                if(config.tcp&&v21)modern_tcp.feed(read_buffer.data(),n,on_packet);
                else if(config.tcp)stream_framer.feed(read_buffer.data(),n,[&](Bytes body){Bytes shard;big(shard,static_cast<std::uint32_t>(body.size()),4);shard.insert(shard.end(),body.begin(),body.end());on_shard(shard);});
            }else{
                connection.stream_socket.read_datagrams(datagram_batch.data(),config.packet_size+(v21?0:4),32,[&](const auto* p,std::size_t n){
                    ++receive.video.datagrams;receive.video.bytes.fetch_add(n,std::memory_order_relaxed);
                    auto packet=assembler.accept(p,n,now_ns());if(packet)on_packet(std::move(*packet));});
                while(auto packet=assembler.pop_ready(now_ns()))on_packet(std::move(*packet));
                receive.video.reordered.fetch_add(assembler.reordered()-reordered,std::memory_order_relaxed);reordered=assembler.reordered();
                receive.video.late_shards.fetch_add(assembler.late_shards()-late_shards,std::memory_order_relaxed);late_shards=assembler.late_shards();
                receive.video.reassembly_drops.fetch_add(assembler.dropped()-reassembly_drops,std::memory_order_relaxed);reassembly_drops=assembler.dropped();
                receive.video.fec_recovered_shards.fetch_add(assembler.recovered_shards()-repaired_shards,std::memory_order_relaxed);repaired_shards=assembler.recovered_shards();
                receive.video.fec_recovered_packets.fetch_add(assembler.recovered_packets()-repaired_packets,std::memory_order_relaxed);repaired_packets=assembler.recovered_packets();
            }
            if(options.control_queue){SessionControlCommand command;for(unsigned i=0;i<32&&options.control_queue->pop(command);++i){
                if(command.type==SessionCommand::Keyframe||command.type==SessionCommand::Restart)request_idr();
            }}
            if(callbacks.video_tick)callbacks.video_tick();
            // Retry recovery even when the PC emits no further video packets.
            if(!configured||corrupted)request_idr();
            // In unified mode the daemon owns liveness and reports actual
            // source closure. A busy SteamVR control loop is not a lost client.
            if(!options.surfxr_unified)require(Clock::now()-last_control<std::chrono::seconds(2),"surfxr_keepalive_timeout");
            if(Clock::now()>=next_keepalive){connection.send_control(control(2,v21));next_keepalive=Clock::now()+std::chrono::milliseconds(500);}
            // Packet arrival wakes immediately; idle work runs only at a
            // reassembly/control/recovery deadline. The bounded cap observes
            // local commands and mode cancellation without a 1 ms spin.
            auto wait_until=std::min(Clock::now()+std::chrono::milliseconds(20),next_keepalive);
            if((!configured||corrupted)&&last_idr!=Clock::time_point::min())
                wait_until=std::min(wait_until,last_idr+std::chrono::milliseconds(config.tcp?250:50));
            if(!config.tcp)if(const auto ready=assembler.next_ready_ns(now_ns()))
                wait_until=std::min(wait_until,Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(*ready))));
            const auto wait_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(wait_until-Clock::now()).count();
            const int timeout=wait_ns>0?static_cast<int>(std::min<std::int64_t>(20,(wait_ns+999999)/1000000)):0;
            pollfd fds[2]{{connection.control_socket.fd(),POLLIN,0},{connection.stream_socket.fd(),POLLIN,0}};::poll(fds,2,timeout);
        }
        result.exit_code=result.received_frames?0:2;
    }catch(const ServerStopped&){server_stopped=true;result.server_stopped=true;result.exit_code=0;}
    catch(const Restarting&){error="surfxr_server_restarting";result.exit_code=3;}
    catch(const Cancelled&){result.exit_code=result.received_frames?0:2;}
    catch(const std::exception& e){const std::string code=e.what();error=code.rfind("surfxr_",0)==0&&code.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")==std::string::npos?code:"surfxr_callback_failed";result.exit_code=retryable(error)?3:1;}
    catch(...){error="surfxr_unknown_failure";result.exit_code=1;}
    // Stop independent uploads before closing sockets or captured callbacks.
    closing.store(true);microphone.reset();upload.reset();priority.reset();
    if(callbacks.timestamped_tracking)try{callbacks.timestamped_tracking(false);}catch(...){error="surfxr_callback_failed";result.exit_code=1;}
    if(options.haptics_queue)options.haptics_queue->clear();
    if(callbacks.connection_ended)try{callbacks.connection_ended();}catch(...){error="surfxr_callback_failed";result.exit_code=1;}
    totals.frames=result.received_frames;totals.connected=result.connection_started;
    result.connection_closed=true;
    // Retain a stable numeric diagnostic through the launcher's text filter.
    // This fingerprints the already sanitized local check name, never payloads.
    std::uint32_t error_id=0;
    if(!error.empty()){error_id=2166136261u;for(unsigned char c:error)error_id=(error_id^c)*16777619u;}
    std::ostringstream json;json<<"{\"backend\":\"surfxr\",\"protocol_version\":\""<<(options.surfxr_version=="21.0.0-dev14"?"21.0.0-dev14":"20.14.1")<<"\",\"stage\":\""<<stage
        <<"\",\"status\":\""<<(server_stopped?"server_stopped":error.empty()?(result.received_frames?"frames_received":"cancelled"):"failed")
        <<"\",\"error\":\""<<error<<"\",\"error_id\":"<<error_id<<",\"frames\":"<<result.received_frames<<",\"video_packet_gaps\":"<<lost
        <<",\"unmatched_tracking\":"<<unmatched<<",\"audio_bytes\":"<<audio_bytes.load()
        <<",\"split_streams\":"<<(connection.priority_socket.fd()>=0?"true":"false")
        <<",\"video_haptics\":"<<(video_haptics?"true":"false")
        <<",\"priority_receiver\":"<<(priority_receiver_started?"true":"false")
        <<",\"gaze_streaming\":"<<(gaze_streaming?"true":"false")
        <<",\"server_stopped\":"<<(server_stopped?"true":"false")
        <<",\"microphone_upload\":{\"start_attempts\":"<<totals.microphone.start_attempts
        <<",\"capture_failures\":"<<totals.microphone.capture_failures<<",\"packets\":"<<totals.microphone.packets<<'}'
        <<",\"video_backpressure_drops\":"<<totals.backpressure_drops<<",\"connection_closed\":true,\"udp_receive\":";
    write_receive_json(json,receive.snapshot());json<<'}';
    result.sanitized_json=json.str();return result;
}
}
SessionResult run(const SessionOptions& options,const SessionCallbacks& callbacks,const std::atomic_bool* stop){
    if(options.surfxr_desktop)return run_desktop(options,callbacks,stop);
    const auto end=options.seconds?Clock::now()+std::chrono::seconds(options.seconds):Clock::time_point::max();
    SessionResult result;Totals totals;totals.launch_requested=options.surfxr_start_prepared;unsigned reconnects=0;
    auto recovery_deadline=Clock::time_point::max();
    bool exhausted=false;std::string last_retry_report;
    // SURFXR changes its OpenVR encoder/display configuration before streaming.
    // Keep the caller's XR session/capabilities alive while the PC restarts.
    // Each attempt joins uploads and closes sockets before connecting again.
    // The old PC can accept another handshake before the dashboard actually
    // kills SteamVR. Recover that later disconnect too, as client_core does.
    do{
        const auto frames=totals.frames;
        result=attempt(options,callbacks,stop,end,recovery_deadline,totals);
        if(!result.server_stopped&&totals.frames==frames&&recovery_deadline<end&&Clock::now()>=recovery_deadline&&!(stop&&stop->load())&&
                !(totals.startup&&totals.startup->pending())){
            if(result.sanitized_json.find("\"error\":\"\"")!=std::string::npos&&!last_retry_report.empty())result.sanitized_json=last_retry_report;
            exhausted=true;result.exit_code=options.surfxr_unified?1:totals.connected?0:1;break;
        }
        if(result.exit_code==3){
            last_retry_report=result.sanitized_json;
            // Retrying the source cannot repair a failed shared transport.
            // Leave authentication/connection recovery to its outer owner.
            if(options.surfxr_unified){
                try{options.surfxr_unified->check();}
                catch(const Cancelled&){result.exit_code=totals.frames?0:2;break;}
                catch(...){result.exit_code=1;break;}
            }
            // Repeated successful handshakes without video are not recovery.
            // Only actual video progress renews the reconnect budget.
            if(totals.frames!=frames)recovery_deadline=Clock::time_point::max();
            if(recovery_deadline==Clock::time_point::max()){
                const bool restarting=result.sanitized_json.find("surfxr_server_restarting")!=std::string::npos;
                recovery_deadline=Clock::now()+std::chrono::seconds(restarting?45:totals.frames?10:60);
            }
            if(Clock::now()>=recovery_deadline&&!(totals.startup&&totals.startup->pending())){result.exit_code=options.surfxr_unified?1:totals.connected?0:1;exhausted=true;break;}
            ++reconnects;const auto retry_at=std::min(end,Clock::now()+std::chrono::seconds(1));
            while(Clock::now()<retry_at&&!(stop&&stop->load()))std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }while(result.exit_code==3);
    result.reconnect_exhausted=exhausted;
    if(totals.startup)totals.startup->close();
    auto startup=totals.startup?totals.startup->snapshot():SteamVrStartupStatus{};
    if(!result.server_stopped&&(result.exit_code==1||exhausted)&&!(stop&&stop->load())&&startup.state!=SteamVrStartupState::Failed){
        const auto reason=result.sanitized_json.find("surfxr_pc_untrusted")!=std::string::npos?SteamVrStartupReason::Untrusted:SteamVrStartupReason::SessionFailed;
        startup={SteamVrStartupState::Failed,reason,startup.request_id};
        if(callbacks.steamvr_startup)callbacks.steamvr_startup(startup);
    }
    if(!result.sanitized_json.empty())result.sanitized_json.insert(result.sanitized_json.size()-1,
        ",\"steamvr_startup\":{\"state\":"+std::to_string(static_cast<unsigned>(startup.state))+
        ",\"reason\":"+std::to_string(static_cast<unsigned>(startup.reason))+",\"request_id\":"+std::to_string(startup.request_id)+"}");
    if(!result.sanitized_json.empty())result.sanitized_json.insert(result.sanitized_json.size()-1,",\"reconnect_exhausted\":"+std::string(exhausted?"true":"false"));
    if(!result.sanitized_json.empty())result.sanitized_json.insert(result.sanitized_json.size()-1,",\"reconnects\":"+std::to_string(reconnects));
    return result;
}
}
