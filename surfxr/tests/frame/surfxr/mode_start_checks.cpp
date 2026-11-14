#include "mode_startup.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

namespace surfxr::transport {
void mode_start_checks(){
    Modes policy(true);policy.prepare_vr(true);policy.begin();policy.request(false);
    const auto first=policy.preparation();
    require(first&&!policy.pending()&&policy.desktop()&&!policy.desired_desktop(),"preparation_keeps_desktop_source_running");
    policy.request(true);policy.request(false);const auto next=policy.preparation();
    policy.prepared(first,true);require(next!=first&&!policy.pending(),"late_startup_cannot_commit_new_request");
    policy.prepared(next,false);require(policy.desired_desktop()&&!policy.pending(),"failed_startup_keeps_desktop_running");
    policy.request(false);policy.prepared(policy.preparation(),true);
    require(policy.pending()&&!policy.begin(),"ready_startup_enters_vr");
    require(policy.take_prepared()&&!policy.take_prepared(),"prepared_launch_consumed_once");
    policy.request(true);require(policy.pending()&&policy.begin(),"return_to_desktop_never_waits_for_startup");

    // Reserved private endpoint; no discovery, real launcher or streaming peer.
    auto socket=Socket::bind("127.0.0.1",0,false);sockaddr_in address{};socklen_t size=sizeof(address);
    require(::getsockname(socket.fd(),reinterpret_cast<sockaddr*>(&address),&size)==0,"mode_start_test_port");
    SessionOptions options;options.host="127.0.0.1";options.local_address="127.0.0.2";options.surfxr_control_port=ntohs(address.sin_port);
    auto modes=std::make_shared<Modes>(true);modes->prepare_vr(true);modes->begin();
    std::atomic_bool stop{false};std::atomic_uint phase{1},queries{0},launches{0},failures{0};std::exception_ptr error;
    std::thread responder([&]{try{
        while(!stop.load()){
            pollfd event{socket.fd(),POLLIN,0};if(::poll(&event,1,10)<=0)continue;
            std::array<std::uint8_t,128> bytes{};sockaddr_in peer{};socklen_t count=sizeof(peer);
            const auto n=::recvfrom(socket.fd(),bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),&count);
            require(n>=18&&n==17+bytes[16],"mode_start_identity_length");
            Writer reply;
            if(!std::memcmp(bytes.data(),"ALVRFS01",8)){
                ++launches;reply.bytes={'A','L','V','R','P','S','0','1'};
                reply.bytes.insert(reply.bytes.end(),bytes.begin()+8,bytes.begin()+16);reply.integer(1,1);
            }else{
                require(!std::memcmp(bytes.data(),"ALVRFQ01",8),"mode_start_status_query");++queries;
                reply.bytes={'A','L','V','R','P','Q','0','1'};
                reply.bytes.insert(reply.bytes.end(),bytes.begin()+8,bytes.begin()+16);
                const auto state=phase.load();reply.integer(1,1);reply.integer(state,1);reply.integer(state==3?2:0,1);reply.integer(31,8);
            }
            require(::sendto(socket.fd(),reply.bytes.data(),reply.bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),count)==ssize_t(reply.bytes.size()),"mode_start_reply");
        }
    }catch(...){error=std::current_exception();}});
    struct Join {std::atomic_bool& stop;std::thread& thread;~Join(){stop.store(true);if(thread.joinable())thread.join();}} join{stop,responder};
    SessionCallbacks callbacks;callbacks.steamvr_startup=[&](const auto& value){if(value.state==SteamVrStartupState::Failed)++failures;};
    const auto until=[&](auto predicate){const auto deadline=Clock::now()+std::chrono::seconds(3);
        while(!predicate()&&Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(predicate(),"mode_start_wait_timeout");};
    {
        ModeStartup startup(options,callbacks,modes,[&]{return stop.load();});
        modes->request(false);until([&]{return queries>=1;});
        require(!modes->cancel.load()&&modes->desktop(),"startup_status_wait_does_not_cancel_video_ingestion");
        modes->request(true);phase=2;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(!modes->pending()&&modes->desired_desktop(),"cancelled_prepare_never_switches_on_late_ready");
        modes->request(false);until([&]{return modes->pending();});
        require(!modes->begin()&&launches>=2,"ready_preparation_commits_matching_mode");
        modes->request(true);modes->begin();phase=3;
        modes->request(false);until([&]{return modes->desired_desktop();});
        require(failures&&!modes->pending()&&modes->desktop(),"launch_failure_does_not_stop_healthy_desktop");
        stop.store(true);until([&]{return modes->pending();});
        modes->begin();until([&]{return modes->pending();});
        // External stop must win even if a just-starting source clears cancel.
        startup.close();
    }
    stop.store(true);responder.join();if(error)std::rethrow_exception(error);
}
}
