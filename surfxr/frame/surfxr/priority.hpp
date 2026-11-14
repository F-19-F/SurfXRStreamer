#pragma once
#include "socket.hpp"
#include "surfxr/session.hpp"
#include "surfxr/surfxr_feedback.hpp"
#include <exception>
#include <thread>

namespace surfxr::transport {
// Owns the small-packet reader. Join before sockets/callbacks are released.
class PriorityReceiver {
public:
    PriorityReceiver(Connection&,const SessionOptions&,const SessionCallbacks&,bool game_audio,bool video_haptics,
                     std::atomic<std::uint64_t>& audio_bytes,ReceiveCounters&);
    ~PriorityReceiver();
    void check();
private:
    std::atomic_bool stop_{false};
    std::mutex mutex_;std::exception_ptr error_;
    const int wake_fd_;
    std::thread thread_;
};
}
