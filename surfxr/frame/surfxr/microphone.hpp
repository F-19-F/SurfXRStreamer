#pragma once
#include "socket.hpp"
#include "surfxr/session.hpp"
#include <thread>

namespace surfxr::transport {
struct MicrophoneUploadStats {
    std::uint64_t start_attempts=0,capture_failures=0,packets=0;
};
// One owner for capture callbacks. Video reception cannot delay mono upload or
// recovery. The session joins this owner before closing sockets/callback state.
class MicrophoneUpload {
public:
    MicrophoneUpload(Connection&,const SessionOptions&,const SessionCallbacks&,MicrophoneUploadStats&);
    ~MicrophoneUpload();
    void check();
private:
    Connection& connection_;const SessionOptions& options_;const SessionCallbacks& callbacks_;
    MicrophoneUploadStats& stats_;
    std::atomic_bool stopping_{false};std::mutex error_mutex_;std::exception_ptr error_;std::thread worker_;
    bool cancelled() const;
    void run();
};
}
