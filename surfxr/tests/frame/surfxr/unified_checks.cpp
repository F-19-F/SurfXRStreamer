#include "unified.hpp"
#include "surfxr/surfxr.hpp"
#include "surfxr/surfxr_unified.hpp"
#include "surfxr/surfxr_unified_media.hpp"
#include "surfxr/surfxr_desktop.hpp"
#include "surfxr/surfxr_modes.hpp"
#include "unified_queue.hpp"
#include "dashboard.hpp"
#include "surfxr/haptics.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <thread>

surfxr::transport::Bytes client_connection_config(unsigned port,bool lifecycle);
surfxr::transport::Bytes client_unified_connection_config(unsigned media,unsigned priority,bool video_haptics);
namespace surfxr::transport {
namespace {
unsigned port(const Socket& socket){sockaddr_in address{};socklen_t size=sizeof(address);require(getsockname(socket.fd(),reinterpret_cast<sockaddr*>(&address),&size)==0,"unified_test_port");return ntohs(address.sin_port);}
Bytes frame(const Bytes& body){Writer w;w.integer(body.size(),4);w.bytes.insert(w.bytes.end(),body.begin(),body.end());return w.bytes;}
}
void unified_session_case(unsigned hello_flags,bool force_aes){
    {
        UnifiedQueue queue;queue.clear(1);Bytes entry{0,0,0,0,1,2,3,4};const auto* identity=entry.data();
        queue.push(std::move(entry),8,1,"queue_test_budget",4);pollfd event{queue.fd(),POLLIN,0};
        require(::poll(&event,1,0)==1,"unified_test_queue_notification");bool rejected=false;
        try{queue.push(Bytes{9},8,1,"queue_test_budget");}catch(const std::runtime_error&){rejected=true;}
        require(rejected,"unified_test_queue_prefix_unaccounted");
        require(queue.read_datagrams(4,32,1,[&](const auto* data,std::size_t size){
            require(data==identity+4&&size==4&&data[0]==1,"unified_test_queue_copied_owner");
            queue.clear(2); // callback runs outside the lock; old batch stops
            queue.push(Bytes{5},8,2,"queue_test_budget");
        })==1,"unified_test_queue_generation_batch");
        std::uint8_t value=0;require(queue.read(&value,1,false,1)==0&&queue.read(&value,1,false,2)==1&&value==5,"unified_test_queue_stale_consumer");
        require(::poll(&event,1,0)==0,"unified_test_queue_notification_not_drained");
    }
    // Reserve all endpoints before any synthetic handshake. Both peers use
    // private loopback IPs; the test never contacts a Streamer or opens devices.
    Socket listener,video,audio,startup;auto broker=Socket::bind("127.0.0.1",0,true);unsigned base=0;
    for(unsigned i=0;i<64;++i)try{
        auto tcp=Socket::bind("127.0.0.1",0,true);const auto candidate=port(tcp);if(candidate>65533)continue;
        auto v=Socket::bind("127.0.0.1",candidate+1,false),a=Socket::bind("127.0.0.1",candidate+2,false),s=Socket::bind("127.0.0.1",candidate,false);
        listener=std::move(tcp);video=std::move(v);audio=std::move(a);startup=std::move(s);base=candidate;break;
    }catch(const std::runtime_error&){}
    require(base,"unified_test_ports_unavailable");
    desktop::Ticket ticket;ticket.key=desktop::random();ticket.id=desktop::random();ticket.port=base;ticket.unified=true;ticket.media_modes=hello_flags&4;
    std::atomic_bool stop{false},desktop_configured{false};std::atomic_uint audio_packets{0},microphone_starts{0},microphone_stops{0},mic_reads{0},mic_packets{0};
    std::exception_ptr server_error;unsigned begins=0,ends=0,negotiations=0,keyboard_events=0;
    const bool keyboard_enabled=hello_flags==7&&force_aes;
    std::thread server([&]{try{
        auto admission=broker.accept("127.0.0.2",Clock::now()+std::chrono::seconds(5),[] {return false;});
        Bytes query;std::array<std::uint8_t,128> query_buffer{};const auto query_deadline=Clock::now()+std::chrono::seconds(5);
        while(query.size()<10||(query.size()<10u+query[9])){
            const auto count=admission.read(query_buffer.data(),query_buffer.size(),true);query.insert(query.end(),query_buffer.begin(),query_buffer.begin()+count);
            require(Clock::now()<query_deadline,"unified_test_broker_query_timeout");if(!count)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(std::equal(desktop::dashboard_magic.begin(),desktop::dashboard_magic.end(),query.begin())&&query[8]==0&&query[9]>0,"unified_test_broker_ticket_query");
        Bytes reply{0};reply.insert(reply.end(),ticket.key.begin(),ticket.key.end());reply.insert(reply.end(),ticket.id.begin(),ticket.id.end());
        reply.insert(reply.end(),{static_cast<std::uint8_t>(base),static_cast<std::uint8_t>(base>>8),7,60,40});admission.send(reply,true,{});
        admission=broker.accept("127.0.0.2",Clock::now()+std::chrono::seconds(5),[] {return false;});query.clear();
        while(query.size()<9){const auto count=admission.read(query_buffer.data(),9-query.size(),true);query.insert(query.end(),query_buffer.begin(),query_buffer.begin()+count);
            require(Clock::now()<query_deadline,"unified_test_broker_probe_timeout");if(!count)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        require(std::equal(desktop::dashboard_magic.begin(),desktop::dashboard_magic.end(),query.begin())&&query[8]==9,"unified_test_broker_media_mode_probe");
        admission.send(Bytes{static_cast<std::uint8_t>(ticket.media_modes?1:0)},true,{});
        auto socket=listener.accept("127.0.0.2",Clock::now()+std::chrono::seconds(5),[] {return false;});Framer framer(2*1024*1024,true);
        const auto receive=[&]{return receive_control(socket,framer,Clock::now()+std::chrono::seconds(5),[] {return false;});};
        const auto claim=receive();require(claim.size()==(ticket.media_modes?41u:40u)&&std::equal(ticket.id.begin(),ticket.id.end(),claim.begin()+8)&&(!ticket.media_modes||claim.back()==1),"unified_test_ticket");
        const auto nonce=desktop::random();Bytes challenge(desktop::magic.begin(),desktop::magic.end());challenge.insert(challenge.end(),nonce.begin(),nonce.end());socket.send(frame(challenge),true,{});
        const auto auth=receive();require(auth.size()==64,"unified_test_auth_size");desktop::Key client{},mac{};std::copy_n(auth.data(),32,client.begin());std::copy_n(auth.data()+32,32,mac.begin());
        require(desktop::equal(mac,desktop::proof(ticket.key,nonce,client,"ALVR-DESKTOP-client-proof-v1")),"unified_test_auth");
        const auto proof=desktop::proof(ticket.key,nonce,client,"ALVR-DESKTOP-server-proof-v1");
        desktop::Cipher outgoing(desktop::proof(ticket.key,nonce,client,"ALVR-DESKTOP-control-s2c-v1")),incoming(desktop::proof(ticket.key,nonce,client,"ALVR-DESKTOP-control-c2s-v1"));
        unified::MediaCipher media_cipher(desktop::proof(ticket.key,nonce,client,"ALVR-UNIFIED-video-s2c-v1")),audio_cipher(desktop::proof(ticket.key,nonce,client,"ALVR-UNIFIED-audio-s2c-v1")),mic_cipher(desktop::proof(ticket.key,nonce,client,"ALVR-UNIFIED-priority-c2s-v1"));
        Writer hello;hello.integer(base+1,2);hello.integer(base+2,2);hello.integer(hello_flags,1);
        // Coalesce authentication, hello and a keepalive into one TCP write.
        // The production reader must preserve complete and partial records.
        auto greeting=frame(Bytes(proof.begin(),proof.end()));
        for(const auto& body:{unified::record(unified::Kind::Hello,0,hello.bytes),unified::record(unified::Kind::KeepAlive,0)}){auto part=frame(outgoing.seal(body));greeting.insert(greeting.end(),part.begin(),part.end());}
        socket.send(greeting,true,{});video.connect_udp("127.0.0.2",base+1);audio.connect_udp("127.0.0.2",base+2);
        const auto send=[&](unified::Kind kind,unsigned generation,const Bytes& body={}){socket.send(frame(outgoing.seal(unified::record(kind,generation,body))),true,{});};
        const auto next=[&]{for(;;){auto record=unified::parse(incoming.open(receive()));if(record.kind==unified::Kind::Keyboard){require(keyboard_enabled&&record.generation==13&&record.body==Bytes({0,0}),"unified_test_keyboard_state");continue;}if(record.kind!=unified::Kind::KeepAlive)return record;}};
        auto setup=next();require(setup.kind==unified::Kind::Hello&&setup.generation==0,"unified_test_one_negotiation");Reader capabilities(setup.body);require(!capabilities.blob(65536).empty()&&!capabilities.blob(65536).empty(),"unified_test_capabilities");
        if(hello_flags&4){require(capabilities.integer(1)==1,"unified_test_media_version");const auto mode=static_cast<unified::Protection>(capabilities.integer(1));
            require(mode==((force_aes||(hello_flags&8))?unified::Protection::AesGcm:unified::Protection::Plain),"unified_test_media_selection");
            media_cipher.protection(mode);audio_cipher.protection(mode);mic_cipher.protection(mode);
        }
        capabilities.end();++negotiations;
        if(keyboard_enabled)send(unified::Kind::Keyboard,0,Bytes{2,1});
        const auto media_record=[&](unsigned generation,const Bytes& shard){Bytes record;media_cipher.seal(generation,shard.data(),shard.size(),record);return record;};
        Reassembler microphone(1200,true,192000,1u<<2);
        for(unsigned generation=1;generation<=12;++generation){
            const auto begin=next();require(begin.kind==unified::Kind::Begin&&begin.generation==generation&&begin.body==Bytes({static_cast<std::uint8_t>(generation%2?0:1)}),"unified_test_begin_without_handshake");++begins;
            if(keyboard_enabled){
                const std::uint8_t pixel[]{10,20,30,255};
                auto image=keyboard_image::identify(keyboard_image::encode(1,1,pixel),generation);
                image.insert(image.begin(),1);
                if(generation>1)send(unified::Kind::Keyboard,generation-1,image);
                if(generation%2)send(unified::Kind::Keyboard,generation,image);
            }
            if(keyboard_enabled&&generation<=2){
                // Stall the actual authenticated peer beyond both former
                // deadlines: first mid-prefix, then without a pending frame.
                auto bytes=frame(outgoing.seal(unified::record(unified::Kind::Control,generation,control(3,true))));
                const auto prefix=generation==1?2u:0u;
                if(prefix)socket.send(Bytes(bytes.begin(),bytes.begin()+prefix),true,{});
                std::this_thread::sleep_for(std::chrono::seconds(4));
                socket.send(Bytes(bytes.begin()+prefix,bytes.end()),true,{});
            }else send(unified::Kind::Control,generation,control(3,true));
            const auto request=next();require(request.kind==unified::Kind::Control&&request.generation==generation&&request.body==control(1,true),"unified_test_control_adapter");
            const Bytes picture(48*1024,static_cast<std::uint8_t>(generation));
            const auto parts=shards(3,generation,picture,1200,true);const auto& shard=parts.front();
            if(generation>1)video.send(media_record(generation-1,shard),false,{});
            auto record=media_record(generation,shard);auto corrupt=record;corrupt[media_cipher.protection()==unified::Protection::Plain?0:corrupt.size()-1]^=1;
            video.send(corrupt,false,{});video.send(record,false,{});video.send(record,false,{});
            for(unsigned i=1;i<parts.size();++i)video.send(media_record(generation,parts[i]),false,{});
            const auto pcm=shards(2,generation,Bytes{1,0,2,0},1200,true).front();Bytes audio_record;
            audio_cipher.seal(0,pcm.data(),pcm.size(),audio_record);audio.send(audio_record,false,{});
            std::array<std::uint8_t,4096> bytes{};for(unsigned i=0;i<8;++i){const auto count=audio.read(bytes.data(),bytes.size(),false);if(!count)break;
                Bytes scratch;const auto plain=mic_cipher.open(bytes.data(),count,scratch);Reader r(plain.data,plain.size);require(r.integer(4)==0,"unified_test_mic_epoch");if(auto packet=microphone.accept(r.data(),r.remaining(),now_ns())){require(packet->bytes.size()==1920,"unified_test_mic_pcm");++mic_packets;}}
            const auto end=next();require(end.kind==unified::Kind::End&&end.generation==generation,"unified_test_end");++ends;
        }
        const auto busy=next();require(busy.kind==unified::Kind::Begin&&busy.generation==13,"unified_test_busy_source_begin");++begins;
        const bool video_haptics=hello_flags&4;
        send(unified::Kind::Control,13,client_unified_connection_config(base+1,base+2,video_haptics));send(unified::Kind::Control,13,control(0,true));
        const auto ready=next();require(ready.kind==unified::Kind::Control&&ready.body==control(3,true),"unified_test_busy_source_ready");
        if(!video_haptics){Writer pulse(true);pulse.integer(path_id("/user/hand/left"),8);pulse.duration(250'000'000);pulse.real(100);pulse.real(.4f);
            const auto shard=shards(1,0,pulse.bytes,1200,true).front();Bytes record;audio_cipher.seal(13,shard.data(),shard.size(),record);audio.send(record,false,{});}
        // Exercise the real VR session beyond its legacy two-second deadline.
        // Outer liveness continues while the source sends no keepalive/video.
        const auto paused=Clock::now()+std::chrono::milliseconds(3500);
        while(Clock::now()<paused){send(unified::Kind::KeepAlive,0);std::this_thread::sleep_for(std::chrono::milliseconds(100));}
        if(hello_flags==7&&force_aes){send(unified::Kind::Control,13,desktop::reserved(5,"frame_session_stopped"));send(unified::Kind::Ended,13,Bytes{0});}
        auto ended=next();while(ended.kind==unified::Kind::Control){if(const auto key=desktop::keyboard_input_event(ended.body)){require(keyboard_enabled,"unified_test_no_unsolicited_keyboard");++keyboard_events;}require(ended.generation==13,"unified_test_busy_source_control_generation");ended=next();}
        require(!keyboard_enabled||keyboard_events==4,"unified_test_vr_consumes_desktop_events");
        require(ended.kind==unified::Kind::End&&ended.generation==13,"unified_test_busy_source_did_not_retire_early");++ends;
        if(hello_flags==7&&!force_aes){
            const auto consume=[&](){for(;;){auto record=next();
                if(record.kind==unified::Kind::TrackingBegin){send(unified::Kind::Tracking,record.generation,desktop::reserved(5,"frame_session_stopped"));continue;}
                if(record.kind!=unified::Kind::Tracking)return record;}};
            const auto vr=consume();require(vr.kind==unified::Kind::Begin&&vr.generation==14&&vr.body==Bytes{1},"unified_test_failed_vr_begin");++begins;
            // Unexpected source closure is a source failure, not a reliable
            // transport failure or a fabricated user/PC shutdown request.
            send(unified::Kind::Ended,14,Bytes{0});
            const auto vr_end=consume();require(vr_end.kind==unified::Kind::End&&vr_end.generation==14,"unified_test_failed_vr_retired");++ends;
            const auto desktop_begin=consume();require(desktop_begin.kind==unified::Kind::Begin&&desktop_begin.generation==15&&desktop_begin.body==Bytes{0},"unified_test_failed_vr_returns_desktop");++begins;
            send(unified::Kind::Control,15,desktop::configuration({0,0,32,32,32,32},60,base+1,false));send(unified::Kind::Control,15,control(0,true));
            const auto desktop_ready=consume();require(desktop_ready.kind==unified::Kind::Control&&desktop_ready.generation==15&&desktop_ready.body==control(3,true),"unified_test_fallback_stream_ready");
            Writer decoder(true);decoder.integer(1,4);decoder.integer(1,4);decoder.blob({0,0,0,1,0x40});decoder.string("");send(unified::Kind::Control,15,decoder.bytes);
            // TCP configuration and UDP video have independent delivery order.
            // Wait for the real consumer before emitting this fixture's sole IDR.
            const auto configured_deadline=Clock::now()+std::chrono::seconds(2);
            while(!desktop_configured.load()&&Clock::now()<configured_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(desktop_configured.load(),"unified_test_fallback_decoder_not_configured");
            const Bytes nal{0,0,0,1,0x26,1};const auto picture=desktop::video(7,true,nal.data(),nal.size());
            for(const auto& shard:shards(3,0,picture,1200,true))video.send(media_record(15,shard),false,{});
            auto desktop_end=consume();while(desktop_end.kind==unified::Kind::Control)desktop_end=consume();
            require(desktop_end.kind==unified::Kind::End&&desktop_end.generation==15,"unified_test_fallback_user_stop");++ends;
            while(!stop.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return;
        }
        // The physical connection remains authenticated while SteamVR is idle.
        send(unified::Kind::KeepAlive,0);
        // A slow/absent optional tracking consumer must retain the exact local
        // failure reason instead of hiding it behind a generic transport error.
        send(unified::Kind::Tracking,13,Bytes(4097));
        while(!stop.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }catch(...){server_error=std::current_exception();}});
    struct Join {std::atomic_bool& stop;std::thread& thread;~Join(){stop.store(true);if(thread.joinable())thread.join();}} join{stop,server};
    SessionOptions options;options.host="127.0.0.1";options.local_address="127.0.0.2";options.surfxr_version="21.0.0-dev14";options.surfxr_modes=std::make_shared<Modes>(true);options.microphone_enabled=true;options.surfxr_media_aes=force_aes;
    options.surfxr_dashboard_port=port(broker);
    options.keyboard_overlay=std::make_shared<KeyboardOverlay>();
    options.haptics_queue=std::make_shared<HapticsQueue>();
    SessionCallbacks callbacks;callbacks.audio_pcm=[&](const auto* data,auto size){require(size==4&&data[0]==1&&data[2]==2,"unified_test_audio_pcm");++audio_packets;};
    callbacks.microphone_start=[&]{++microphone_starts;return true;};callbacks.microphone_stop=[&]{++microphone_stops;};
    callbacks.microphone_pcm=[&](auto* data,auto size){if(mic_reads.fetch_add(1)%4)return std::size_t{0};std::memset(data,0,size);return size;};
    {
        const auto negotiated_ticket=request_desktop(options,[] {return false;});
        require(negotiated_ticket.unified&&negotiated_ticket.media_modes==ticket.media_modes&&negotiated_ticket.port==ticket.port,"unified_test_production_broker_modes");
        auto shared_transport=std::make_shared<UnifiedTransport>(options,callbacks,negotiated_ticket,&stop);auto& transport=*shared_transport;int physical_video=-1,physical_audio=-1;Socket stale_media;
        for(unsigned generation=1;generation<=12;++generation){
            Connection connection;connection.modern=true;connection.control_socket=transport.begin(generation%2!=0,{});auto media=transport.video(),priority=transport.priority();
            if(generation==1){physical_video=media.fd();physical_audio=priority.fd();}else require(media.fd()==physical_video&&priority.fd()==physical_audio,"unified_test_same_sockets");
            pollfd notification{connection.control_socket.fd(),POLLIN,0};require(::poll(&notification,1,5000)==1&&connection.control_socket.readable(),"unified_test_virtual_readable");
            Framer framer(4096,true);require(server_control(receive_control(connection.control_socket,framer,Clock::now()+std::chrono::seconds(5),[] {return false;}),true).type==3,"unified_test_control_receive");connection.send_control(control(1,true));
            if(keyboard_enabled){
                require(transport.desktop_keyboard(),"unified_test_desktop_keyboard_negotiated");
                const auto image=options.keyboard_overlay->snapshot();
                if(generation%2)require(image&&image->token==generation&&image->rgba[0]==30,"unified_test_desktop_candidate_image");
                else require(!image,"unified_test_candidate_cleared_and_stale_generation_ignored");
            }
            Reassembler video_frames(1200,true);const auto deadline=Clock::now()+std::chrono::seconds(5);bool received=false;
            Bytes scratch(32*1200,0xa5);
            if(generation>1)require(stale_media.read_datagrams(scratch.data(),1200,1,[](const auto*,auto){fail("unified_test_stale_adapter_published");})==0,"unified_test_stale_adapter_drained_new_video");
            while(Clock::now()<deadline&&(!received||audio_packets.load()<generation)){transport.check();
                media.read_datagrams(scratch.data(),1200,32,[&](const auto* bytes,std::size_t size){
                    auto packet=video_frames.accept(bytes,size,now_ns());if(packet){require(!received&&packet->bytes==Bytes(48*1024,static_cast<std::uint8_t>(generation)),"unified_test_no_stale_or_replay");received=true;}
                });
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            require(std::all_of(scratch.begin(),scratch.end(),[](auto value){return value==0xa5;}),"unified_test_datagrams_copied_to_scratch");
            require(received&&audio_packets.load()==generation,"unified_test_media_progress");stale_media=std::move(media);transport.end();
        }
        auto source_options=options;source_options.surfxr_modes.reset();source_options.surfxr_unified=shared_transport;source_options.surfxr_control_port=base;
        std::atomic_bool source_stop{false};std::atomic_uint source_started{0},source_ended{0},startup_failed{0};SessionResult source_result;
        std::atomic_uint keyboard_index{0};
        auto source_callbacks=callbacks;
        source_callbacks.desktop_input=[&](desktop::Event& event){const auto i=keyboard_index.fetch_add(1);if(i>=4)return false;event=i<2?desktop::Event{desktop::EventKind::Key,0,0,0,0x1e,i==0?1:0}:i==2?desktop::Event{}:desktop::Event{desktop::EventKind::Button,0,.3f,.6f,0,1,42};return true;};source_callbacks.connection_started=[&]{++source_started;};source_callbacks.connection_ended=[&]{++source_ended;};
        source_callbacks.steamvr_startup=[&](const auto& status){if(status.state==SteamVrStartupState::Failed)++startup_failed;};
        std::thread source([&]{source_result=run(source_options,source_callbacks,&source_stop);});
        struct SourceJoin {std::atomic_bool& stop;std::thread& worker;~SourceJoin(){stop.store(true);if(worker.joinable())worker.join();}} source_join{source_stop,source};
        const auto source_deadline=Clock::now()+std::chrono::seconds(5);
        while(!source_started.load()&&Clock::now()<source_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(source_started==1,"unified_test_source_started");
        if(!(hello_flags&4)){bool pulse=false;const auto deadline=Clock::now()+std::chrono::milliseconds(200);
            while(!pulse&&Clock::now()<deadline){const auto value=options.haptics_queue->consume(now_ns(),true)[0];pulse=value.changed&&value.amplitude==.4f;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            require(pulse,"unified_test_legacy_priority_pulse_preserved");}
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));
        if(hello_flags==7&&force_aes){const auto deadline=Clock::now()+std::chrono::seconds(2);
            while(!source_ended.load()&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(source_ended==1,"unified_test_explicit_pc_stop_arrives");
        }else if(source_ended.load()){source_stop.store(true);source.join();throw std::runtime_error("unified_test_busy_source_misclassified_as_disconnected: "+source_result.sanitized_json);}
        source_stop.store(true);source.join();
        require(source_result.exit_code!=1&&source_ended==1,"unified_test_busy_source_cancel");transport.end();
        if(hello_flags==7&&force_aes)require(source_result.server_stopped&&startup_failed==0,"unified_test_explicit_pc_stop_is_not_startup_failure");
        require(source_result.sanitized_json.find(hello_flags&4?"\"priority_receiver\":false":"\"priority_receiver\":true")!=std::string::npos,"unified_test_no_empty_priority_owner");
        if(hello_flags==7&&!force_aes){
            options.surfxr_modes->request(false);auto mode_options=options;mode_options.surfxr_unified=shared_transport;mode_options.surfxr_control_port=base;
            std::atomic_bool mode_stop{false};unsigned desktop_frames=0,mode_failed=0;auto mode_callbacks=callbacks;
            mode_callbacks.desktop_config=[&](const auto&){desktop_configured.store(true);};
            mode_callbacks.steamvr_startup=[&](const auto& status){if(status.state==SteamVrStartupState::Failed){require(status.reason==SteamVrStartupReason::SessionFailed,"unified_test_failed_startup_has_fixed_reason");++mode_failed;}};
            mode_callbacks.video_packet=[&](unsigned id,const auto* data,std::size_t count,std::uint64_t stamp){
                require(id==0&&count==11&&data[4]==0x40&&stamp==7,"unified_test_fallback_real_desktop_access_unit");++desktop_frames;mode_stop.store(true);};
            const auto mode_result=run_modes(mode_options,mode_callbacks,&mode_stop);
            require(mode_result.exit_code==0&&desktop_frames==1&&mode_failed==1&&mode_result.sanitized_json.find("\"unified_vr_fallbacks\":1")!=std::string::npos&&
                mode_result.sanitized_json.find("surfxr_unified_backend_failed")!=std::string::npos,("unified_test_source_failure_keeps_desktop_owner frames="+std::to_string(desktop_frames)+" failures="+std::to_string(mode_failed)+" "+mode_result.sanitized_json).c_str());
        }else{
        bool exact_failure=false;const auto deadline=Clock::now()+std::chrono::seconds(2);
        while(!exact_failure&&Clock::now()<deadline){
            try{transport.check();}catch(const std::runtime_error& error){exact_failure=std::strcmp(error.what(),"surfxr_unified_tracking_queue_full")==0;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(exact_failure,"unified_test_failure_reason_lost");
        }
    }
    stop.store(true);server.join();if(server_error)std::rethrow_exception(server_error);
    const auto generations=hello_flags==7&&!force_aes?15u:13u;
    require(begins==generations&&ends==generations&&negotiations==1&&microphone_starts==1&&microphone_stops==1&&mic_packets>0,"unified_test_persistent_ownership");
}
void unified_session_checks(){unified_session_case(3,false);unified_session_case(7,false);unified_session_case(7,true);}
}
