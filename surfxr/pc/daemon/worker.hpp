#pragma once
#include "../desktop/windows.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
namespace surfxr::daemon {
using WorkerRun=int (*)(std::uint64_t,std::uint64_t,std::uint64_t);
using WorkerSend=bool (*)(void*,const std::uint8_t*,std::size_t);
using WorkerRunV7=int (*)(std::uint64_t,std::uint64_t,void*,WorkerSend);
using VideoSend=std::function<bool(const std::uint8_t*,std::size_t)>;
class Worker {
public:
    explicit Worker(WorkerRun,std::wstring pipe={},std::wstring image=L"vrserver.exe");
    explicit Worker(WorkerRunV7,std::wstring pipe={},std::wstring image=L"vrserver.exe");
    ~Worker();
    Worker(const Worker&)=delete;
    bool idle()const{return !busy_.load();}
    void destination(unsigned port);
    void destination(VideoSend);
    bool wait_idle(unsigned milliseconds);
    unsigned sender_port()const{return sender_port_.load();}
private:
    void loop(WorkerRun,WorkerRunV7);
    struct SendGate;
    surfxr::transport::desktop::Handle stop_event_,retire_event_;
    std::mutex destination_mutex_;std::condition_variable idle_ready_;VideoSend direct_;std::shared_ptr<SendGate> gate_;
    std::wstring pipe_,image_;std::atomic_bool stop_{false},busy_{false};std::atomic_uint destination_{0},sender_port_{0},retirement_{0};std::thread thread_;
};
}
