#pragma once
#include "socket.hpp"
#include "surfxr/session.hpp"
#include <thread>

namespace surfxr::transport {
// Optional pose-only SURFXR connection. Never requests SteamVR startup or owns
// media/decoder callbacks; the desktop session remains usable without it.
class DesktopTracking {
public:
    DesktopTracking(const SessionOptions&,const SessionCallbacks&,const std::atomic_bool*);
    ~DesktopTracking();
private:
    const SessionOptions& options_;const SessionCallbacks& callbacks_;const std::atomic_bool* stop_;
    std::atomic_bool stopping_{false};std::thread worker_;
    bool cancelled()const;
    void run();
};
}
