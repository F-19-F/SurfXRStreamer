#include "socket.hpp"
#include "surfxr/surfxr.hpp"
#include "session_deadline.hpp"
#include "desktop_tracking.hpp"
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <poll.h>

using namespace surfxr::transport;
Bytes client_connection_config(unsigned port,bool lifecycle);
namespace {
unsigned socket_port(const Socket& socket){sockaddr_in address{};socklen_t n=sizeof(address);
    require(!getsockname(socket.fd(),reinterpret_cast<sockaddr*>(&address),&n),"test_reserved_port");return ntohs(address.sin_port);}
struct Join {std::atomic_bool& stop;std::thread& thread;~Join(){stop=true;if(thread.joinable())thread.join();}};
void desktop_tracking_checks(){
    auto listener=Socket::bind("127.0.0.1",0,true);
    surfxr::SessionOptions options;options.host="127.0.0.1";options.local_address="127.0.0.2";
    options.surfxr_version="21.0.0-dev14";options.surfxr_control_port=socket_port(listener);
    options.surfxr_view_width=options.surfxr_view_height=32;options.surfxr_refresh_rates={90};
    std::atomic_bool stop{false};unsigned media_callbacks=0;
    surfxr::SessionCallbacks callbacks;
    auto boundary=std::make_shared<surfxr::StageBoundary>();boundary->origin_epoch=1;boundary->width=2;boundary->depth=3;
    boundary->quads.assign(3,{1,0,2,1,2,2,3,2,4,3,0,4});
    auto next_boundary=std::make_shared<surfxr::StageBoundary>(*boundary);next_boundary->origin_epoch=2;
    std::atomic<float> ipd{.064f},fov_noise{0};std::atomic_bool optics_valid{true},fresh_tracking{true},gap_seen{false};std::atomic_uint64_t epoch{1};
    callbacks.tracking_frame=[&](surfxr::TrackingFrame& frame){frame.origin=surfxr::TrackingOrigin::Stage;frame.origin_epoch=epoch.load();frame.boundary=frame.origin_epoch==1?boundary:next_boundary;
        frame.optics_valid=optics_valid.load();frame.ipd_meters=ipd.load();
        frame.eye_fov={{{-.7f,.7f,.7f,-.7f},{-.7f,.7f,.7f,-.7f+fov_noise.load()}}};
        frame.stage_supported=frame.stage_locatable=true;frame.sampled_monotonic_ns=now_ns();
        frame.head.flags=surfxr::OrientationValid|surfxr::PositionValid;frame.head.pose.position={.1f,1.7f,.2f};
        if(!fresh_tracking.load()){gap_seen=true;return false;}return true;};
    callbacks.connection_started=[&]{++media_callbacks;};callbacks.connection_ended=[&]{++media_callbacks;};
    DesktopTracking tracking(options,callbacks,&stop);
    const auto deadline=Clock::now()+std::chrono::seconds(8);
    Connection server;server.modern=true;server.control_socket=listener.accept("127.0.0.2",deadline,{});
    Framer framer(4096,true);
    const auto caps=receive_control(server.control_socket,framer,deadline,{});
    require(std::string(caps.begin(),caps.end()).find("\"frame_desktop_tracking\":1")!=std::string::npos,"test_pose_only_capability");
    require(std::string(caps.begin(),caps.end()).find("\"frame_desktop_optics\":1")!=std::string::npos,"test_desktop_optics_capability");
    const auto enable=[&](bool on){Writer w(true);w.integer(5,4);w.string(on?"frame_desktop_tracking_on":"frame_desktop_tracking_off");server.send_control(w.bytes);};
    unsigned boundaries=0,optical_packets=0;float received_ipd=0;
    const auto pose=[&](const Bytes& bytes){Reader r(bytes,true);const auto type=r.integer(4);
        if(type==4){float x[2]{};for(unsigned eye=0;eye<2;++eye){
                for(unsigned i=0;i<4;++i)(void)r.real();
                x[eye]=r.real();
                for(unsigned i=0;i<6;++i)(void)r.real();}
            r.end();received_ipd=x[1]-x[0];++optical_packets;return false;}
        if(type!=11)return false;
        const auto payload=r.blob();Reader fixed(payload);
        if(payload.size()>=4&&fixed.integer(4)==0x56444244){++boundaries;return false;}
        Reader body(payload,true);require(body.duration()>0&&body.integer(8)==1,"test_pose_only_tracking_bytes");return true;};
    enable(true);unsigned received=0;
    while(received<3&&Clock::now()<deadline)if(pose(receive_control(server.control_socket,framer,deadline,{})))++received;
    require(received==3,"test_desktop_keeps_uploading_pose");
    require(boundaries==0,"legacy desktop peer gets no boundary extension");
    require(optical_packets==0,"old desktop server gets no optical packets before acknowledgement");
    Writer ack(true);ack.integer(5,4);ack.string("frame_boundary_sync_v1");server.send_control(ack.bytes);
    while(!boundaries&&Clock::now()<deadline)(void)pose(receive_control(server.control_socket,framer,deadline,{}));
    require(boundaries==1,"desktop boundary acknowledgement keeps tracking enabled");
    Writer optics_ack(true);optics_ack.integer(5,4);optics_ack.string("frame_desktop_optics_v1");server.send_control(optics_ack.bytes);
    const auto wait_poses=[&](unsigned count){unsigned seen=0;while(seen<count&&Clock::now()<deadline)
            if(pose(receive_control(server.control_socket,framer,deadline,{})))++seen;
        require(seen==count,"desktop optics never interrupts tracking");};
    wait_poses(5);require(optical_packets==1&&received_ipd==.064f,"desktop initial optical update");
    // Noise remains quiet, but a held 0.2 mm adjustment must reach the PC.
    ipd.store(.06402f);fov_noise.store(.0002f);wait_poses(30);
    require(optical_packets==1,"desktop optical noise does not flood lens updates");
    ipd.store(.0642f);wait_poses(40);
    require(optical_packets==2&&received_ipd==.0642f,"desktop final fine adjustment is delivered");
    wait_poses(30);require(optical_packets==2,"held desktop IPD never repeats");
    ipd.store(.0653f);wait_poses(5);
    require(optical_packets==3&&received_ipd==.0653f,"desktop large IPD step remains immediate");
    optics_valid.store(false);ipd.store(.069f);wait_poses(5);
    require(optical_packets==3,"invalid desktop optics do not overwrite PC while poses continue");
    optics_valid.store(true);wait_poses(5);
    require(optical_packets==4&&received_ipd==.069f,"recovered desktop optics synchronize");
    fov_noise.store(.002f);wait_poses(5);require(optical_packets==5,"real desktop FOV update");
    epoch.store(2);wait_poses(5);require(optical_packets==6,"desktop epoch resends optics");
    enable(false);
    // Drain the bounded in-flight control data, then observe a full keepalive
    // interval with no pose packets. The same socket resumes on the next ON.
    auto drain=Clock::now()+std::chrono::milliseconds(100);
    while(Clock::now()<drain){pollfd event{server.control_socket.fd(),POLLIN,0};
        if(::poll(&event,1,5)>0)(void)receive_control(server.control_socket,framer,deadline,{});}
    auto quiet=Clock::now()+std::chrono::milliseconds(1100);unsigned keepalives=0;
    while(Clock::now()<quiet){pollfd event{server.control_socket.fd(),POLLIN,0};if(::poll(&event,1,5)<=0)continue;
        const auto bytes=receive_control(server.control_socket,framer,deadline,{});require(!pose(bytes)&&boundaries==1&&optical_packets==6,"test_disabled_desktop_pose_boundary_and_optics_stop");
        Reader r(bytes,true);if(r.integer(4)==2)++keepalives;}
    require(keepalives>0,"test_pose_switch_preserves_connection");enable(true);
    bool resumed=false;while(!resumed&&Clock::now()<deadline)resumed=pose(receive_control(server.control_socket,framer,deadline,{}));
    require(resumed&&media_callbacks==0,"test_pose_only_resume_never_starts_media");
    while(boundaries<2&&Clock::now()<deadline)(void)pose(receive_control(server.control_socket,framer,deadline,{}));
    require(boundaries==2,"explicit desktop tracking resume sends its initial snapshot");
    require(optical_packets==7,"desktop resume resynchronizes optics once on same connection");
    fresh_tracking=false;
    const auto gap_deadline=Clock::now()+std::chrono::milliseconds(200);
    while(!gap_seen&&Clock::now()<gap_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(gap_seen,"desktop upload owner observed a tracking gap");
    fresh_tracking=true;
    wait_poses(30);
    require(boundaries==2,"same epoch tracking recovery does not republish unchanged boundary");
    Writer ended(true);ended.integer(5,4);ended.string("frame_session_stopped");server.send_control(ended.bytes);
    // Explicit normal shutdown retires this source. Restarting and ordinary
    // transport recovery still retry, but shutdown must not reconnect at 2 s.
    pollfd reconnect{listener.fd(),POLLIN,0};
    require(::poll(&reconnect,1,2300)==0,"test_stopped_desktop_tracking_does_not_reconnect");stop.store(true);
}
void inbound(bool restart,bool trusted=true){
    auto listener=Socket::bind("127.0.0.1",0,true),media=Socket::bind("127.0.0.1",0,false);
    const auto port=socket_port(listener),media_port=socket_port(media);
    auto launch_socket=Socket::bind("127.0.0.1",port,false);
    surfxr::SessionOptions options;options.backend=surfxr::SessionBackend::Surfxr;options.surfxr_version="21.0.0-dev14";
    options.host="127.0.0.1";options.local_address="127.0.0.2";options.surfxr_control_port=port;
    options.surfxr_view_width=options.surfxr_view_height=32;options.surfxr_refresh_rates={90};options.seconds=6;
    std::atomic_bool stop{false};std::atomic<unsigned> connected{0},ended{0};surfxr::SessionResult result;
    std::atomic<unsigned> launches{0};
    std::thread responder([&]{
        while(!stop){
            std::array<std::uint8_t,128> bytes{};sockaddr_in peer{};socklen_t n=sizeof(peer);
            const auto count=::recvfrom(launch_socket.fd(),bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),&n);
            if(count>17&&!std::memcmp(bytes.data(),"ALVRFS01",8)){
                ++launches;Bytes reply{'A','L','V','R','P','S','0','1'};reply.insert(reply.end(),bytes.begin()+8,bytes.begin()+16);reply.push_back(trusted?1:0);
                ::sendto(launch_socket.fd(),reply.data(),reply.size(),0,reinterpret_cast<sockaddr*>(&peer),n);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });Join responder_join{stop,responder};
    std::atomic_uint denied{0};surfxr::SessionCallbacks callbacks;callbacks.connection_started=[&]{++connected;};callbacks.connection_ended=[&]{++ended;};
    callbacks.steamvr_startup=[&](const auto& status){if(!trusted&&status.state==surfxr::SteamVrStartupState::Failed){require(status.reason==surfxr::SteamVrStartupReason::Untrusted,"test_explicit_start_denial_reason");++denied;}};
    std::thread client([&]{result=surfxr::transport::run(options,callbacks,&stop);});Join join{stop,client};
    if(!trusted){client.join();require(result.exit_code==1&&launches==1&&connected==0&&ended==1&&denied==1&&
        result.sanitized_json.find("surfxr_pc_untrusted")!=std::string::npos,"test_explicit_start_denial_is_not_blindly_retried");return;}
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    for(unsigned phase=0;phase<(restart?2u:1u);++phase){
        Connection server;server.modern=true;server.control_socket=listener.accept("127.0.0.2",deadline,{});
        Framer framer(2*1024*1024,true);
        const auto caps=receive_control(server.control_socket,framer,deadline,{});
        const std::string advertised(caps.begin(),caps.end());
        require(advertised.find("\"frame_client_connect\":1")!=std::string::npos&&
            advertised.find("\"frame_session_lifecycle\":1")!=std::string::npos,"test_client_initiates_with_identity_and_lifecycle");
        server.send_control(client_connection_config(media_port,true));
        if(restart&&!phase){server.send_control(control(2,true));continue;}
        server.send_control(control(0,true));
        const auto ready=receive_control(server.control_socket,framer,deadline,{});
        Reader reader(ready,true);require(reader.integer(4)==3,"test_inbound_stream_ready");reader.end();
        // An encoder may have neither NAL config nor video yet. The client must
        // actively request both, including after a runtime restart.
        unsigned requests=0;
        while(requests<2&&Clock::now()<deadline){
            const auto bytes=receive_control(server.control_socket,framer,deadline,{});
            Reader packet(bytes,true);if(packet.integer(4)==1)++requests;
        }
        require(requests==2,"test_idr_retried_before_decoder_config_or_first_video");
        Writer notification(true);notification.integer(5,4);notification.string("frame_session_stopped");
        server.send_control(notification.bytes);
        while(Clock::now()<deadline&&ended.load()<(restart?2u:1u))std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
    require(result.exit_code==0&&result.server_stopped&&result.received_frames==0,"test_explicit_stop_is_normal_without_fabricated_frames");
    require(connected==1&&ended==(restart?2u:1u),"test_attempt_boundary_retires_images_even_on_restart");
    require(result.sanitized_json.find("\"server_stopped\":true")!=std::string::npos,"test_stop_report");
    require(launches==1,"test_only_user_connect_requests_steamvr_never_a_reconnect");
}
}
void client_connect_checks(){
    desktop_tracking_checks();
    const auto now=Clock::now();SessionDeadline deadline(now+std::chrono::seconds(90),now+std::chrono::seconds(10));
    require(deadline.expired(now+std::chrono::seconds(11)),"test_retry_deadline_limits_handshake");
    deadline.connected();
    require(!deadline.expired(now+std::chrono::seconds(60)),"test_retry_deadline_never_truncates_restored_stream");
    require(deadline.expired(now+std::chrono::seconds(90)),"test_restored_stream_preserves_requested_duration");
    inbound(false);inbound(true);inbound(false,false);
}
