#pragma once
#include "windows.hpp"
#include "surfxr/surfxr_desktop_crypto.hpp"
#include <chrono>
#include <mutex>
#include <optional>

namespace surfxr::transport::desktop {
using Clock=std::chrono::steady_clock;
class Socket {
public:
    SOCKET value=INVALID_SOCKET;
    std::function<bool(unsigned)> poll;
    explicit Socket(SOCKET s=INVALID_SOCKET):value(s){}~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
    Socket(Socket&& s)noexcept:value(s.value),poll(std::move(s.poll)){s.value=INVALID_SOCKET;}
    Socket(const Socket&)=delete;
    bool ready(unsigned milliseconds)const;
    void send_all(const Bytes&)const;
    void read_all(std::uint8_t*,std::size_t)const;
    static Socket bind(const std::string&,unsigned,bool tcp,unsigned receive_buffer_bytes=0);
};
class Control {
public:
    Socket socket;
    std::unique_ptr<Cipher> incoming,outgoing;
    std::function<void(const Bytes&)> write;
    std::function<Bytes()> read;
    explicit Control(Socket s):socket(std::move(s)){}
    void send(const Bytes&);
    Bytes receive();
    std::optional<Bytes> poll_receive();
    bool flush_send();
    void queue_send(const Bytes&);
private:
    std::mutex send_mutex_;
    Bytes pending_;
    Bytes pending_send_;std::size_t send_offset_=0;
};
void authenticate(Control&,const Key&,Key& server,Key& client);
bool verify_capabilities(const Bytes&,bool* keyboard=nullptr,bool* candidate_click=nullptr,bool* desktop_keyboard=nullptr);
void answer_discovery(Socket&,const Options&);
}
