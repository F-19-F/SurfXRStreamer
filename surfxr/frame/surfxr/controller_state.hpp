#pragma once
#include "wire.hpp"

namespace surfxr::transport {
// Reliable initial state and edges, independent of head tracking/UDP upload.
// The monotonic timestamp is also the PC's barrier against pre-enable poses.
class ControllerStateUpload {
public:
    const Bytes* update(bool enabled,std::uint64_t now){
        if(sent_&&enabled==enabled_)return nullptr;
        Writer body;body.integer(0x56444353,4);body.integer(1,4);
        body.integer(now,8);body.integer(enabled,4);
        Writer control(true);control.integer(11,4);control.blob(body.bytes);
        bytes_=std::move(control.bytes);sent_=true;enabled_=enabled;return &bytes_;
    }
private:
    bool sent_=false,enabled_=false;Bytes bytes_;
};
}
