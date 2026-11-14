#pragma once
#include "start.hpp"
#include "surfxr/surfxr_modes.hpp"
#include <unistd.h>

namespace surfxr::transport {
// The existing startup protocol runs beside desktop ingestion, never on XR
// or the decoder thread. Only matching ready requests retire the old source.
class ModeStartup {
public:
    ModeStartup(const SessionOptions& options,const SessionCallbacks& callbacks,
                std::shared_ptr<Modes> modes,std::function<bool()> cancelled)
        :options_(options),callbacks_(callbacks),modes_(std::move(modes)),cancelled_(std::move(cancelled)),
         worker_([this]{run();}){}
    ~ModeStartup(){close();}
    void close(){done_.store(true);if(worker_.joinable())worker_.join();}
private:
    const SessionOptions& options_;const SessionCallbacks& callbacks_;
    std::shared_ptr<Modes> modes_;std::function<bool()> cancelled_;
    std::atomic_bool done_{false};std::thread worker_;
    void run(){
        while(!done_.load()){
            // begin() clears the media cancel bit. Keep publishing owner
            // cancellation until joined, even if it races that reset.
            if(cancelled_()){
                modes_->cancel.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;
            }
            const auto serial=modes_->preparation();
            if(!serial){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
            const auto cancelled=[&]{return done_.load()||cancelled_()||modes_->preparation()!=serial;};
            SessionCallbacks progress;
            progress.steamvr_startup=[&](const auto& status){
                if(!cancelled()&&callbacks_.steamvr_startup)callbacks_.steamvr_startup(status);
            };
            std::unique_ptr<SteamVrStartupMonitor> monitor;
            try{
                std::array<char,64> hostname{};
                require(::gethostname(hostname.data(),hostname.size()-1)==0,"surfxr_client_hostname_failed");
                for(auto& c:hostname)if(c&&!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                        (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))c='-';
                const auto launched=request_steamvr(options_,hostname.data(),cancelled);
                if(cancelled())throw Cancelled{};
                monitor=std::make_unique<SteamVrStartupMonitor>(options_,hostname.data(),progress,!launched);
                monitor->wait_ready(cancelled);
                monitor->close();
                if(!cancelled())modes_->prepared(serial,true);
            }catch(const Cancelled&){}
            catch(const std::exception&){
                auto status=monitor?monitor->snapshot():SteamVrStartupStatus{};
                if(monitor)monitor->close();
                if(!cancelled()){
                    if(status.state!=SteamVrStartupState::Failed)
                        status={SteamVrStartupState::Failed,SteamVrStartupReason::LauncherFailed,0};
                    if(callbacks_.steamvr_startup)callbacks_.steamvr_startup(status);
                    modes_->prepared(serial,false);
                }
            }
        }
    }
};
}
