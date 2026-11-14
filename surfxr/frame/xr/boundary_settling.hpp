#pragma once
#include "boundary_sampling.hpp"
#include "steamvr_boundary_ipc.hpp"

namespace surfxr::xr_detail {
// Event-triggered reads settle across two distinct worker ticks. Failed reads
// keep a bounded retry alive; stable geometry is never polled on the timer.
class BoundarySettling {
public:
    void request(std::int64_t now){
        candidate_={};attempts_=0;pending_=true;next_=now;
    }
    void cancel(){pending_=false;candidate_={};}
    bool due(std::int64_t now)const{return pending_&&now>=next_;}
    bool observe(const BoundarySnapshot& value,const std::array<float,12>& origin,
                 std::uint64_t universe,std::int64_t now){
        ++attempts_;next_=now+(attempts_<10?100'000'000:1'000'000'000);
        StageBoundary check;check.width=value.width;check.depth=value.depth;
        if(value.count<=StageBoundary::max_quads)
            check.quads.assign(value.quads.begin(),value.quads.begin()+value.count);
        if(!valid_stage_boundary(check)){candidate_={};return false;}
        bool same=candidate_.count==value.count&&candidate_.token==value.token&&universe_==universe&&
            !boundary_origin_changed(origin_,origin)&&
            std::abs(candidate_.width-value.width)<1e-4f&&std::abs(candidate_.depth-value.depth)<1e-4f;
        if(same)for(unsigned q=0;q<value.count;++q)for(unsigned p=0;p<12;++p)
            same&=std::abs(candidate_.quads[q][p]-value.quads[q][p])<1e-4f;
        if(same){pending_=false;candidate_={};return true;}
        candidate_=value;origin_=origin;universe_=universe;return false;
    }
private:
    BoundarySnapshot candidate_{};
    std::array<float,12> origin_{};
    std::uint64_t universe_=0;
    std::int64_t next_=0;
    unsigned attempts_=0;
    bool pending_=false;
};
}
