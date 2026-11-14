#include "start.hpp"
#include <cstring>
#include <poll.h>
#include <sys/random.h>

namespace surfxr::transport {
namespace {
std::optional<SteamVrStartupStatus> query(const SessionOptions& options,const std::string& hostname,
        const std::atomic_bool& stop,bool bootstrap){
    Bytes request{'A','L','V','R','F','Q','0','1'};request.resize(16);
    require(::getrandom(request.data()+8,8,0)==8,"surfxr_start_nonce_failed");
    request.push_back(static_cast<std::uint8_t>(hostname.size()));request.insert(request.end(),hostname.begin(),hostname.end());
    auto socket=Socket::bind(options.local_address,0,false);socket.connect_udp(options.host,options.surfxr_control_port);
    const auto cancelled=[&]{return stop.load();};socket.send(request,false,cancelled);
    std::array<std::uint8_t,128> bytes{};const auto deadline=Clock::now()+std::chrono::milliseconds(bootstrap?500:40);
    while(!stop.load()&&Clock::now()<deadline){
        const auto count=socket.read(bytes.data(),bytes.size(),false);
        if(count==27&&!std::memcmp(bytes.data(),"ALVRPQ01",8)&&!std::memcmp(bytes.data()+8,request.data()+8,8)&&
                bytes[16]==1&&bytes[17]<=3&&bytes[18]<=4&&(bytes[17]==3?bytes[18]!=0:bytes[18]==0)){
            Reader r(bytes.data()+19,8);const auto phase=bytes[17];
            return SteamVrStartupStatus{phase==3?SteamVrStartupState::Failed:static_cast<SteamVrStartupState>(phase),
                static_cast<SteamVrStartupReason>(bytes[18]),r.integer(8)};
        }
        pollfd event{socket.fd(),POLLIN,0};::poll(&event,1,10);
    }
    return {};
}
}
SteamVrStartupMonitor::SteamVrStartupMonitor(const SessionOptions& options,std::string hostname,const SessionCallbacks& callbacks,bool retry_idle):
    options_(options),callbacks_(callbacks),hostname_(std::move(hostname)),retry_idle_(retry_idle){
    if(callbacks_.steamvr_startup)callbacks_.steamvr_startup(status_);
    worker_=std::thread([this]{
        try{run();}catch(...){
            {std::lock_guard<std::mutex> lock(mutex_);status_={SteamVrStartupState::Failed,SteamVrStartupReason::SessionFailed,status_.request_id};first_=supported_=true;}
            ready_.notify_all();
        }
    });
}
SteamVrStartupMonitor::~SteamVrStartupMonitor(){close();}
void SteamVrStartupMonitor::close(){video_ready();if(worker_.joinable())worker_.join();}
void SteamVrStartupMonitor::video_ready(){stop_.store(true);ready_.notify_all();}
void SteamVrStartupMonitor::publish(SteamVrStartupStatus value){
    bool changed=false;
    {std::lock_guard<std::mutex> lock(mutex_);changed=status_.state!=value.state||status_.reason!=value.reason||status_.request_id!=value.request_id;status_=value;}
    ready_.notify_all();if(changed&&callbacks_.steamvr_startup)callbacks_.steamvr_startup(value);
}
void SteamVrStartupMonitor::run(){
    const auto safety=Clock::now()+std::chrono::seconds(150);
    while(!stop_.load()){
        std::optional<SteamVrStartupStatus> value;
        bool bootstrap=false;{std::lock_guard<std::mutex> lock(mutex_);bootstrap=!first_;}
        try{value=query(options_,hostname_,stop_,bootstrap);}catch(const Cancelled&){break;}catch(...){/* Older/missing status responders use legacy establishment. */}
        if(stop_.load())break;
        {std::lock_guard<std::mutex> lock(mutex_);first_=true;if(value)supported_=true;}
        if(value)publish(*value);else ready_.notify_all();
        if(value&&value->state==SteamVrStartupState::NotStarted){
            if(retry_idle_&&!idle_retried_){
                idle_retried_=true;
                try{if(request_steamvr(options_,hostname_,[&]{return stop_.load();}))publish({SteamVrStartupState::Launching,SteamVrStartupReason::None,value->request_id});}
                catch(const Cancelled&){break;}
                catch(const std::runtime_error& error){publish({SteamVrStartupState::Failed,std::strcmp(error.what(),"surfxr_pc_untrusted")==0?SteamVrStartupReason::Untrusted:SteamVrStartupReason::LauncherFailed,value->request_id});}
            }else publish({SteamVrStartupState::Failed,SteamVrStartupReason::LauncherFailed,value->request_id});
        }
        if(pending()&&Clock::now()>=safety)publish({SteamVrStartupState::Failed,SteamVrStartupReason::DriverNotReady,snapshot().request_id});
        std::unique_lock<std::mutex> lock(mutex_);ready_.wait_for(lock,std::chrono::milliseconds(500),[&]{return stop_.load();});
    }
}
SteamVrStartupStatus SteamVrStartupMonitor::snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return status_;}
bool SteamVrStartupMonitor::pending()const{
    std::lock_guard<std::mutex> lock(mutex_);return supported_&&(status_.state==SteamVrStartupState::NotStarted||status_.state==SteamVrStartupState::Launching);
}
void SteamVrStartupMonitor::check()const{require(snapshot().state!=SteamVrStartupState::Failed,"surfxr_steamvr_start_failed");}
void SteamVrStartupMonitor::wait_ready(const std::function<bool()>& cancelled){
    for(;;){
        if(cancelled())throw Cancelled{};
        check();
        std::unique_lock<std::mutex> lock(mutex_);
        if(first_&&(!supported_||status_.state==SteamVrStartupState::RuntimeReady))return;
        ready_.wait_for(lock,std::chrono::milliseconds(20));
    }
}
void SteamVrStartupMonitor::driver_ready(){
    bool legacy=false;{std::lock_guard<std::mutex> lock(mutex_);legacy=!supported_;}
    if(legacy)publish({SteamVrStartupState::RuntimeReady,SteamVrStartupReason::None,0});
}
}
