#include "surfxr/surfxr.hpp"
#include "start.hpp"
#include <sys/random.h>
#include <cstring>
#include <poll.h>

namespace surfxr::transport {
bool request_steamvr(const SessionOptions& options,const std::string& hostname,const std::function<bool()>& cancelled){
    require(!hostname.empty()&&hostname.size()<=63,"surfxr_invalid_client_hostname");
    Bytes query{'A','L','V','R','F','S','0','1'};query.resize(16);
    require(::getrandom(query.data()+8,8,0)==8,"surfxr_start_nonce_failed");
    query.push_back(static_cast<std::uint8_t>(hostname.size()));query.insert(query.end(),hostname.begin(),hostname.end());
    auto socket=Socket::bind(options.local_address,0,false);socket.connect_udp(options.host,options.surfxr_control_port);
    socket.send(query,false,cancelled);
    const auto deadline=Clock::now()+std::chrono::milliseconds(500);
    std::array<std::uint8_t,128> reply{};
    while(Clock::now()<deadline){
        if(cancelled&&cancelled())throw Cancelled{};
        try{
            const auto n=socket.read(reply.data(),reply.size(),false);
            if(n==17&&!std::memcmp(reply.data(),"ALVRPS01",8)&&!std::memcmp(reply.data()+8,query.data()+8,8)){
                require(reply[16]<=1,"surfxr_invalid_start_response");
                require(reply[16]==1,"surfxr_pc_untrusted");return true;
            }
        }catch(const std::runtime_error& e){if(std::string(e.what())!="surfxr_receive_failed")throw;return false;}
        pollfd event{socket.fd(),POLLIN,0};::poll(&event,1,20);
    }
    return false; // An already running driver/older companion may not advertise this capability.
}
}
