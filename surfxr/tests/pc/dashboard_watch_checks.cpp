#include "dashboard.hpp"
#include <algorithm>
#include <iostream>
#include <future>

namespace surfxr::transport::desktop {
void dashboard_watch_self_test(){
    WSADATA startup{};require(WSAStartup(MAKEWORD(2,2),&startup)==0,"dashboard_watch_test_winsock");
    struct Cleanup{~Cleanup(){WSACleanup();}}cleanup;
    for(unsigned reply:{0u,2u,255u}){
        auto listener=Socket::bind("127.0.0.1",0,true);
        sockaddr_in address{};int length=sizeof(address);
        require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"dashboard_watch_handshake_endpoint");
        Ticket ticket;ticket.id.fill(17);
        auto broker=std::async(std::launch::async,[&]{
            Socket socket(accept(listener.value,nullptr,nullptr));
            require(socket.value!=INVALID_SOCKET,"dashboard_watch_handshake_accept");
            DWORD timeout=500;setsockopt(socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            Bytes request(46);socket.read_all(request.data(),request.size());
            require(std::equal(dashboard_magic.begin(),dashboard_magic.end(),request.begin())&&request[8]==11,"dashboard_watch_handshake_opcode");
            require(std::equal(ticket.id.begin(),ticket.id.end(),request.begin()+9),"dashboard_watch_handshake_ticket");
            require(std::memcmp(request.data()+41,&address.sin_addr,4)==0,"dashboard_watch_handshake_peer");
            if(reply!=255)socket.send_all(Bytes{static_cast<std::uint8_t>(reply)});
            if(reply==0)socket.send_all(Bytes{2});
            shutdown(socket.value,SD_SEND);
        });
        DashboardWatch watch(ticket,address,ntohs(address.sin_port));
        broker.get();
        const auto deadline=Clock::now()+std::chrono::milliseconds(250);
        while(reply!=255&&!watch.revoked()&&Clock::now()<deadline)Sleep(1);
        require(watch.revoked()==(reply!=255),"dashboard_watch_handshake_compatibility");
    }
    // Reserve ephemeral loopback listeners; never touch the installed broker.
    for(bool explicit_revoke:{false,true}){
        auto listener=Socket::bind("127.0.0.1",0,true);
        sockaddr_in address{};int length=sizeof(address);
        require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"dashboard_watch_test_endpoint");
        Socket host(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        require(connect(host.value,reinterpret_cast<sockaddr*>(&address),length)==0,"dashboard_watch_test_connect");
        Socket broker(accept(listener.value,nullptr,nullptr));require(broker.value!=INVALID_SOCKET,"dashboard_watch_test_accept");
        DashboardWatch watch(std::move(host));
        const auto begin=Clock::now();
        for(unsigned i=0;i<10000;++i)require(!watch.revoked(),"dashboard_watch_idle_revoked");
        const auto cost=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count();
        std::cout<<"Dashboard notification idle poll: "<<cost/10000.0<<" us/call\n";
        if(explicit_revoke)broker.send_all(Bytes{2});
        shutdown(broker.value,SD_SEND);
        const auto deadline=Clock::now()+std::chrono::milliseconds(250);
        if(explicit_revoke){
            while(!watch.revoked()&&Clock::now()<deadline)Sleep(1);
            require(watch.revoked(),"dashboard_watch_explicit_revoke_delayed");
            require(watch.revoked(),"dashboard_watch_revocation_not_latched");
        }else{
            // EOF/crash must preserve the daemon's existing heartbeat grace.
            for(unsigned i=0;i<100;++i)require(!watch.revoked(),"dashboard_watch_eof_revoked");
        }
    }
}
}
