#include "surfxr/battery.hpp"
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace surfxr {
namespace {
constexpr std::uint64_t hmd_ttl_ns=30'000'000'000ull,controller_ttl_ns=5'000'000'000ull;
bool fresh(std::uint64_t stamp,std::uint64_t now,std::uint64_t ttl) noexcept {
    return stamp&&now>=stamp&&now-stamp<=ttl;
}
std::string text_file(const std::filesystem::path& path){
    std::ifstream stream(path,std::ios::binary);if(!stream)return {};
    char bytes[65]{};stream.read(bytes,sizeof(bytes));const auto size=stream.gcount();
    if(size<=0||size>64||stream.bad())return {};
    std::string value(bytes,static_cast<std::size_t>(size));
    const auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");
    return first==std::string::npos?std::string{}:value.substr(first,last-first+1);
}
DeviceBattery supply_battery(const std::filesystem::path& path){
    const auto capacity=text_file(path/"capacity");
    if(capacity.empty()||capacity.size()>3)return {};
    int percent=0;for(char c:capacity){if(c<'0'||c>'9')return {};percent=percent*10+c-'0';}
    if(percent>100)return {};
    DeviceBattery result;result.percent=percent;
    const auto status=text_file(path/"status");
    if(status=="Charging"){result.charging_known=true;result.charging=true;}
    else if(status=="Discharging"||status=="Not charging"||status=="Full")result.charging_known=true;
    return result;
}
}
std::uint64_t battery_monotonic_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
DeviceBattery sanitize_battery(DeviceBattery value) noexcept {
    if(value.percent<0||value.percent>100)return {};
    if(!value.charging_known)value.charging=false;
    return value;
}
DeviceBattery battery_from_xr_state(bool profile_active,std::uint64_t flags,float level) noexcept {
    constexpr std::uint64_t valid=1,charging=2,no_battery=8;
    if(!profile_active||!(flags&valid)||(flags&no_battery)||!std::isfinite(level)||level<0||level>1)return {};
    return {static_cast<int>(std::lround(static_cast<double>(level)*100)),(flags&charging)!=0,true};
}
DeviceBattery read_headset_battery(const std::string& root) noexcept {
    try{
        if(root.empty()||root.size()>4096)return {};
        std::error_code error;std::filesystem::directory_iterator it(root,error),end;if(error)return {};
        unsigned entries=0,systems=0,unspecified=0;DeviceBattery system,other;
        for(;it!=end;it.increment(error)){
            if(error||++entries>64)return {};
            const auto path=it->path();if(text_file(path/"type")!="Battery")continue;
            const auto scope=text_file(path/"scope"),present=text_file(path/"present");
            if((!scope.empty()&&scope!="System")||(!present.empty()&&present!="1"))continue;
            // A controller's scope=Device is excluded. For this Frame's
            // unscoped max1720x battery, require a single unambiguous candidate.
            if(scope=="System"){++systems;system=supply_battery(path);}
            else{++unspecified;other=supply_battery(path);}
        }
        if(error)return {};
        if(systems)return systems==1?system:DeviceBattery{};
        return unspecified==1?other:DeviceBattery{};
    }catch(...){return {};}
}
BatterySnapshot fresh_battery_snapshot(BatterySnapshot value,std::uint64_t now_ns) noexcept {
    value.hmd=fresh(value.hmd_sampled_monotonic_ns,now_ns,hmd_ttl_ns)?sanitize_battery(value.hmd):DeviceBattery{};
    if(!fresh(value.controllers_sampled_monotonic_ns,now_ns,controller_ttl_ns))value.controllers={};
    else for(auto& hand:value.controllers)hand=sanitize_battery(hand);
    return value;
}
struct BatteryMonitor::Impl {
    std::string root;std::chrono::milliseconds interval;
    mutable std::mutex mutex;std::condition_variable wake;std::once_flag stopped;
    BatterySnapshot state;bool stopping=false;std::thread worker;
    DeviceBattery runtime_hmd;std::uint64_t runtime_hmd_sampled_ns=0;
    bool sync_raw_battery=false;
    Impl(std::string path,std::chrono::milliseconds period):root(std::move(path)),interval(period){
        if(interval<std::chrono::milliseconds(100)||interval>std::chrono::seconds(30))throw std::invalid_argument("battery_poll_interval_out_of_range");
        worker=std::thread([this]{run();});
    }
    void run(){
        for(;;){
            // All filesystem activity stays outside the shared snapshot lock.
            const auto value=read_headset_battery(root);const auto now=battery_monotonic_ns();
            std::unique_lock<std::mutex> lock(mutex);if(stopping)return;
            state.hmd=value;state.hmd_sampled_monotonic_ns=now;
            if(wake.wait_for(lock,interval,[&]{return stopping;}))return;
        }
    }
    void stop(){std::call_once(stopped,[this]{
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;}
        wake.notify_all();if(worker.joinable())worker.join();
    });}
    ~Impl(){stop();}
};
BatteryMonitor::BatteryMonitor(std::string root,std::chrono::milliseconds interval):impl_(std::make_unique<Impl>(std::move(root),interval)){}
BatteryMonitor::~BatteryMonitor()=default;
void BatteryMonitor::set_sync_raw_battery(bool enabled){
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->sync_raw_battery=enabled;
}
DeviceBattery BatteryMonitor::raw_headset(std::uint64_t now_ns)const {
    if(!now_ns)now_ns=battery_monotonic_ns();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return fresh_battery_snapshot(impl_->state,now_ns).hmd;
}
void BatteryMonitor::publish_headset_runtime(DeviceBattery value,std::uint64_t sampled_ns){
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->runtime_hmd=sanitize_battery(value);impl_->runtime_hmd_sampled_ns=sampled_ns;
}
void BatteryMonitor::publish_controllers(const std::array<DeviceBattery,2>& values,std::uint64_t now_ns){
    if(!now_ns)now_ns=battery_monotonic_ns();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state.controllers={sanitize_battery(values[0]),sanitize_battery(values[1])};
    impl_->state.controllers_sampled_monotonic_ns=now_ns;
}
BatterySnapshot BatteryMonitor::snapshot(std::uint64_t now_ns)const {
    if(!now_ns)now_ns=battery_monotonic_ns();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result=fresh_battery_snapshot(impl_->state,now_ns);
    if(!impl_->sync_raw_battery&&impl_->runtime_hmd.percent>=0&&fresh(impl_->runtime_hmd_sampled_ns,now_ns,controller_ttl_ns)){
        result.hmd=impl_->runtime_hmd;result.hmd_sampled_monotonic_ns=impl_->runtime_hmd_sampled_ns;
    }
    return result;
}
void BatteryMonitor::stop(){impl_->stop();}
} // namespace surfxr
