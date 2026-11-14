#include "socket.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace surfxr::transport {
std::uint64_t now_ns(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());}
Socket::~Socket(){if(fd_>=0)::close(fd_);}
bool Socket::readable()const{if(adapter_)return adapter_->readable();pollfd fd{fd_,POLLIN,0};::poll(&fd,1,0);return fd.revents&(POLLIN|POLLERR|POLLHUP);}
Socket& Socket::operator=(Socket&& other)noexcept{if(this!=&other){if(fd_>=0)::close(fd_);fd_=std::exchange(other.fd_,-1);adapter_=std::move(other.adapter_);}return *this;}
namespace {
sockaddr_in address(const std::string& host,unsigned port){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(static_cast<std::uint16_t>(port));
    if(!host.empty())require(inet_pton(AF_INET,host.c_str(),&a.sin_addr)==1,"surfxr_ipv4_required");
    return a;
}
void cancelled(const std::function<bool()>& f){if(f&&f())throw Cancelled{};}
}
Socket Socket::bind(const std::string& host,unsigned port,bool tcp,unsigned receive_buffer_bytes){
    Socket result(::socket(AF_INET,(tcp?SOCK_STREAM:SOCK_DGRAM)|SOCK_NONBLOCK|SOCK_CLOEXEC,0));require(result.fd_>=0,"surfxr_socket_failed");
    require(receive_buffer_bytes<=4*1024*1024,"surfxr_invalid_receive_buffer");
    // Choose before listen/SYN so TCP negotiates sufficient window scaling.
    // Keep the explicit bound; decoder queues/metadata deadlines own freshness.
    const int one=1,buffer=receive_buffer_bytes?static_cast<int>(receive_buffer_bytes):(tcp?256:4096)*1024;
    if(tcp){::setsockopt(result.fd_,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));::setsockopt(result.fd_,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));}
    ::setsockopt(result.fd_,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer));
    const auto a=address(host,port);require(::bind(result.fd_,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==0,"surfxr_bind_failed");
    if(tcp)require(::listen(result.fd_,2)==0,"surfxr_listen_failed");
    return result;
}
Socket Socket::accept(const std::string& host,Clock::time_point deadline,const std::function<bool()>& cancel)const{
    const auto expected=address(host,0);
    while(Clock::now()<deadline){
        cancelled(cancel);sockaddr_in peer{};socklen_t size=sizeof(peer);
        const int client=::accept4(fd_,reinterpret_cast<sockaddr*>(&peer),&size,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(client>=0){Socket s(client);if(peer.sin_addr.s_addr!=expected.sin_addr.s_addr)continue;
            const int one=1;::setsockopt(client,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));return s;}
        require(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR,"surfxr_accept_failed");pollfd p{fd_,POLLIN,0};::poll(&p,1,20);
    }
    cancelled(cancel);fail("surfxr_accept_timeout");
}
void Socket::connect_udp(const std::string& host,unsigned port){if(adapter_)return;const auto a=address(host,port);require(::connect(fd_,reinterpret_cast<const sockaddr*>(&a),sizeof(a))==0,"surfxr_udp_connect_failed");}
Socket Socket::connect_tcp(const std::string& host,unsigned port,const std::string& local,
        Clock::time_point deadline,const std::function<bool()>& cancel){
    Socket result(::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0));require(result.fd_>=0,"surfxr_socket_failed");
    const int one=1,buffer=256*1024;
    ::setsockopt(result.fd_,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    ::setsockopt(result.fd_,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer));
    const auto source=address(local,0),destination=address(host,port);
    require(::bind(result.fd_,reinterpret_cast<const sockaddr*>(&source),sizeof(source))==0,"surfxr_bind_failed");
    cancelled(cancel);
    if(::connect(result.fd_,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))==0)return result;
    require(errno==EINPROGRESS,"surfxr_connect_failed");
    while(Clock::now()<deadline){
        cancelled(cancel);pollfd event{result.fd_,POLLOUT,0};const auto ready=::poll(&event,1,20);
        if(ready<0&&errno==EINTR)continue;
        require(ready>=0,"surfxr_connect_failed");
        if(!ready)continue;
        int error=0;socklen_t length=sizeof(error);
        require(::getsockopt(result.fd_,SOL_SOCKET,SO_ERROR,&error,&length)==0&&!error,"surfxr_connect_failed");
        return result;
    }
    cancelled(cancel);fail("surfxr_connect_timeout");
}
void Socket::send(const Bytes& b,bool tcp,const std::function<bool()>& cancel){
    if(adapter_){cancelled(cancel);adapter_->send(b);return;}
    const auto deadline=Clock::now()+std::chrono::milliseconds(250);std::size_t offset=0;
    while(offset<b.size()){
        cancelled(cancel);const auto n=::send(fd_,b.data()+offset,b.size()-offset,MSG_NOSIGNAL);
        if(n>0){require(tcp||static_cast<std::size_t>(n)==b.size(),"surfxr_partial_datagram");offset+=static_cast<std::size_t>(n);continue;}
        require(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR),"surfxr_send_failed");
        require(Clock::now()<deadline,"surfxr_send_timeout");pollfd p{fd_,POLLOUT,0};::poll(&p,1,5);
    }
}
void Socket::send_parts(const std::uint8_t* prefix,std::size_t prefix_size,
        const std::uint8_t* payload,std::size_t payload_size,const std::function<bool()>& cancel){
    require(!adapter_&&prefix&&prefix_size&&payload&&payload_size&&
        prefix_size<=65507&&payload_size<=65507-prefix_size,"surfxr_invalid_outbound_packet");
    iovec vectors[2]{{const_cast<std::uint8_t*>(prefix),prefix_size},
        {const_cast<std::uint8_t*>(payload),payload_size}};
    msghdr message{};message.msg_iov=vectors;message.msg_iovlen=2;
    const auto deadline=Clock::now()+std::chrono::milliseconds(250);
    for(;;){
        cancelled(cancel);const auto n=::sendmsg(fd_,&message,MSG_NOSIGNAL);
        if(n>0){require(static_cast<std::size_t>(n)==prefix_size+payload_size,"surfxr_partial_datagram");return;}
        require(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR),"surfxr_send_failed");
        require(Clock::now()<deadline,"surfxr_send_timeout");pollfd p{fd_,POLLOUT,0};::poll(&p,1,5);
    }
}
std::size_t Socket::read(std::uint8_t* p,std::size_t size,bool tcp){
    if(adapter_)return adapter_->read(p,size);
    const auto n=::recv(fd_,p,size,tcp?0:MSG_TRUNC);
    if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return 0;fail("surfxr_receive_failed");}
    if(n==0){require(!tcp,"surfxr_peer_closed");return 0;}
    require(static_cast<std::size_t>(n)<=size,"surfxr_oversized_datagram");return static_cast<std::size_t>(n);
}
unsigned Socket::read_datagrams(std::uint8_t* data,std::size_t stride,unsigned count,
        const std::function<void(const std::uint8_t*,std::size_t)>& sink){
    require(data&&stride>=64&&stride<=65536&&count&&count<=32,"surfxr_invalid_datagram_batch");
    if(adapter_)return adapter_->read_datagrams(data,stride,count,sink);
    std::array<mmsghdr,32> messages{};std::array<iovec,32> vectors{};
    for(unsigned i=0;i<count;++i){vectors[i]={data+i*stride,stride};messages[i].msg_hdr.msg_iov=&vectors[i];messages[i].msg_hdr.msg_iovlen=1;}
    const auto n=::recvmmsg(fd_,messages.data(),count,MSG_DONTWAIT|MSG_TRUNC,nullptr);
    if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)return 0;fail("surfxr_receive_failed");}
    for(int i=0;i<n;++i){require(messages[i].msg_len<=stride,"surfxr_oversized_datagram");
        if(messages[i].msg_len)sink(data+static_cast<std::size_t>(i)*stride,messages[i].msg_len);}
    return static_cast<unsigned>(n);
}
void Connection::send_control(const Bytes& body){
    require(!body.empty()&&body.size()<=2*1024*1024,"surfxr_invalid_control_size");
    Bytes frame;if(modern){Writer w;w.integer(body.size(),4);frame=std::move(w.bytes);}else big(frame,static_cast<std::uint32_t>(body.size()),4);
    frame.insert(frame.end(),body.begin(),body.end());
    std::lock_guard<std::mutex> lock(control_mutex_);control_socket.send(frame,true,cancelled);
}
void Connection::send_stream(unsigned stream,const Bytes& payload){
    require(stream<indices_.size(),"surfxr_invalid_stream");
    const bool priority=priority_socket.fd()>=0&&stream!=3;
    std::lock_guard<std::mutex> lock(priority?priority_mutex_:stream_mutex_);
    auto& socket=priority?priority_socket:stream_socket;
    if(modern&&tcp){
        require(!payload.empty()&&payload.size()<=8*1024*1024,"surfxr_invalid_outbound_packet");
        Writer w;w.integer(stream,2);w.integer(indices_[stream]++,4);w.integer(payload.size(),4);w.bytes.insert(w.bytes.end(),payload.begin(),payload.end());socket.send(w.bytes,true,cancelled);
    }else for(const auto& b:shards(stream,indices_[stream]++,payload,packet_size,modern))socket.send(b,tcp,cancelled);
}
Bytes receive_control(Socket& socket,Framer& framer,Clock::time_point deadline,const std::function<bool()>& cancel){
    Bytes result;std::array<std::uint8_t,65536> byte{};
    // Read exactly one record. Bytes of the next handshake stage stay in the
    // socket; partial prefixes and payloads survive every nonblocking read.
    while(result.empty()){
        cancelled(cancel);require(Clock::now()<deadline,"surfxr_handshake_timeout");
        const auto n=socket.read(byte.data(),std::min(byte.size(),framer.needed()),true);
        if(n)framer.feed(byte.data(),n,[&](Bytes b){result=std::move(b);});
        else{pollfd p{socket.fd(),POLLIN,0};::poll(&p,1,5);}
    }
    return result;
}
}
