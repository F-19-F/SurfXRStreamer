#include "media_lease.hpp"
#include <thread>
#include <future>
namespace surfxr::transport::desktop {
void media_lease_self_test(){
    // Unique process-owned kernel object. Never touch the live media lease.
    const auto name=L"Local\\SURFXRFrameMediaTest-"+std::to_wstring(GetCurrentProcessId());
    bool blocked=false;
    {
        MediaLease desktop(name.c_str(),100);
        std::thread vr([&]{try{MediaLease lease(name.c_str(),20);}catch(const std::exception&){blocked=true;}});vr.join();
        require(blocked,"desktop_media_lease_allowed_concurrent_vr");
    }
    bool acquired=false;
    std::thread vr([&]{try{MediaLease lease(name.c_str(),100);acquired=true;}catch(...){}});vr.join();
    require(acquired,"desktop_media_lease_not_released");
    Handle cancelled(CreateEventW(nullptr,TRUE,FALSE,nullptr));require(cancelled.value,"desktop_media_lease_cancel_event");
    {
        MediaLease desktop(name.c_str(),100);
        auto pending=std::async(std::launch::async,[&]{try{MediaLease lease(name.c_str(),8000,{},cancelled.value);return false;}catch(...){return true;}});
        SetEvent(cancelled.value);
        require(pending.wait_for(std::chrono::milliseconds(250))==std::future_status::ready&&pending.get(),"desktop_media_lease_event_cancel_timeout");
    }
    // Cancellation after semaphore admission must return its ownership before
    // throwing; otherwise the next desktop source can never be admitted.
    bool rejected=false;unsigned checks=0;
    try{MediaLease lease(name.c_str(),100,[&]{return ++checks>1;},cancelled.value);}catch(...){rejected=true;}
    require(rejected,"desktop_media_lease_cancelled_admission");
    MediaLease following(name.c_str(),100);
}
}
