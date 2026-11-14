#include "network.hpp"

namespace surfxr::transport::desktop {
bool Socket::ready(unsigned milliseconds)const{if(poll)return poll(milliseconds);WSAPOLLFD p{value,POLLRDNORM,0};const auto result=WSAPoll(&p,1,static_cast<int>(milliseconds));require(result!=SOCKET_ERROR,"desktop_socket_poll");return result>0;}
Socket Socket::bind(const std::string& ip,unsigned port,bool tcp,unsigned receive_buffer_bytes){
    Socket s(::socket(AF_INET,tcp?SOCK_STREAM:SOCK_DGRAM,0));require(s.value!=INVALID_SOCKET,"desktop_socket_create");
    if(receive_buffer_bytes){require(receive_buffer_bytes<=4*1024*1024,"desktop_receive_buffer_bound");const int bytes=static_cast<int>(receive_buffer_bytes);require(setsockopt(s.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes))==0,"desktop_receive_buffer_failed");}
    BOOL exclusive=TRUE;require(setsockopt(s.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0,"desktop_socket_exclusive");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<u_short>(port));require(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)==1,"desktop_bind_ipv4");
    require(::bind(s.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==0,"desktop_port_in_use");
    if(tcp)require(listen(s.value,1)==0,"desktop_socket_listen");return s;
}
void Socket::send_all(const Bytes& b)const{
    const auto deadline=Clock::now()+std::chrono::seconds(2);std::size_t at=0;
    while(at<b.size()){require(Clock::now()<deadline,"desktop_control_send_timeout");const auto n=::send(value,reinterpret_cast<const char*>(b.data()+at),static_cast<int>(b.size()-at),0);require(n>0,"desktop_control_send_failed");at+=static_cast<std::size_t>(n);}
}
void Socket::read_all(std::uint8_t* data,std::size_t count)const{
    const auto deadline=Clock::now()+std::chrono::seconds(2);std::size_t at=0;
    while(at<count){require(Clock::now()<deadline,"desktop_control_receive_timeout");const auto n=recv(value,reinterpret_cast<char*>(data+at),static_cast<int>(count-at),0);require(n>0,"desktop_control_closed");at+=static_cast<std::size_t>(n);}
}
void Control::send(const Bytes& body){std::lock_guard<std::mutex> lock(send_mutex_);if(write){write(body);return;}auto b=outgoing?outgoing->seal(body):body;Bytes framed;for(unsigned i=0;i<4;++i)framed.push_back(static_cast<std::uint8_t>(b.size()>>(8*i)));framed.insert(framed.end(),b.begin(),b.end());socket.send_all(framed);}
Bytes Control::receive(){if(read)return read();std::uint8_t prefix[4];socket.read_all(prefix,4);Reader r(prefix,4);const auto size=r.integer(4);require(size>0&&size<=2*1024*1024,"desktop_control_length");Bytes body(static_cast<std::size_t>(size));socket.read_all(body.data(),body.size());return incoming?incoming->open(body):body;}
void authenticate(Control& control,const Key& root,Key& server,Key& client){
    server=random();Bytes hello(magic.begin(),magic.end());hello.insert(hello.end(),server.begin(),server.end());control.send(hello);
    auto b=control.receive();require(b.size()==64,"desktop_auth_failed");Key mac{};std::copy_n(b.data(),32,client.begin());std::copy_n(b.data()+32,32,mac.begin());
    require(equal(mac,proof(root,server,client,"ALVR-DESKTOP-client-proof-v1")),"desktop_auth_failed");
    const auto ack=proof(root,server,client,"ALVR-DESKTOP-server-proof-v1");control.send(Bytes(ack.begin(),ack.end()));
    control.incoming=std::make_unique<Cipher>(proof(root,server,client,"ALVR-DESKTOP-control-c2s-v1"));control.outgoing=std::make_unique<Cipher>(proof(root,server,client,"ALVR-DESKTOP-control-s2c-v1"));
}
std::optional<Bytes> Control::poll_receive(){
    // Only this owner reads the source socket. Preserve a partial frame while
    // SteamVR is busy; select/recv EOF still reports a confirmed disconnect.
    const auto packet=[&]()->std::optional<Bytes>{
        if(pending_.size()<4)return std::nullopt;
        Reader prefix(pending_.data(),4);const auto count=prefix.integer(4);require(count>0&&count<=2*1024*1024,"desktop_control_bound");
        if(pending_.size()<count+4)return std::nullopt;
        Bytes body(pending_.begin()+4,pending_.begin()+4+static_cast<std::ptrdiff_t>(count));pending_.erase(pending_.begin(),pending_.begin()+4+static_cast<std::ptrdiff_t>(count));
        return incoming?incoming->open(body):body;
    };
    if(auto complete=packet())return complete;
    if(socket.ready(0)){
        std::array<std::uint8_t,8192> bytes{};const auto n=recv(socket.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0);
        require(n>0,"desktop_control_closed");pending_.insert(pending_.end(),bytes.begin(),bytes.begin()+n);
        require(pending_.size()<=2*1024*1024+8196,"desktop_control_pending_bound");
    }
    return packet();
}
void Control::queue_send(const Bytes& body){
    require(pending_send_.empty()&&body.size()<=2*1024*1024,"desktop_control_pending_send_bound");
    auto bytes=outgoing?outgoing->seal(body):body;
    for(unsigned i=0;i<4;++i)pending_send_.push_back(static_cast<std::uint8_t>(bytes.size()>>(8*i)));
    pending_send_.insert(pending_send_.end(),bytes.begin(),bytes.end());send_offset_=0;
}
bool Control::flush_send(){
    while(send_offset_<pending_send_.size()){
        const auto n=::send(socket.value,reinterpret_cast<const char*>(pending_send_.data()+send_offset_),static_cast<int>(pending_send_.size()-send_offset_),0);
        if(n==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return false;
        require(n>0,"desktop_control_closed");send_offset_+=static_cast<std::size_t>(n);
    }
    pending_send_.clear();send_offset_=0;return true;
}
bool verify_capabilities(const Bytes& b,bool* keyboard,bool* candidate_click,bool* desktop_keyboard){
    Reader r(b,true);require(r.integer(4)==0,"desktop_client_connection_result");r.integer(8);r.string(128);require(r.integer(4)==0,"desktop_ipv4_required");r.skip(4);require(r.boolean(),"desktop_streaming_capability_required");
    for(unsigned i=0;i<4;++i)r.integer(4);const auto rates=r.integer(8);require(rates>0&&rates<=16,"desktop_refresh_capability");for(unsigned i=0;i<rates;++i)r.real();r.integer(4);
    for(unsigned i=0;i<5;++i)r.boolean();r.real();r.boolean();const auto extension=r.string(4096);r.end();
    require(extension.find("\"frame_desktop\":1")!=std::string::npos&&extension.find("\"frame_desktop_input\":1")!=std::string::npos,"desktop_extension_required");
    if(desktop_keyboard)*desktop_keyboard=extension.find("\"frame_desktop_keyboard\":1")!=std::string::npos;
    if(keyboard)*keyboard=extension.find("\"frame_keyboard\":1")!=std::string::npos;
    if(candidate_click)*candidate_click=extension.find("\"frame_candidate_click\":1")!=std::string::npos;
    return extension.find("\"frame_desktop_multimonitor\":1")!=std::string::npos;
}
}
