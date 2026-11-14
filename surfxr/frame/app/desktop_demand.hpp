#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace surfxr::viewer {
// XR owns transitions; the media owner only reads per-stream epoch tokens.
// Resume never validates queued input or output from an earlier interval.
class DesktopFrameDemand {
    std::array<std::atomic<std::uint64_t>,4> epochs_{};
    std::uint64_t serial_=0;unsigned mask_=0;
    const bool enabled_;
public:
    std::atomic_uint capture_requested{15},capture_accepted{15};
    std::atomic_bool capture_supported{false};
    explicit DesktopFrameDemand(bool enabled=true):enabled_(enabled){}
    bool enabled()const{return enabled_;}
    void set_mask(unsigned mask){
        if(mask&~15u)throw std::invalid_argument("desktop_demand_mask");
        if(!enabled_)return;
        for(unsigned id=0;id<epochs_.size();++id)if((mask^mask_)&(1u<<id)){
            std::uint64_t epoch=0;
            if(mask&(1u<<id)){if(++serial_==0)throw std::overflow_error("desktop_demand_epoch_exhausted");epoch=serial_;}
            epochs_[id].store(epoch,std::memory_order_release);
        }
        mask_=mask;
    }
    std::uint64_t epoch(unsigned id)const{return epochs_.at(id).load(std::memory_order_acquire);}
    const std::atomic<std::uint64_t>* token(unsigned id)const{return enabled_?&epochs_.at(id):nullptr;}
};
}
