#include "boundary_upload.hpp"
#include "../../../frame/xr/boundary_sampling.hpp"
#include "../../../frame/xr/boundary_settling.hpp"
#include "../../../frame/xr/ipd_sampling.hpp"
#include <limits>

using namespace surfxr;
using namespace surfxr::transport;
void boundary_checks(){
    xr_detail::IpdSampling ipd(10'000'000'000);
    require(ipd.due(1),"IPD first connection samples without waiting for an event");
    ipd.sampled(.065f,1);
    for(std::uint64_t i=1;i<10000;++i)
        require(!ipd.due(i*1'000'000),"steady tracking does not resample IPD per frame");
    require(ipd.due(2,true)&&ipd.value()==.065f,"runtime event invalidates read deadline without changing cache");
    ipd.sampled(.0652f,2);
    require(!ipd.due(10'000'000'001)&&ipd.due(10'000'000'002),"missed event watchdog uses last actual read");
    for(float value:{0.f,-1.f,.2f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        ipd.sampled(value,3);require(ipd.value()==0,"bad native IPD permits bounded XR fallback");
    }
    ipd.sampled(.064f,4);require(ipd.value()==.064f,"native IPD recovers after invalid read");
    TrackingFrame frame;frame.origin_epoch=7;
    auto boundary=std::make_shared<StageBoundary>();boundary->origin_epoch=7;boundary->width=2;boundary->depth=3;
    boundary->quads.assign(3,{1,0,2,1,2,2,3,2,4,3,0,4});frame.boundary=boundary;
    const auto packet=frame_boundary(frame);Reader outer(packet,true);
    require(outer.integer(4)==11,"boundary uses negotiated ReservedBuffer");
    const auto bytes=outer.blob();outer.end();Reader r(bytes);
    require(bytes.size()==172&&r.integer(4)==0x56444244&&r.integer(4)==1&&r.integer(8)==7,"boundary fixed little endian header");
    require(r.real()==2&&r.real()==3&&r.integer(4)==3,"boundary play area and wall count");
    for(unsigned i=0;i<3;++i)for(float v:{1.f,0.f,2.f,1.f,2.f,2.f,3.f,2.f,4.f,3.f,0.f,4.f})
        require(r.real()==v,"boundary Rust fixture coordinates preserved");
    r.end();
    BoundaryUpload upload;
    require(upload.update(frame,1)&&!upload.update(frame,2),"boundary first upload and tracking rate deduplication");
    frame.boundary=std::make_shared<StageBoundary>(*boundary);
    require(!upload.update(frame,3),"identical resampling does not send");
    require(!upload.update(frame,5'000'000'001)&&!upload.update(frame,3'600'000'000'000),"stable boundary never retries on a timer");
    auto changed=std::make_shared<StageBoundary>(*boundary);changed->quads[0][0]=1.5f;frame.boundary=changed;
    require(upload.update(frame,5'000'000'002),"geometry update sends without recenter");
    frame.origin_epoch=8;
    require(frame_boundary(frame).empty()&&!upload.update(frame,5'000'000'003),"old epoch cannot publish boundary");
    changed=std::make_shared<StageBoundary>(*changed);changed->origin_epoch=8;frame.boundary=changed;
    require(!upload.update(frame,5'000'000'004),"epoch alone does not rewrite unchanged room geometry");
    frame.boundary_sync_enabled=false;
    require(frame_boundary(frame).empty()&&!upload.update(frame,5'000'000'005),"disabled boundary never uploads cached geometry");
    frame.boundary_sync_enabled=true;
    require(!upload.update(frame,5'000'000'006),"reenable preserves duplicate suppression on the same reliable connection");
    frame.boundary.reset();require(!upload.update(frame,5'000'000'007),"tracking gap leaves last sent geometry intact");
    frame.boundary=changed;require(!upload.update(frame,5'000'000'008),"tracking recovery does not rewrite the same room");
    for(unsigned i=1;i<=9;++i){auto jittered=std::make_shared<StageBoundary>(*changed);jittered->quads[0][0]+=.02f*i;frame.boundary=jittered;
        require(!upload.update(frame,10'000'000'000+i),"small boundary corrections are ignored, never deferred");}
    require(!upload.update(frame,3'600'000'000'000),"elapsed time cannot turn a small change into a write");
    auto corrected=std::make_shared<StageBoundary>(*changed);corrected->quads[0][0]+=.25f;frame.boundary=corrected;
    require(upload.update(frame,11'000'000'000),"large cumulative change compares to last sent geometry");
    BoundaryUpload reconnected;require(reconnected.update(frame,12'000'000'000),"new reliable connection sends current geometry once");
    xr_detail::BoundarySampling sampling;
    require(!xr_detail::boundary_time_ready(99,100,0)&&xr_detail::boundary_time_ready(100,100,0)&&
        !xr_detail::boundary_time_ready(100,100,200)&&
        !xr_detail::boundary_time_ready(200,100,200)&&!xr_detail::boundary_time_ready(0,0,0),"sample only in current runtime epoch, never predicted future Stage");
    sampling.update(8,true);const auto old=sampling.generation();require(sampling.accepts(old),"initial active subscription");
    require(!sampling.update(8,true),"steady tracking does not request new samples");
    sampling.update(8,false);require(!sampling.accepts(old),"tracking loss discards queued response");
    sampling.update(8,true);require(!sampling.accepts(old)&&sampling.accepts(sampling.generation()),"same epoch recovery rejects pre-loss response");
    const auto recovered=sampling.generation();sampling.update(9,true);
    require(!sampling.accepts(recovered),"Stage change rejects old in-flight subscription");
    std::array<float,12> origin{},jitter{};jitter[3]=5e-7f;jitter[0]=5e-7f;
    require(!xr_detail::boundary_origin_changed(origin,jitter),"cache flush float noise does not resample walls");
    jitter[3]=2e-4f;require(xr_detail::boundary_origin_changed(origin,jitter),"cumulative translation correction triggers sampling");
    jitter={};jitter[1]=2e-5f;require(xr_detail::boundary_origin_changed(origin,jitter),"rotation correction triggers sampling");
    xr_detail::BoundarySettling settling;
    xr_detail::BoundarySnapshot sample;sample.token=3;sample.width=2;sample.depth=3;sample.count=3;
    for(unsigned i=0;i<3;++i)sample.quads[i]=boundary->quads[i];
    settling.request(1);require(settling.due(1),"new event requests sample");
    require(!settling.observe(sample,origin,7,1)&&!settling.due(99'000'000),"first plausible read waits for another tick");
    require(settling.due(100'000'001),"settling retry due");
    auto moved=sample;moved.quads[0][0]+=4;
    require(!settling.observe(moved,origin,7,100'000'001),"transient geometry cannot publish");
    require(!settling.observe(moved,jitter,7,200'000'001),"changing origin invalidates candidate");
    require(!settling.observe(moved,jitter,8,300'000'001),"changing universe invalidates candidate");
    require(settling.observe(moved,jitter,8,400'000'001)&&!settling.due(20'000'000'000),"stable sample publishes then stops geometry reads");
    settling.request(500'000'001);sample.count=0;
    for(std::int64_t i=0;i<10;++i)require(!settling.observe(sample,origin,7,500'000'001+i*100'000'000),"unavailable calibration remains pending");
    require(!settling.due(1'500'000'001)&&settling.due(2'400'000'001),"persistent failures back off to one second");
    sample.count=3;
    require(!settling.observe(sample,origin,7,2'400'000'001)&&settling.observe(sample,origin,7,3'400'000'001),"recovery after more than three failures needs no new event");
    settling.request(4'000'000'000);settling.cancel();require(!settling.due(20'000'000'000),"unsubscribe cancels pending retries");
    for(float value:{0.f,-1.f,101.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
        auto bad=std::make_shared<StageBoundary>(*changed);bad->width=value;frame.boundary=bad;
        require(frame_boundary(frame).empty(),"invalid dimensions do not overwrite PC boundary");
    }
    for(unsigned count:{0u,2u,65u}){auto bad=std::make_shared<StageBoundary>(*changed);bad->quads.resize(count);frame.boundary=bad;
        require(frame_boundary(frame).empty(),"boundary count bounded without truncation");}
    auto bad=std::make_shared<StageBoundary>(*changed);bad->quads[0][0]=std::numeric_limits<float>::quiet_NaN();frame.boundary=bad;
    require(frame_boundary(frame).empty(),"nonfinite collision data rejected");
    frame.boundary.reset();require(!upload.update(frame,20'000'000'000),"unavailable source keeps PC boundary");
}
