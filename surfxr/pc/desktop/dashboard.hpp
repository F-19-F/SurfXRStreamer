#pragma once
#include "network.hpp"
#include "surfxr/surfxr_dashboard.hpp"
namespace surfxr::transport::desktop {
bool system_identity();
Ticket consume_ticket(Control&,const sockaddr_in&);
bool dashboard_announce(bool unified=false,unsigned port=control_port,bool media_modes=false);
Bytes dashboard_settings(const Ticket&,const sockaddr_in&,const Bytes&);
bool dashboard_pointer_policy(const Ticket&,const sockaddr_in&,bool fallback=false);
Bytes dashboard_audio(const Ticket&,const sockaddr_in&);
bool dashboard_lease(const Ticket&,const sockaddr_in&,bool streaming);
unsigned dashboard_lease_status(const Ticket&,const sockaddr_in&,bool streaming,bool vr=false);
unsigned dashboard_monitors(const Ticket&,const sockaddr_in&,unsigned count=0);
// Explicit session revocation is independent of the heartbeat's stall grace.
class DashboardWatch {
public:
    DashboardWatch(const Ticket&,const sockaddr_in&,unsigned port=dashboard_port);
    explicit DashboardWatch(Socket socket):socket_(std::make_unique<Socket>(std::move(socket))){}
    bool revoked();
private:
    std::unique_ptr<Socket> socket_;
    bool revoked_=false;
};
}
