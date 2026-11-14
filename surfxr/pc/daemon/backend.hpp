#pragma once
#include "../desktop/network.hpp"
#include "desktop_module.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
namespace surfxr::daemon {
using surfxr::transport::Bytes;
class Worker;
class Backend {
public:
    using Send=std::function<bool(const Bytes&)>;
    using VideoSend=std::function<bool(const std::uint8_t*,std::size_t)>;
    Backend(bool desktop,unsigned generation,Send control,VideoSend video);
    ~Backend();
    void desktop(DesktopRun,unsigned fps,unsigned bitrate,bool input,std::function<unsigned(unsigned)> monitors,std::function<bool()> idle,std::function<bool()> prefer_pc={});
    void vr(Bytes caps,Worker&);
    void tracking(Bytes caps);
    bool push(Bytes);
    bool uplink(const std::uint8_t*,std::size_t);
    bool uplink(const Bytes& bytes){return uplink(bytes.data(),bytes.size());}
    int receive(std::uint8_t*,std::size_t);
    bool readable(unsigned);
    bool stopped()const{return stop_.load();}
    unsigned generation()const{return generation_;}
    bool is_desktop()const{return desktop_;}
    bool finished()const{return finished_.load();}
    unsigned reason()const{return reason_.load();}
    void stop();
private:
    void run_vr(Bytes,Worker&);
    void run_tracking(Bytes);
    bool desktop_;unsigned generation_;Send control_;VideoSend video_;
    surfxr::transport::desktop::Handle controls_ready_,stop_ready_;
    std::atomic_bool stop_{false},finished_{false};std::thread thread_;
    std::atomic_uint reason_{0};
    std::mutex mutex_;std::condition_variable ready_;std::deque<Bytes> controls_;
    std::size_t queued_bytes_=0;
    SOCKET uplink_=INVALID_SOCKET,control_socket_=INVALID_SOCKET;
};
}
