#pragma once
#include "surfxr/vr_frame.hpp"

namespace surfxr {
// Timing starts at the locally uploaded tracking ID in monotonic nanoseconds.
// The separate capture/image timestamp identifies new images, not pose age.
// These stages include PC work and transport together; no remote clock or
// guessed Game/Encoding/Networking split enters this read-only meter.
class SurfxrPerformance {
public:
    struct Snapshot {
        bool available=false,fps_valid=false;
        double total_ms=0,upstream_ms=0,decode_ms=0,after_decode_ms=0,fps=0;
    };
    bool presented(const VrStereoFrame& pair,std::uint64_t now) noexcept {
        if(!pair.metadata||(!pair.metadata->linear_layout&&!pair.metadata->gaze_atlas))return false;
        const auto stamp=pair.metadata->timestamp,received=pair.metadata->header_received_monotonic_ns;
        const auto tracking=pair.metadata->surfxr_tracking_timestamp?pair.metadata->surfxr_tracking_timestamp:stamp;
        if(!stamp||!received||received<tracking||now<received||now-tracking>10'000'000'000ull)return false;
        if(epoch_!=pair.origin_epoch){*this={};epoch_=pair.origin_epoch;}
        if(stamp<=last_stamp_)return false; // Reprojection is not a new frame.
        std::uint64_t output=0;
        const auto include=[&](const auto& frames){for(const auto& frame:frames){
            if(!frame||frame->decoder_output_monotonic_ns<received||frame->decoder_output_monotonic_ns>now)return false;
            output=std::max(output,frame->decoder_output_monotonic_ns);
        }return true;};
        if(!include(pair.eyes)||(pair.foveated&&!include(pair.focus)))return false;
        if(!last_at_||now-last_at_>2'000'000'000ull){value_={};window_=now;frames_=0;}
        const auto smooth=[&](double& value,std::uint64_t ns){const double sample=double(ns)/1e6;
            value=value_.available?value*.95+sample*.05:sample;};
        smooth(value_.total_ms,now-tracking);smooth(value_.upstream_ms,received-tracking);
        smooth(value_.decode_ms,output-received);smooth(value_.after_decode_ms,now-output);
        value_.available=true;last_at_=now;last_stamp_=stamp;++frames_;
        if(now-window_>=1'000'000'000ull){value_.fps=double(frames_-1)*1e9/double(now-window_);
            value_.fps_valid=true;window_=now;frames_=1;}
        return true;
    }
    Snapshot snapshot(std::uint64_t now) const noexcept {
        return last_at_&&now>=last_at_&&now-last_at_<=2'000'000'000ull?value_:Snapshot{};
    }
private:
    Snapshot value_;
    std::uint64_t last_stamp_=0,last_at_=0,epoch_=0,window_=0,frames_=0;
};
} // namespace surfxr
