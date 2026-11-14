#pragma once
#include "socket.hpp"
#include "surfxr/session.hpp"
#include <condition_variable>
#include <thread>
namespace surfxr::transport {
// Explicit user connection only. A retry after an established stream never relaunches PC VR.
bool request_steamvr(const SessionOptions&,const std::string& hostname,
                     const std::function<bool()>& cancelled);
// Startup-only observer. Queries never launch SteamVR and never block video
// ingestion; legacy peers without this capability keep their existing path.
class SteamVrStartupMonitor {
public:
    SteamVrStartupMonitor(const SessionOptions&,std::string,const SessionCallbacks&,bool retry_idle=false);
    ~SteamVrStartupMonitor();
    void wait_ready(const std::function<bool()>&);
    bool pending()const;
    void check()const;
    void driver_ready();void video_ready();void close();
    SteamVrStartupStatus snapshot()const;
private:
    void run();void publish(SteamVrStartupStatus);
    const SessionOptions& options_;const SessionCallbacks& callbacks_;std::string hostname_;
    mutable std::mutex mutex_;std::condition_variable ready_;
    SteamVrStartupStatus status_{SteamVrStartupState::Launching};
    bool first_=false,supported_=false,retry_idle_=false,idle_retried_=false;std::atomic_bool stop_{false};std::thread worker_;
};
}
