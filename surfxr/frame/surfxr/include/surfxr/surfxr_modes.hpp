#pragma once
#include <atomic>
#include <mutex>
#include <cstdint>

namespace surfxr::transport {
// Unified desktop keeps receiving while the requested VR runtime starts.
class Modes {
public:
    explicit Modes(bool desktop):desired_desktop_(desktop),active_desktop_(desktop){}
    bool request(bool desktop){
        std::lock_guard<std::mutex> lock(mutex_);
        if(desired_desktop_!=desktop){desired_desktop_=desktop;++request_serial_;}
        if(desktop!=active_desktop_&&!(prepare_vr_&&active_desktop_&&!desktop))cancel.store(true);
        return true;
    }
    void prepare_vr(bool enabled){std::lock_guard<std::mutex> lock(mutex_);prepare_vr_=enabled;}
    bool desired_desktop()const{std::lock_guard<std::mutex> lock(mutex_);return desired_desktop_;}
    std::uint64_t preparation()const{
        std::lock_guard<std::mutex> lock(mutex_);
        return prepare_vr_&&active_desktop_&&!desired_desktop_&&!cancel.load()?request_serial_:0;
    }
    void prepared(std::uint64_t serial,bool ready){
        std::lock_guard<std::mutex> lock(mutex_);
        if(serial!=request_serial_||!active_desktop_||desired_desktop_||cancel.load())return;
        if(ready){prepared_serial_=serial;cancel.store(true);}else{desired_desktop_=true;++request_serial_;}
    }
    bool take_prepared(){
        std::lock_guard<std::mutex> lock(mutex_);
        const bool ready=!active_desktop_&&!desired_desktop_&&prepared_serial_==request_serial_;
        prepared_serial_=0;return ready;
    }
    bool begin(){
        std::lock_guard<std::mutex> lock(mutex_);
        active_desktop_=desired_desktop_;cancel.store(false);return active_desktop_;
    }
    bool fallback_to_desktop(){
        std::lock_guard<std::mutex> lock(mutex_);if(cancel.load())return false;
        desired_desktop_=true;++request_serial_;if(!active_desktop_)cancel.store(true);return true;
    }
    bool pending()const{std::lock_guard<std::mutex> lock(mutex_);return cancel.load();}
    bool desktop()const{std::lock_guard<std::mutex> lock(mutex_);return active_desktop_;}
    std::atomic_bool cancel{false};
private:
    mutable std::mutex mutex_;
    bool desired_desktop_,active_desktop_,prepare_vr_=false;
    std::uint64_t request_serial_=1,prepared_serial_=0;
};
}
