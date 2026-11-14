#pragma once
#include "surfxr/surfxr_desktop.hpp"
#include <optional>

namespace surfxr::transport::desktop {
inline constexpr unsigned capture_type=15,capture_ack_type=16;
struct CaptureRequest {unsigned generation=0,serial=0,mask=15;};
inline Bytes capture_record(CaptureRequest value,bool ack=false){
    require(value.generation&&value.serial&&value.mask<=15,"desktop_capture_value");
    Writer w(true);w.integer(ack?capture_ack_type:capture_type,4);w.integer(1,4);
    w.integer(value.generation,4);w.integer(value.serial,4);w.integer(value.mask,4);return w.bytes;
}
inline CaptureRequest capture_record(const Bytes& bytes,bool ack=false){
    Reader r(bytes,true);require(r.integer(4)==(ack?capture_ack_type:capture_type)&&r.integer(4)==1,"desktop_capture_version");
    CaptureRequest value;value.generation=static_cast<unsigned>(r.integer(4));value.serial=static_cast<unsigned>(r.integer(4));value.mask=static_cast<unsigned>(r.integer(4));r.end();
    require(value.generation&&value.serial&&value.mask<=15,"desktop_capture_value");return value;
}
// One request in flight; fast UI changes coalesce without an unbounded queue.
class CaptureSync {
    unsigned serial_=0,generation_=0,accepted_=16;
    std::optional<CaptureRequest> pending_;
public:
    std::optional<CaptureRequest> request(unsigned generation,unsigned mask){
        require(mask<=15,"desktop_capture_mask");if(!generation)return {};
        if(generation!=generation_){generation_=generation;accepted_=16;pending_.reset();}
        if(pending_||accepted_==mask)return {};
        require(serial_!=UINT32_MAX,"desktop_capture_serial_exhausted");
        pending_=CaptureRequest{generation,++serial_,mask};return pending_;
    }
    bool acknowledge(CaptureRequest ack){
        if(!pending_||ack.generation!=pending_->generation||ack.serial!=pending_->serial||ack.mask!=pending_->mask)return false;
        accepted_=ack.mask;pending_.reset();return true;
    }
};
}
