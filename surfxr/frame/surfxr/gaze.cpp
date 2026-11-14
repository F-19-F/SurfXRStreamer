#include "wire.hpp"
#include <cmath>

namespace surfxr::transport {
Bytes gaze_feedback(const GazeSample* sample,std::uint64_t now,std::uint64_t epoch){
    Writer w(true);
    const bool fresh=sample&&sample->origin_epoch==epoch&&sample->sampled_monotonic_ns&&
        sample->sampled_monotonic_ns<=now&&now-sample->sampled_monotonic_ns<100'000'000;
    w.boolean(fresh);
    if(fresh){
        w.integer(sample->sampled_monotonic_ns,8);
        w.integer(sample->origin_epoch,8);
        for(unsigned eye=0;eye<2;++eye){const auto p=sample->positions[eye];
            const bool valid=sample->valid[eye]&&std::isfinite(p[0])&&std::isfinite(p[1])&&
                p[0]>=-.5f&&p[0]<=.5f&&p[1]>=-.5f&&p[1]<=.5f;
            w.boolean(valid);w.real(valid?p[0]:0);w.real(valid?p[1]:0);
        }
    }
    return std::move(w.bytes);
}
}
