#pragma once
#include "wire.hpp"
#include <cmath>

namespace surfxr::transport {
// Called by the existing upload owner. Immutable snapshots avoid rebuilding
// collision packets at the tracking rate. The reliable connection delivers a
// change once; driver readiness/retries belong to the PC commit owner.
class BoundaryUpload {
public:
    const Bytes* update(const TrackingFrame& frame,std::uint64_t){
        if(!frame.boundary_sync_enabled||!frame.boundary)return nullptr;
        if(source_==frame.boundary&&epoch_==frame.origin_epoch)return nullptr;
        source_=frame.boundary;epoch_=frame.origin_epoch;
        auto bytes=frame_boundary(frame);
        if(bytes.empty())return nullptr;
        // Compare with the last SENT geometry, not the latest noisy read, so
        // cumulative real changes still propagate. Epoch alone is not geometry.
        if(sent_&&same(*sent_,*frame.boundary))return nullptr;
        sent_=frame.boundary;last_=std::move(bytes);
        return &last_;
    }
private:
    static bool same(const StageBoundary& a,const StageBoundary& b){
        if(a.quads.size()!=b.quads.size()||std::abs(a.width-b.width)>=.2f||std::abs(a.depth-b.depth)>=.2f)return false;
        for(unsigned q=0;q<a.quads.size();++q)for(unsigned p=0;p<12;++p)
            if(std::abs(a.quads[q][p]-b.quads[q][p])>=.2f)return false;
        return true;
    }
    std::shared_ptr<const StageBoundary> source_;
    std::shared_ptr<const StageBoundary> sent_;
    std::uint64_t epoch_=0;
    Bytes last_;
};
}
