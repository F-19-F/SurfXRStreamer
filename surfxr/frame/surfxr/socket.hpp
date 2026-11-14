#pragma once
#include "stream.hpp"
#include "wire.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>

namespace surfxr::transport {
using Clock=std::chrono::steady_clock;
std::uint64_t now_ns();
struct Cancelled {};
class SocketAdapter {
public:
    virtual ~SocketAdapter()=default;
    virtual int fd()const=0;
    virtual bool readable()const=0;
    virtual std::size_t read(std::uint8_t*,std::size_t)=0;
    // The sink borrows a complete datagram only until it returns. Adapters
    // may hand it their owned queue entry without copying into data.
    virtual unsigned read_datagrams(std::uint8_t* data,std::size_t stride,unsigned count,
                                   const std::function<void(const std::uint8_t*,std::size_t)>& sink){
        unsigned used=0;for(;used<count;++used){const auto n=read(data+used*stride,stride);if(!n)break;sink(data+used*stride,n);}return used;
    }
    virtual void send(const Bytes&)=0;
};
class Socket {
public:
    Socket()=default;
    explicit Socket(int fd):fd_(fd){}
    explicit Socket(std::shared_ptr<SocketAdapter> adapter):adapter_(std::move(adapter)){}
    ~Socket();
    Socket(Socket&& other)noexcept:fd_(other.fd_),adapter_(std::move(other.adapter_)){other.fd_=-1;}
    Socket& operator=(Socket&&)noexcept;
    Socket(const Socket&)=delete;
    static Socket bind(const std::string& address,unsigned port,bool tcp,
                       unsigned receive_buffer_bytes=0);
    Socket accept(const std::string& host,Clock::time_point deadline,const std::function<bool()>& cancelled)const;
    static Socket connect_tcp(const std::string& host,unsigned port,const std::string& local,
                              Clock::time_point deadline,const std::function<bool()>& cancelled);
    void connect_udp(const std::string& host,unsigned port);
    void send(const Bytes&,bool tcp,const std::function<bool()>& cancelled);
    // Atomically send borrowed UDP segments; neither segment outlives this
    // call. Unified plaintext prefixes can avoid copying the shard payload.
    void send_parts(const std::uint8_t*,std::size_t,const std::uint8_t*,std::size_t,
                    const std::function<bool()>& cancelled);
    std::size_t read(std::uint8_t*,std::size_t,bool tcp);
    unsigned read_datagrams(std::uint8_t*,std::size_t stride,unsigned count,
                            const std::function<void(const std::uint8_t*,std::size_t)>&);
    int fd()const{return adapter_?adapter_->fd():fd_;}
    bool readable()const;
private:
    int fd_=-1;
    std::shared_ptr<SocketAdapter> adapter_;
};
class Connection {
public:
    Socket control_socket,stream_socket,priority_socket;
    bool tcp=false,modern=false;unsigned packet_size=1400;
    std::function<bool()> cancelled;
    void send_control(const Bytes&);
    void send_stream(unsigned stream,const Bytes&);
private:
    std::mutex control_mutex_,stream_mutex_,priority_mutex_;
    std::array<std::uint32_t,5> indices_{};
};
Bytes receive_control(Socket&,Framer&,Clock::time_point,const std::function<bool()>&);
}
