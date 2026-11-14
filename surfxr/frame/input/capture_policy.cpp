#include "surfxr/system_input.hpp"

namespace surfxr::frame_input {
void CapturePolicy::leave() noexcept {
    if(active_){++exit_serial_;for(auto& h:hands_){h.swallow=h.click;h.taps=0;} }
    active_=false;disarmed_=requested_;
}
void CapturePolicy::request(bool capture,std::uint64_t now) noexcept {
    heartbeat_=now;requested_=capture;
    if(!capture){leave();disarmed_=false;return;}
    if(!disarmed_&&available_==3&&touch_available_==3&&!hands_[0].click&&!hands_[1].click&&!active_){
        active_=true;for(auto& h:hands_){h.taps=0;h.first_tap_ns=0;}
    }
}
bool CapturePolicy::update(unsigned hand,bool touch,bool down,std::uint64_t now) noexcept {
    if(hand>=2)return down;
    if(active_&&(!heartbeat_||now<heartbeat_||now-heartbeat_>lease_ns))leave();
    auto& h=hands_[hand];
    if(touch)h.touch=down;
    else if(h.click!=down){
        if(down)h.down_ns=now;
        else{
            h.swallow=false;
            if(active_){
                const bool click=now>=h.down_ns&&now-h.down_ns>=30'000'000&&now-h.down_ns<=500'000'000;
                if(!click||now<h.first_tap_ns||now-h.first_tap_ns>2'000'000'000)h.taps=0;
                if(click){if(!h.taps)h.first_tap_ns=now;if(++h.taps==4)leave();}
            }
        }
        h.click=down;if(!down)h.swallow=false;
    }
    return forwarded(hand,touch);
}
void CapturePolicy::advance(std::uint64_t now,unsigned available,unsigned touch_available) noexcept {
    available&=3;touch_available&=available;
    if(available_!=available){leave();for(unsigned i=0;i<2;++i)if(!(available&(1u<<i))){
        const bool swallow=hands_[i].swallow;hands_[i]={};hands_[i].swallow=swallow;
    }}
    available_=available;touch_available_=touch_available;
    if(active_&&(!heartbeat_||now<heartbeat_||now-heartbeat_>lease_ns))leave();
}
Snapshot CapturePolicy::snapshot(std::uint64_t now) const noexcept {
    Snapshot result;result.sampled_ns=now;result.exit_serial=exit_serial_;
    result.available=available_;result.touch_available=touch_available_;result.capture=active_;
    for(unsigned i=0;i<2;++i){if(hands_[i].click)result.clicks|=1u<<i;if(hands_[i].touch)result.touches|=1u<<i;}
    return result;
}
bool CapturePolicy::forwarded(unsigned hand,bool touch) const noexcept {
    if(hand>=2||active_)return false;
    return touch?hands_[hand].touch:(!hands_[hand].swallow&&hands_[hand].click);
}
void CapturePolicy::stop() noexcept {leave();requested_=false;disarmed_=false;heartbeat_=0;}
}
