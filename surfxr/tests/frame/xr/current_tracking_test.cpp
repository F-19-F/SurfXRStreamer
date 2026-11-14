#define XR_USE_TIMESPEC
#include <time.h>
#include "current_tracking.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace surfxr;
namespace {
std::vector<std::pair<XrSpace,XrTime>> queries;
bool head_valid=true,velocity_valid=true,convert_valid=true;
void check(bool v,const char* message){if(!v)throw std::runtime_error(message);}
XrSpace handle(unsigned n){return reinterpret_cast<XrSpace>(std::uintptr_t(n));}
XrResult XRAPI_PTR convert(XrInstance,const timespec* t,XrTime* out){
    *out=t->tv_sec*1'000'000'000ll+t->tv_nsec;return convert_valid?XR_SUCCESS:XR_ERROR_TIME_INVALID;
}
XrResult XRAPI_PTR locate(XrSpace space,XrSpace stage,XrTime time,XrSpaceLocation* out){
    check(stage==handle(9),"all poses use Stage");queries.emplace_back(space,time);
    out->locationFlags=(space==handle(1)&&!head_valid)?0:15;
    out->pose.orientation={0,0,0,1};out->pose.position={float(time)*1e-9f,2,3};
    auto* velocity=static_cast<XrSpaceVelocity*>(out->next);
    velocity->velocityFlags=velocity_valid?XR_SPACE_VELOCITY_LINEAR_VALID_BIT|XR_SPACE_VELOCITY_ANGULAR_VALID_BIT:0;
    velocity->linearVelocity={1,2,3};velocity->angularVelocity={4,5,6};return XR_SUCCESS;
}
}
int main(){try{
    xr_detail::CurrentTracking sampler;TrackingFrame cached,out;
    cached.optics_valid=true;cached.origin_epoch=1;cached.origin=TrackingOrigin::Stage;
    cached.sampled_monotonic_ns=1'000'000'000;cached.ipd_meters=.064f;
    cached.stage_supported=cached.focused=cached.visible=true;
    for(auto& hand:cached.hands){hand.grip_active=true;hand.connection_known=hand.connected=true;hand.buttons=ButtonA;}
    auto publish=[&](XrTime begin,XrTime change){sampler.publish(XR_NULL_HANDLE,convert,locate,handle(9),handle(1),{handle(2),handle(3)},cached,begin,change);};
    publish(900'000'000,0);
    for(unsigned n=0;n<4;++n){
        const auto ns=1'001'000'000ull+n*2'000'000;const timespec time{1,long(ns-1'000'000'000)};
        check(sampler.sample(out,time,ns),"fresh upload sample");
        check(out.sequence==n+1&&out.sampled_monotonic_ns==ns&&out.query_xr_time==std::int64_t(ns)&&out.predicted_xr_time==out.query_xr_time,"one present-time query identity");
        check(out.prediction_lead_seconds==0&&out.head.angular_velocity.z==6&&out.hands[1].grip.linear_velocity.y==2,"unpredicted pose with both velocities");
        for(unsigned d=0;d<3;++d)check(queries[n*3+d].second==std::int64_t(ns),"head and grips share one sample time");
        check(out.hands[0].buttons==ButtonA,"actions retain XR thread state");
        check(std::abs(out.eye_poses[1].position.x-out.eye_poses[0].position.x-.064f)<1e-6,"eyes follow new head pose");
    }
    check(queries.size()==12,"four upload ticks cause twelve fresh locates");
    head_valid=false;check(!sampler.sample(out,{1,9'000'000},1'009'000'000),"lost head not resent");head_valid=true;
    cached.hands[0].connected=false;publish(900'000'000,0);velocity_valid=false;
    check(sampler.sample(out,{1,10'000'000},1'010'000'000)&&!out.hands[0].grip_active&&!out.head.angular_velocity_valid,"disconnected hand and invalid velocity cleared");
    publish(900'000'000,1'011'000'000);
    check(!sampler.sample(out,{1,11'000'000},1'011'000'000),"pending Stage change gates uploads");
    cached.origin_epoch=2;publish(1'011'000'000,0);
    check(!sampler.sample(out,{1,10'000'000},1'012'000'000),"old Stage time rejected");
    check(sampler.sample(out,{1,12'000'000},1'012'000'000)&&out.origin_epoch==2,"new Stage epoch sampled");
    check(!sampler.sample(out,{1,101'000'000},1'101'000'000),"stale action snapshot expires");
    convert_valid=false;check(!sampler.sample(out,{1,13'000'000},1'013'000'000),"failed time conversion");
    sampler.clear();convert_valid=true;check(!sampler.sample(out,{1,13'000'000},1'013'000'000),"shutdown barrier prevents runtime access");
    std::cout<<"Fresh head/controller sampling checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
