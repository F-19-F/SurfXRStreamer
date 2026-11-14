#include "start.hpp"
#include "session_deadline.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

namespace surfxr::transport {
void mode_start_checks();
void steamvr_start_checks(){
    mode_start_checks();
    const auto now=Clock::now();SessionDeadline budget(now+std::chrono::seconds(90),now+std::chrono::seconds(10));
    require(!budget.expired(now+std::chrono::seconds(11),true)&&budget.expired(now+std::chrono::seconds(90),true),"startup_progress_preserves_only_recovery_budget");
    for(const auto phase:{0u,1u,3u,99u}){
        auto socket=Socket::bind("127.0.0.1",0,false);sockaddr_in address{};socklen_t size=sizeof(address);
        require(::getsockname(socket.fd(),reinterpret_cast<sockaddr*>(&address),&size)==0,"startup_test_port_reserved");
        SessionOptions options;options.host="127.0.0.1";options.local_address="127.0.0.2";options.surfxr_control_port=ntohs(address.sin_port);
        std::atomic_bool stop{false};std::atomic_uint queries{0},ready{0},failed{0},launches{0};std::exception_ptr error;
        std::thread responder([&]{try{
            while(!stop.load()){
                pollfd event{socket.fd(),POLLIN,0};if(::poll(&event,1,10)<=0)continue;
                std::array<std::uint8_t,128> bytes{};sockaddr_in peer{};socklen_t count=sizeof(peer);
                const auto n=::recvfrom(socket.fd(),bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),&count);
                if(n==27&&!std::memcmp(bytes.data(),"ALVRFS01",8)){
                    require(phase==0&&++launches==1,"startup_idle_retries_one_user_launch_only");
                    Bytes reply{'A','L','V','R','P','S','0','1'};reply.insert(reply.end(),bytes.begin()+8,bytes.begin()+16);reply.push_back(1);
                    ::sendto(socket.fd(),reply.data(),reply.size(),0,reinterpret_cast<sockaddr*>(&peer),count);continue;
                }
                require(n==27&&!std::memcmp(bytes.data(),"ALVRFQ01",8)&&bytes[16]==10&&
                    !std::memcmp(bytes.data()+17,"frame-test",10),"startup_status_query_is_read_only_and_has_identity");
                const auto query=++queries;if(phase==99)continue;
                Writer reply;reply.bytes={'A','L','V','R','P','Q','0','1'};reply.bytes.insert(reply.bytes.end(),bytes.begin()+8,bytes.begin()+16);
                reply.integer(1,1);reply.integer((phase==1&&query>1)||(phase==0&&launches)?2:phase,1);reply.integer(phase==3?2:0,1);reply.integer(17,8);
                if(phase==1&&query==1){auto forged=reply.bytes;forged[8]^=1;forged[17]=2;
                    ::sendto(socket.fd(),forged.data(),forged.size(),0,reinterpret_cast<sockaddr*>(&peer),count);
                    // A slow first producer response must not be treated as a
                    // legacy peer after only the old 40 ms read window.
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));}
                require(::sendto(socket.fd(),reply.bytes.data(),reply.bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),count)==27,"startup_test_reply");
            }
        }catch(...){error=std::current_exception();}});
        struct Join {std::atomic_bool& stop;std::thread& thread;~Join(){stop.store(true);if(thread.joinable())thread.join();}} join{stop,responder};
        SessionCallbacks callbacks;callbacks.steamvr_startup=[&](const auto& status){
            require(status.state!=SteamVrStartupState::Completed,"driver_ready_never_fabricates_xr_completion");
            if(status.state==SteamVrStartupState::RuntimeReady)++ready;
            if(status.state==SteamVrStartupState::Failed){require(status.reason==SteamVrStartupReason::LauncherFailed&&status.request_id==17,"startup_failure_reason_retained");++failed;}
        };
        {SteamVrStartupMonitor monitor(options,"frame-test",callbacks,phase==0);const auto deadline=Clock::now()+std::chrono::seconds(3);bool rejected=false;
            try{monitor.wait_ready([&]{return Clock::now()>=deadline;});}catch(const std::runtime_error& e){rejected=std::strcmp(e.what(),"surfxr_steamvr_start_failed")==0;}
            require(phase==3?rejected&&failed==1:!rejected,"startup_wait_has_explicit_failure");
            if(phase==1)require(queries>=2&&ready==1&&!monitor.pending(),"startup_waits_for_authenticated_nonce_matching_ready");
            if(phase==0)require(launches==1&&queries>=2&&ready==1,"startup_idle_makes_one_bounded_launch_retry");
            if(phase==99)require(!monitor.pending()&&queries>0,"legacy_status_peer_retains_compatibility_path");
            monitor.video_ready();
        }
        stop.store(true);responder.join();
        if(error)std::rethrow_exception(error);
    }
}
}
