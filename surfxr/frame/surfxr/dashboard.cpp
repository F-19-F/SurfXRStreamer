#include "dashboard.hpp"
#include "socket.hpp"
#include <poll.h>
#include <unistd.h>

namespace surfxr::transport {
namespace {
bool media_modes_available(const SessionOptions& options,const std::function<bool()>& cancelled){
    try{
        auto socket=Socket::connect_tcp(options.host,options.surfxr_dashboard_port,options.local_address,Clock::now()+std::chrono::milliseconds(500),cancelled);
        Bytes query(desktop::dashboard_magic.begin(),desktop::dashboard_magic.end());query.push_back(9);socket.send(query,true,cancelled);
        std::uint8_t response=0;const auto end=Clock::now()+std::chrono::milliseconds(500);
        while(Clock::now()<end){if(socket.read(&response,1,true))return response==1;
            if(cancelled())throw Cancelled{};
            pollfd fd{socket.fd(),POLLIN,0};::poll(&fd,1,5);}
    }catch(const Cancelled&){throw;}catch(...){}return false;
}
}
desktop::Ticket request_desktop(const SessionOptions& options,const std::function<bool()>& cancelled){
    std::array<char,64> hostname{};require(::gethostname(hostname.data(),hostname.size())==0,"surfxr_client_hostname_failed");
    hostname.back()=0;
    for(auto& c:hostname)if(c&&!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))c='-';
    std::string name(hostname.data());
    require(!name.empty()&&name.size()<=63&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"surfxr_invalid_client_hostname");
    Bytes query(desktop::dashboard_magic.begin(),desktop::dashboard_magic.end());query.push_back(0);query.push_back(static_cast<std::uint8_t>(name.size()));query.insert(query.end(),name.begin(),name.end());
    const auto deadline=Clock::now()+std::chrono::seconds(60);
    unsigned last_status=1;
    while(Clock::now()<deadline){
        auto socket=Socket::connect_tcp(options.host,options.surfxr_dashboard_port,options.local_address,std::min(deadline,Clock::now()+std::chrono::seconds(3)),cancelled);
        socket.send(query,true,cancelled);Bytes reply(70);std::size_t used=0;
        const auto receive_deadline=std::min(deadline,Clock::now()+std::chrono::seconds(3));
        while(used<reply.size()){
            if(cancelled())throw Cancelled{};
            require(Clock::now()<receive_deadline,"surfxr_dashboard_desktop_timeout");
            used+=socket.read(reply.data()+used,reply.size()-used,true);
            if(used&&reply[0])break;
            pollfd event{socket.fd(),POLLIN,0};::poll(&event,1,5);
        }
        if(reply[0]==0){auto ticket=desktop::dashboard_ticket(reply);if(ticket.unified)ticket.media_modes=media_modes_available(options,cancelled);return ticket;}
        last_status=reply[0];require(last_status==1||last_status==3,"surfxr_dashboard_desktop_disabled_or_host_unavailable");
        const auto retry=std::min(deadline,Clock::now()+std::chrono::milliseconds(500));
        while(Clock::now()<retry){if(cancelled())throw Cancelled{};poll(nullptr,0,10);}
    }
    fail(last_status==1?"surfxr_pc_untrusted":"surfxr_dashboard_desktop_disabled_or_host_unavailable");return {};
}
}
