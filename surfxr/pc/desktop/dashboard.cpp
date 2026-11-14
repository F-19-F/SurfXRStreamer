#include "dashboard.hpp"
#include <algorithm>
namespace surfxr::transport::desktop {
namespace {
Bytes query(unsigned kind,const Ticket* ticket=nullptr,const sockaddr_in* peer=nullptr,bool streaming=false,unsigned count=0,bool unified=false,unsigned port=control_port,bool media_modes=false,const Bytes& body={},bool vr=false){
    Bytes request(dashboard_magic.begin(),dashboard_magic.end());request.push_back(static_cast<std::uint8_t>(kind));
    if(ticket){request.insert(request.end(),ticket->id.begin(),ticket->id.end());const auto* bytes=reinterpret_cast<const std::uint8_t*>(&peer->sin_addr);request.insert(request.end(),bytes,bytes+4);}
    request.push_back((system_identity()?1:0)|(streaming?2:0)|(unified?4:0)|(media_modes?8:0)|(vr?16:0));
    if(kind==3&&unified){request.push_back(static_cast<std::uint8_t>(port));request.push_back(static_cast<std::uint8_t>(port>>8));}
    if(kind==4)request.push_back(static_cast<std::uint8_t>(count));
    if(kind==10)request.insert(request.end(),body.begin(),body.end());
    Socket socket(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));require(socket.value!=INVALID_SOCKET,"desktop_dashboard_socket");
    DWORD timeout=500;setsockopt(socket.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(socket.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(dashboard_port);
    require(connect(socket.value,reinterpret_cast<const sockaddr*>(&target),sizeof(target))==0,"desktop_dashboard_unavailable");
    socket.send_all(request);Bytes reply(1);socket.read_all(reply.data(),1);
    if(kind==1&&reply[0]==0){reply.resize(70);socket.read_all(reply.data()+1,69);}
    if(kind==4&&reply[0]==0){reply.resize(2);socket.read_all(reply.data()+1,1);}
    if((kind==6||kind==10)&&reply[0]==0){std::uint8_t prefix[4];socket.read_all(prefix,4);Reader r(prefix,4);const auto size=r.integer(4);require(size<=65536,"desktop_dashboard_audio_bound");reply.resize(static_cast<std::size_t>(size)+1);socket.read_all(reply.data()+1,static_cast<std::size_t>(size));}
    return reply;
}
}
bool system_identity(){
    Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return false;
    std::array<std::uint8_t,512> bytes{};DWORD size=0;
    if(!GetTokenInformation(token.value,TokenUser,bytes.data(),static_cast<DWORD>(bytes.size()),&size))return false;
    return IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,WinLocalSystemSid)!=FALSE;
}
Ticket consume_ticket(Control& control,const sockaddr_in& peer){
    const auto bytes=control.receive();require((bytes.size()==40||(bytes.size()==41&&bytes[40]==1))&&std::equal(ticket_magic.begin(),ticket_magic.end(),bytes.begin()),"desktop_dashboard_ticket_required");
    Ticket ticket;std::copy_n(bytes.data()+8,32,ticket.id.begin());auto result=dashboard_ticket(query(1,&ticket,&peer));result.media_modes=bytes.size()==41;return result;
}
Bytes dashboard_settings(const Ticket& ticket,const sockaddr_in& peer,const Bytes& request){
    try{auto reply=query(10,&ticket,&peer,false,0,false,control_port,false,request);
        if((reply.size()==41||reply.size()==49)&&reply[0]==0)return Bytes(reply.begin()+1,reply.end());
    }catch(...){}return {};
}
bool dashboard_pointer_policy(const Ticket& ticket,const sockaddr_in& peer,bool fallback){
    Bytes request(25);request[16]=0x80;
    const auto reply=dashboard_settings(ticket,peer,request);
    if(reply.size()!=48)return fallback;
    Reader r(reply.data()+40,8);const auto bits=r.integer(8);double value=0;std::memcpy(&value,&bits,8);
    return value==0?false:value==1?true:fallback;
}
bool dashboard_announce(bool unified,unsigned port,bool media_modes){try{return query(3,nullptr,nullptr,false,0,unified,port,media_modes)[0]==0;}catch(...){return false;}}
Bytes dashboard_audio(const Ticket& ticket,const sockaddr_in& peer){auto bytes=query(6,&ticket,&peer);require(bytes[0]==0,"desktop_dashboard_audio_unavailable");return Bytes(bytes.begin()+1,bytes.end());}
unsigned dashboard_lease_status(const Ticket& ticket,const sockaddr_in& peer,bool streaming,bool vr){try{return query(2,&ticket,&peer,streaming,0,false,control_port,false,{},vr)[0];}catch(...){return 3;}}
bool dashboard_lease(const Ticket& ticket,const sockaddr_in& peer,bool streaming){return dashboard_lease_status(ticket,peer,streaming)==0;}
unsigned dashboard_monitors(const Ticket& ticket,const sockaddr_in& peer,unsigned count){
    try{const auto reply=query(4,&ticket,&peer,false,count);require(reply[0]==0,"desktop_monitor_preference_failed");return reply[1];}
    catch(...){return 0;}
}
DashboardWatch::DashboardWatch(const Ticket& ticket,const sockaddr_in& peer,unsigned port){
    // Older Dashboards close unknown opcodes. Keep their lease fallback.
    try{
        auto socket=std::make_unique<Socket>(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        require(socket->value!=INVALID_SOCKET,"desktop_dashboard_watch_socket");
        DWORD timeout=500;setsockopt(socket->value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        setsockopt(socket->value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        sockaddr_in target{};target.sin_family=AF_INET;target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);target.sin_port=htons(static_cast<u_short>(port));
        require(connect(socket->value,reinterpret_cast<const sockaddr*>(&target),sizeof(target))==0,"desktop_dashboard_watch_unavailable");
        Bytes request(dashboard_magic.begin(),dashboard_magic.end());request.push_back(11);
        request.insert(request.end(),ticket.id.begin(),ticket.id.end());
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(&peer.sin_addr);request.insert(request.end(),bytes,bytes+4);
        request.push_back(system_identity()?1:0);socket->send_all(request);
        std::uint8_t reply=0;socket->read_all(&reply,1);
        if(reply==2)revoked_=true;
        else if(reply==0)socket_=std::move(socket);
    }catch(...){}
}
bool DashboardWatch::revoked(){
    try{
    if(socket_&&socket_->ready(0)){
        std::uint8_t value=0;const auto count=recv(socket_->value,reinterpret_cast<char*>(&value),1,0);
        if(count==1&&value==2)revoked_=true;
        // EOF alone is not an intentional revocation. Keep the existing
        // heartbeat grace for broker crashes and transient transport failures.
        if(count<=0||revoked_)socket_.reset();
    }
    }catch(...){socket_.reset();}
    return revoked_;
}
}
