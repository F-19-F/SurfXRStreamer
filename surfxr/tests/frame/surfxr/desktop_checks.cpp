#include "surfxr/surfxr.hpp"
#include "surfxr/surfxr_desktop_layout.hpp"
#include "surfxr/surfxr_desktop_crypto.hpp"
#include "surfxr/surfxr_dashboard.hpp"
#include "socket.hpp"
#include "../../shared/protocol/desktop_capture_checks.hpp"
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace surfxr::transport {
namespace {
unsigned port(const Socket& s){sockaddr_in address{};socklen_t size=sizeof(address);require(getsockname(s.fd(),reinterpret_cast<sockaddr*>(&address),&size)==0,"desktop_test_port");return ntohs(address.sin_port);}
void framed(Socket& socket,const Bytes& body){Bytes out;for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(body.size()>>(8*i)));out.insert(out.end(),body.begin(),body.end());socket.send(out,true,[] {return false;});}
void crypto_reuse_checks(){
    desktop::Key key{};desktop::Cipher sender(key),receiver(key),duplex(key);Bytes record,plain;
    record.reserve(8192);plain.reserve(8192);const auto* record_buffer=record.data();const auto* plain_buffer=plain.data();
    const auto rejects=[&](const Bytes& wire,bool ordered=false){
        bool rejected=false;try{receiver.open_into(wire,plain,ordered);}catch(const std::runtime_error&){rejected=true;}
        require(rejected&&plain.empty()&&plain.data()==plain_buffer,"desktop_failed_auth_clears_reusable_output");
    };
    for(const auto count:{0u,1u,16u,31u,1400u,4096u}){
        const Bytes expected(count,static_cast<std::uint8_t>(count));sender.seal_into(expected.data(),expected.size(),record);
        auto corrupt=record;corrupt.at(corrupt.size()-1)^=1;rejects(corrupt,true);receiver.open_into(record.data(),record.size(),plain);
        require(plain==expected&&record.data()==record_buffer&&plain.data()==plain_buffer,"desktop_cipher_reuses_output_capacity");
        const auto reverse=duplex.seal(expected);require(duplex.open(reverse)==expected,"desktop_cipher_direction_contexts_independent");
    }
    desktop::Cipher udp_sender(key),udp_receiver(key);std::array<Bytes,4> saved;
    for(unsigned seq=1;seq<=8200;++seq){
        udp_sender.seal_into(Bytes{static_cast<std::uint8_t>(seq)},record);
        if(seq==1)saved[0]=record;
        if(seq==4105)saved[1]=record;
        if(seq==4104)saved[2]=record;
        if(seq==8200)saved[3]=record;
    }
    require(udp_receiver.open(saved[0],false)==Bytes{1},"desktop_replay_initial");
    auto forged=saved[3];forged.at(forged.size()-1)^=1;bool rejected=false;
    try{udp_receiver.open_into(forged,plain,false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected&&plain.empty(),"desktop_replay_future_bad_auth_not_committed");
    rejected=false;try{udp_receiver.open(saved[0],false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"desktop_replay_future_bad_auth_keeps_duplicate");
    require(udp_receiver.open(saved[3],false)==Bytes{8}&&udp_receiver.open(saved[1],false)==Bytes{9},"desktop_replay_window_4095_boundary");
    rejected=false;try{udp_receiver.open(saved[2],false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"desktop_replay_window_4096_expired");
    rejected=false;try{udp_receiver.open(saved[1],false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"desktop_replay_out_of_order_duplicate");
    desktop::ReplayWindow window;
    for(std::uint64_t seq=1;seq<=8195;seq+=2){require(window.accepts(seq),"desktop_replay_circular_progress");window.commit(seq);}
    require(window.accepts(4100)&&!window.accepts(4099)&&!window.accepts(8195)&&window.accepts(8196,true),"desktop_replay_circular_holes_seen_and_expired");
    window.commit(4100);require(!window.accepts(4100),"desktop_replay_circular_hole_committed");
    window.commit(UINT64_MAX);require(!window.accepts(1)&&!window.accepts(UINT64_MAX)&&!window.accepts(0,true),"desktop_replay_no_sequence_wrap");
}
}
void desktop_session_checks(bool multi,bool managed=false){
    desktop::Key zero{};desktop::Cipher reference(zero);const auto bytes=reference.seal(Bytes{1,2,3,4});constexpr char expected[]="0100000000000000e7d6fc0f589e3336167c6728da9239267bb3ba58";constexpr char digits[]="0123456789abcdef";std::string encoded;for(auto b:bytes){encoded+=digits[b>>4];encoded+=digits[b&15];}require(encoded==expected,"desktop_cipher_reference");
    desktop::Cipher reader(zero);auto tampered=bytes;tampered.at(tampered.size()-1)^=1;bool rejected=false;
    try{reader.open(tampered.data(),tampered.size(),false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected&&reader.open(bytes.data(),bytes.size(),false)==Bytes({1,2,3,4}),"desktop_cipher_borrowed_authentication");
    rejected=false;try{reader.open(bytes.data(),bytes.size(),false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"desktop_cipher_borrowed_replay");
    auto listener=Socket::bind("127.0.0.1",0,true),media=Socket::bind("127.0.0.1",0,false);
    desktop::Key vector_key{},ones{};ones.fill(1);for(unsigned i=0;i<32;++i)vector_key[i]=static_cast<std::uint8_t>(i);
    const auto known=desktop::proof(vector_key,zero,ones,"ALVR-DESKTOP-client-proof-v1");encoded.clear();for(auto b:known){encoded+=digits[b>>4];encoded+=digits[b&15];}require(encoded=="bbbeea7b1e7244bba9c17a4c71ba9d831b035fc269335216e32f824a6ca2d042","desktop_hmac_reference");
    auto root=desktop::random();char path[]="/tmp/surfxr-desktop-test-XXXXXX";const int fd=mkstemp(path);require(fd>=0,"desktop_test_key_file");
    constexpr char hex[]="0123456789abcdef";std::string key;for(auto b:root){key+=hex[b>>4];key+=hex[b&15];}key+='\n';require(write(fd,key.data(),key.size())==static_cast<ssize_t>(key.size()),"desktop_test_key_write");close(fd);
    struct Remove {const char* path;~Remove(){unlink(path);}} remove{path};
    SessionOptions o;o.backend=SessionBackend::Surfxr;o.surfxr_desktop=true;o.surfxr_version="21.0.0-dev14";o.host="127.0.0.1";o.local_address="127.0.0.2";o.surfxr_desktop_port=port(listener);o.surfxr_desktop_key_file=path;o.seconds=8;auto commands=std::make_shared<SessionControlQueue>();o.control_queue=commands;
    Socket broker;std::thread admission;std::exception_ptr broker_error;const auto ticket=desktop::random();
    if(managed){
        broker=Socket::bind("127.0.0.1",0,true);o.surfxr_dashboard_desktop=true;o.surfxr_dashboard_port=port(broker);o.surfxr_desktop_key_file="/missing/unused-legacy-key";
        admission=std::thread([&]{try{
            auto socket=broker.accept("127.0.0.2",Clock::now()+std::chrono::seconds(5),[] {return false;});
            Bytes query;std::array<std::uint8_t,128> buffer{};
            const auto deadline=Clock::now()+std::chrono::seconds(5);
            while(query.size()<10||(query.size()<10u+query[9])){const auto n=socket.read(buffer.data(),buffer.size(),true);query.insert(query.end(),buffer.begin(),buffer.begin()+n);require(Clock::now()<deadline,"desktop_broker_test_read");if(!n)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            require(std::equal(desktop::dashboard_magic.begin(),desktop::dashboard_magic.end(),query.begin())&&query[8]==0&&query[9]>0,"desktop_broker_test_query");
            Bytes reply{0};reply.insert(reply.end(),root.begin(),root.end());reply.insert(reply.end(),ticket.begin(),ticket.end());const auto p=port(listener);reply.insert(reply.end(),{static_cast<std::uint8_t>(p),static_cast<std::uint8_t>(p>>8),3,60,40});socket.send(reply,true,[] {return false;});
        }catch(...){broker_error=std::current_exception();}});
    }
    std::exception_ptr error;unsigned inputs=0,capture_requests=0;std::atomic_uint frames{0};std::thread server([&]{try{
        auto socket=listener.accept("127.0.0.2",Clock::now()+std::chrono::seconds(5),[] {return false;});Framer framer(2*1024*1024,true);
        const auto receive=[&]{return receive_control(socket,framer,Clock::now()+std::chrono::seconds(5),[] {return false;});};
        if(managed){const auto claim=receive();require(claim.size()==40&&std::equal(desktop::ticket_magic.begin(),desktop::ticket_magic.end(),claim.begin())&&std::equal(ticket.begin(),ticket.end(),claim.begin()+8),"desktop_broker_test_claim");}
        auto challenge=desktop::random();Bytes hello(desktop::magic.begin(),desktop::magic.end());hello.insert(hello.end(),challenge.begin(),challenge.end());framed(socket,hello);
        auto response=receive();require(response.size()==64,"desktop_test_proof");desktop::Key client{},proof{};std::copy_n(response.data(),32,client.begin());std::copy_n(response.data()+32,32,proof.begin());require(desktop::equal(proof,desktop::proof(root,challenge,client,"ALVR-DESKTOP-client-proof-v1")),"desktop_test_auth");
        auto ack=desktop::proof(root,challenge,client,"ALVR-DESKTOP-server-proof-v1");framed(socket,Bytes(ack.begin(),ack.end()));
        desktop::Cipher incoming(desktop::proof(root,challenge,client,"ALVR-DESKTOP-control-c2s-v1")),outgoing(desktop::proof(root,challenge,client,"ALVR-DESKTOP-control-s2c-v1")),video_cipher(desktop::proof(root,challenge,client,"ALVR-DESKTOP-media-s2c-v1"));
        const auto caps=incoming.open(receive());require(std::search(caps.begin(),caps.end(),reinterpret_cast<const std::uint8_t*>("\"frame_desktop\":1"),reinterpret_cast<const std::uint8_t*>("\"frame_desktop\":1")+17)!=caps.end(),"desktop_test_capability");
        const auto send=[&](const Bytes& b){framed(socket,outgoing.seal(b));};
        const auto receive_client=[&]{for(;;){auto b=incoming.open(receive());Reader type(b,true);
            if(type.integer(4)!=desktop::capture_type)return b;
            const auto value=desktop::capture_record(b);require(multi&&value.mask==0,"desktop_test_capture_zero");
            ++capture_requests;send(desktop::capture_record(value,true));
        }};
        send(desktop::configuration({-32,0,64,64,64,64},60,port(media),false,multi));send(desktop::control(0));
        {auto b=receive_client();Reader r(b,true);require(r.integer(4)==3,"desktop_test_ready");r.end();}
        media.connect_udp("127.0.0.2",port(media));
        desktop::Layout layout;layout.capacity=2;layout.streams={{{-32,0,64,64,64,64},2,{0,0,0,1,0x40}}};
        if(multi)send(desktop::layout(layout));
        else {Writer decoder(true);decoder.integer(1,4);decoder.integer(1,4);decoder.blob(Bytes{0,0,0,1,0x40});decoder.string("");send(decoder.bytes);}
        std::array<unsigned,4> indices{};const Bytes nal{0,0,0,1,0x26,1};
        const auto picture=[&](unsigned id,unsigned generation,std::uint64_t stamp){
            const auto payload=multi?desktop::video(generation,stamp,true,nal.data(),nal.size()):desktop::video(stamp,true,nal.data(),nal.size());
            for(const auto& shard:shards(desktop::video_streams[id],indices[id]++,payload,desktop::packet_size,true))media.send(video_cipher.seal(shard),false,[] {return false;});
        };
        picture(0,1,123000000);
        const auto wait_frames=[&](unsigned count){const auto deadline=Clock::now()+std::chrono::seconds(5);while(frames.load()<count&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));require(frames.load()==count,"desktop_test_video_before_stop");};
        const auto receive_monitor=[&](bool add){for(;;){auto b=receive_client();Reader r(b,true);const auto type=r.integer(4);if(type==desktop::monitor_type){require(r.boolean()==add,"desktop_test_monitor_command");r.end();break;}require(type==1||type==2,"desktop_test_control");r.end();}};
        while(inputs<3){auto b=receive_client();Reader r(b,true);const auto type=r.integer(4);
            if(type==(multi?desktop::input_type:11u)){
                if(multi)require(r.integer(4)==1,"desktop_test_input_generation");
                auto e=desktop::event(r.blob(64));r.end();
                if(e.kind==desktop::EventKind::Move){require(inputs==0&&e.u==.25f,"desktop_test_move");++inputs;}
                else if(e.kind==desktop::EventKind::Button){require(inputs==1||inputs==2,"desktop_test_edge_order");require(e.b==(inputs==1?1:0),"desktop_test_edge_value");++inputs;}
            }else{require(type==1||type==2,"desktop_test_control");r.end();}
        }
        if(multi){
            wait_frames(1);receive_monitor(true);layout.generation=2;layout.streams.push_back({{-128,64,32,64,32,64},2,{0,0,0,1,0x40}});send(desktop::layout(layout));
            picture(0,2,123000001);picture(1,2,123000002);wait_frames(3);
            receive_monitor(false);layout.generation=3;layout.streams.resize(1);send(desktop::layout(layout));
            picture(1,2,999);picture(0,2,999);picture(0,3,123000003);wait_frames(4);
        }
        // TCP input completion does not prove that the independent UDP video
        // callback ran. Keep the synthetic server alive until its picture did.
        const auto video_deadline=Clock::now()+std::chrono::seconds(5);
        while(frames.load()<(multi?4u:1u)&&Clock::now()<video_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(frames.load()==(multi?4u:1u),"desktop_test_video_before_stop");
        send(desktop::reserved(5,"frame_session_stopped"));
    }catch(...){error=std::current_exception();}});
    unsigned next=0,layouts=0,ended=0,capture_acks=0;SessionCallbacks cb;
    if(multi)cb.desktop_capture_mask=[] {return 0u;};
    cb.desktop_capture_ack=[&](bool supported,unsigned mask){if(supported&&mask==0)++capture_acks;};
    cb.desktop_input=[&](desktop::Event& e){if(next==3||(multi&&layouts==0))return false;e=next==0?desktop::Event{desktop::EventKind::Move,0,.25f,.5f,0,0}:desktop::Event{desktop::EventKind::Button,0,.25f,.5f,0,next==1?1:0};++next;return true;};
    cb.desktop_config=[&](const DesktopConfiguration& c){require((multi||c.streams.size()==1)&&c.streams[0].width==64&&c.streams[0].desktop_x==-32,"desktop_test_layout");++layouts;if(multi){require(c.monitor_capacity==2,"desktop_test_monitor_capacity");if(layouts==1)commands->add_monitor();else if(layouts==2)require(c.streams.size()==2&&c.streams[1].desktop_x==-128,"desktop_test_secondary_geometry");else require(layouts==3&&c.streams.size()==1,"desktop_test_remove_layout");}};
    cb.video_packet=[&](unsigned id,const auto* data,std::size_t size,std::uint64_t timestamp){require((id==0||(multi&&id==1))&&size==11&&data[4]==0x40&&timestamp>=123000000&&timestamp<=123000003,"desktop_test_video_callback");const auto count=++frames;if(multi&&count==3)commands->remove_monitor();};cb.connection_ended=[&]{++ended;};
    const auto result=run(o,cb,nullptr);server.join();if(admission.joinable())admission.join();if(broker_error)std::rethrow_exception(broker_error);if(error)std::rethrow_exception(error);
    require(result.exit_code==0&&result.connection_started&&result.server_stopped&&layouts==(multi?3u:1u)&&frames==(multi?4u:1u)&&ended==1&&inputs==3,"desktop_test_session");
    require(!multi||(capture_requests>0&&capture_acks>0),"desktop_capture_not_transmitted");
    auto keyfile=desktop::read_key(path);require(desktop::equal(keyfile,root),"desktop_test_key_read");chmod(path,0644);rejected=false;try{desktop::read_key(path);}catch(...){rejected=true;}require(rejected,"desktop_test_public_key_file_accepted");
}
void unified_media_crypto_checks();
void desktop_connection_checks(){desktop_capture_checks();crypto_reuse_checks();unified_media_crypto_checks();desktop_session_checks(false);desktop_session_checks(true);desktop_session_checks(true,true);}
}
