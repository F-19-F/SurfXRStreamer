#include "presentation_pacing.hpp"
#include "refresh_rate.hpp"
#include "rotational_timewarp.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

int main(){
    using surfxr::xr_detail::VrPresentationPacer;
    const auto check=[](bool value){if(!value)throw std::runtime_error("presentation_pacing_test_failed");};
    {surfxr::RemoteSettingsSnapshot s;auto& c=s.capabilities;
        check(surfxr::xr_detail::display_refresh_target(nullptr)==0);
        c.vr_only=true;c.server_video_controls=false;c.vr_requested=true;c.connected=true;
        s.values[static_cast<unsigned>(surfxr::SettingId::VrFramerate)].requested=144;
        check(surfxr::xr_detail::display_refresh_target(&s)==0);
        c.vr_stream_framerate=120;check(surfxr::xr_detail::display_refresh_target(&s)==120);
        c.vr_stream_framerate=90;check(surfxr::xr_detail::display_refresh_target(&s)==90);
        c.connected=false;check(surfxr::xr_detail::display_refresh_target(&s)==0);
        c.connected=true;c.vr_requested=false;check(surfxr::xr_detail::display_refresh_target(&s)==0);
        c.vr_requested=true;c.vr_only=false;c.server_video_controls=true;
        check(surfxr::xr_detail::display_refresh_target(&s)==144);
        c.server_video_controls=false;check(surfxr::xr_detail::display_refresh_target(&s)==0);
    }
    {surfxr::xr_detail::RotationalTimewarp warp;
        const std::array<float,4> source{0,std::sin(.3f),0,std::cos(.3f)},target{0,std::sin(.5f),0,std::cos(.5f)},fov{1,.9f,1.2f,.8f};
        check(surfxr::xr_detail::rotational_timewarp(source,target,fov,warp));
        check(std::abs(warp.rotation[1]-std::sin(.2f))<1e-6f&&std::abs(warp.rotation[3]-std::cos(.2f))<1e-6f);
        check(warp.fov==fov&&std::abs(warp.orientation[1]-target[1])<1e-6f);
        check(surfxr::xr_detail::rotational_timewarp(source,source,fov,warp));
        check(std::abs(warp.rotation[1])<1e-6f&&std::abs(warp.rotation[3]-1)<1e-6f);
        check(!surfxr::xr_detail::rotational_timewarp(source,{0,0,0,0},fov,warp)&&warp.fov[0]==0);
        check(!surfxr::xr_detail::rotational_timewarp(source,target,{1,1,0,1},warp));
        check(!surfxr::xr_detail::rotational_timewarp(source,{0,0,0,std::numeric_limits<float>::quiet_NaN()},fov,warp));
    }
    constexpr std::uint64_t start=1'000'000'000;
    constexpr std::int64_t period=8'333'333;
    VrPresentationPacer pacing;
    const auto deadline=pacing.deadline(start,start+300'000,start+27'000'000,period);
    check(deadline>start+300'000&&deadline<start+period);
    check(pacing.deadline(start,start+300'000,start+27'000'000,period,true)==0);
    const auto pre=pacing.before_wait_deadline(start,start+1'000'000,start+27'000'000,period);
    check(pre==start+period-500'000);
    check(pacing.before_wait_deadline(start,pre,start+27'000'000,period)==0);
    check(pacing.before_wait_deadline(start,start-1,start+27'000'000,period)==0);
    check(pacing.before_wait_deadline(start,start+1'000'000,start+27'000'000,period,true)==0);
    check(VrPresentationPacer::pacing_period(2*period,120)<=period);
    check(VrPresentationPacer::pacing_period(2*period,90)==11'111'111);
    check(VrPresentationPacer::pacing_period(period,0)==period);
    check(VrPresentationPacer::pacing_period(period,std::numeric_limits<double>::quiet_NaN())==period);
    check(VrPresentationPacer::pacing_period(-1,120)==0);
    check(pacing.deadline(start,deadline,start+27'000'000,period)==0);
    // Display and interval headroom are separate constraints. A near display
    // target must never be treated as the usual multi-interval prediction.
    check(pacing.deadline(start,start+300'000,start+3'000'000,period)==0);
    check(pacing.deadline(start,start-1,start+27'000'000,period)==0);
    for(const auto invalid:{std::int64_t{-1},std::int64_t{0},std::int64_t{1'000'000},std::int64_t{51'000'000}})
        check(pacing.deadline(start,start,start+27'000'000,invalid)==0);
    pacing.set_enabled(false);check(pacing.deadline(start,start,start+27'000'000,period)==0);
    check(pacing.before_wait_deadline(start,start+1'000'000,start+27'000'000,period)==0);
    pacing.set_enabled(true);
    pacing.observe_cpu(20'000'000);
    check(pacing.deadline(start,start,start+27'000'000,period)==0);
    check(pacing.before_wait_deadline(start,start+1'000'000,start+27'000'000,period)==0);
    for(unsigned i=0;i<200;++i){pacing.observe_cpu(400'000);pacing.observe_gpu(1.2,0);}
    check(pacing.deadline(start,start,start+27'000'000,period)>start);
    const auto recovered=pacing.deadline(start,start,start+27'000'000,period);
    pacing.observe_gpu(std::numeric_limits<double>::quiet_NaN(),0);
    check(pacing.deadline(start,start,start+27'000'000,period)==recovered);
    pacing.observe_gpu(8,0);check(pacing.deadline(start,start,start+27'000'000,period)==0);
    // High refresh rates cannot inherit a wait budget from a slower mode.
    check(pacing.deadline(start,start,start+27'000'000,2'000'000)==0);
    std::cout<<"presentation-pacing: deadline, load, invalid-clock and override checks passed\n";
}
