#pragma once
#include "surfxr/surfxr_desktop_capture.hpp"
namespace surfxr::transport {
inline void desktop_capture_checks(){
    using namespace desktop;
    for(unsigned mask:{0u,1u,2u,15u}){
        CaptureRequest value{7,9,mask};const auto wire=capture_record(value);
        const auto decoded=capture_record(wire);require(decoded.generation==7&&decoded.serial==9&&decoded.mask==mask,"capture_roundtrip");
        const auto rejects=[](const Bytes& bytes){bool rejected=false;try{capture_record(bytes);}catch(...){rejected=true;}require(rejected,"capture_bad_record_accepted");};
        auto bad=wire;bad.push_back(0);rejects(bad);bad=wire;bad.pop_back();rejects(bad);
        for(unsigned offset:{0u,1u,2u,3u,4u}){bad=wire;bad[offset]=offset==4?16:0;rejects(bad);}
        rejects(capture_record(value,true));
    }
    CaptureSync sync;require(!sync.request(0,15),"capture_before_layout");
    auto first=sync.request(1,0);require(first&&first->mask==0,"capture_all_off");
    require(!sync.request(1,2),"capture_one_inflight");
    require(!sync.acknowledge({1,first->serial+1,0})&&!sync.acknowledge({1,first->serial,15}),"capture_unmatched_ack");
    require(sync.acknowledge(*first),"capture_accept_off");
    auto second=sync.request(1,2);require(second&&second->serial>first->serial&&second->mask==2,"capture_coalesced_resume_secondary");
    auto third=sync.request(2,15);require(third&&third->generation==2,"capture_layout_restarts_request");
    require(!sync.acknowledge(*second)&&sync.acknowledge(*third)&&!sync.request(2,15),"capture_stale_layout_ack");
}
}
