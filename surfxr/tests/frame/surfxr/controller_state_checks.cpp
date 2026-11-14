#include "upload.hpp"
#include <cmath>
#include <fcntl.h>
#include <sys/socket.h>

namespace surfxr::transport {
void controller_state_checks(){
    for(const auto& flags:{std::pair{true,true},std::pair{true,false},std::pair{false,true}}){
        int sockets[2];require(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0,"controller control pair");
        for(int fd:sockets)fcntl(fd,F_SETFL,O_NONBLOCK);
        Connection connection;connection.modern=flags.first;connection.control_socket=Socket(sockets[0]);
        Socket server(sockets[1]);Framer framer(65536,flags.first);TrackingLedger ledger;
        SessionOptions options;options.surfxr_version=flags.first?"21.0.0-dev14":"20.14.1";
        std::atomic_bool enabled{true};SessionCallbacks callbacks;
        // No valid head sample and no UDP socket: the real Upload worker must
        // still notify the PC of both controller edges over reliable control.
        callbacks.tracking_frame=[](TrackingFrame& frame){
            frame.battery.hmd={99,true,true};frame.battery.hmd_sampled_monotonic_ns=now_ns();
            // Stale controllers must not piggyback on the fresh HMD reading.
            frame.battery.controllers[0]={73,false,true};
            frame.battery.controllers_sampled_monotonic_ns=now_ns()-6'000'000'000ull;
            return false;
        };
        callbacks.vr_controllers_enabled=[&]{return enabled.load();};
        Upload upload(connection,ledger,options,callbacks,120,false,true,false,false,flags.second);
        unsigned received=0,batteries=0;std::uint64_t previous=0;
        for(bool desired:{true,false,true}){
            enabled.store(desired);unsigned this_edge=0;
            const auto deadline=Clock::now()+std::chrono::milliseconds(80);
            while(Clock::now()<deadline){
                std::uint8_t buffer[65536];const auto n=server.read(buffer,sizeof(buffer),true);
                if(n)framer.feed(buffer,n,[&](Bytes bytes){Reader r(bytes,flags.first);
                    const auto kind=r.integer(4);
                    if(kind==5){
                        require(r.integer(8)==path_id("/user/head"),"only fresh HMD battery sent without tracking");
                        require(std::abs(r.real()-.99f)<.00001f&&r.boolean(),"runtime display percentage and charging survive SURFXR wire");
                        r.end();++batteries;return;
                    }
                    if(kind!=11)return;
                    require(flags.first&&flags.second,"legacy session must not send state extension");
                    const auto bytes_body=r.blob();r.end();Reader body(bytes_body);
                    require(body.integer(4)==0x56444353&&body.integer(4)==1,"state wire magic/version");
                    const auto since=body.integer(8);
                    require(since>previous&&since<=now_ns(),"state barrier uses client monotonic clock");
                    require(body.integer(4)==unsigned(desired),"state applies to both hands");body.end();
                    previous=since;++this_edge;++received;
                });
                upload.check();std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(this_edge==unsigned(flags.first&&flags.second),"one reliable notification per initial state/edge");
        }
        require(received==unsigned(flags.first&&flags.second)*3,"all forwarding transitions delivered without UDP");
        require(batteries==1,"battery sent without pose on both SURFXR versions, rate limited");
    }
}
}
