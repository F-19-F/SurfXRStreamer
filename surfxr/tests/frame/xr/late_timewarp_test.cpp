#include "late_timewarp.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Views=std::array<XrCompositionLayerProjectionView,2>;
using Vector=std::array<double,3>;
std::array<XrQuaternionf,2> target{};
XrResult result=XR_SUCCESS;
XrViewStateFlags flags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
std::uint32_t returned_count=2,queries=0;
constexpr XrTime display_time=1'020'000'000;
template<class T>T handle(unsigned n){return reinterpret_cast<T>(std::uintptr_t(n));}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
XrQuaternionf rotation(double x,double y,double z){
    const auto length=std::sqrt(x*x+y*y+z*z),scale=length?std::sin(length/2)/length:0.;
    return {float(x*scale),float(y*scale),float(z*scale),float(std::cos(length/2))};
}
// Independent double-precision rotation matrix reference, not the production
// quaternion product or the shader's cross-product rotation implementation.
Vector rotate(XrQuaternionf q,Vector v){
    const double x=q.x,y=q.y,z=q.z,w=q.w;
    return {(1-2*(y*y+z*z))*v[0]+2*(x*y-z*w)*v[1]+2*(x*z+y*w)*v[2],
        2*(x*y+z*w)*v[0]+(1-2*(x*x+z*z))*v[1]+2*(y*z-x*w)*v[2],
        2*(x*z-y*w)*v[0]+2*(y*z+x*w)*v[1]+(1-2*(x*x+y*y))*v[2]};
}
double distance(Vector a,Vector b){double n=0;for(unsigned i=0;i<3;++i)n+=(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(n);}
XrResult XRAPI_PTR locate(XrSession session,const XrViewLocateInfo* info,XrViewState* state,
        std::uint32_t capacity,std::uint32_t* count,XrView* views){
    check(session==handle<XrSession>(1)&&info->space==handle<XrSpace>(2),"wrong runtime or Stage");
    check(info->displayTime==display_time&&info->viewConfigurationType==XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        "must use this frame's display target without another prediction offset");
    check(capacity==2&&views[0].type==XR_TYPE_VIEW&&views[1].type==XR_TYPE_VIEW,"invalid stereo query");
    ++queries;state->viewStateFlags=flags;*count=returned_count;
    for(unsigned eye=0;eye<2;++eye){views[eye].pose.orientation=target[eye];
        views[eye].pose.position={9,8,7};views[eye].fov={-.4f,.4f,.4f,-.4f};}
    return result;
}
Views source_views(unsigned frame){
    Views views{};
    for(unsigned eye=0;eye<2;++eye){auto& view=views[eye];
        view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view.pose.orientation=rotation(.12,-.19+.01*frame,eye?.04:-.03);
        view.pose.position={eye?.032f:-.032f,1.7f,.2f};
        view.fov=eye?XrFovf{-.7f,.9f,.8f,-.6f}:XrFovf{-.9f,.7f,.6f,-.8f};
        view.subImage.swapchain=handle<XrSwapchain>(eye+3);view.subImage.imageRect.extent={256,256};}
    return views;
}
}

int main(){try{
    using surfxr::xr_detail::RotationalTimewarp;
    using surfxr::xr_detail::late_projection_timewarp;
    std::array<RotationalTimewarp,2> transforms{};
    double negative_error=0;unsigned checked=0;
    for(unsigned frame=0;frame<8;++frame){
        const auto source=source_views(frame);
        // New image, then the same retained image with a newer runtime estimate
        // for the same display time: neither may reuse a previous warped pose.
        for(unsigned repeat=0;repeat<2;++repeat){auto views=source;
            for(unsigned eye=0;eye<2;++eye)target[eye]=rotation(.15-.05*repeat,
                (frame%2?.22:-.25)+.03*repeat,eye?.07:-.04);
            check(late_projection_timewarp(handle<XrSession>(1),handle<XrSpace>(2),display_time,locate,views,transforms),"valid late warp rejected");
            for(unsigned eye=0;eye<2;++eye){const auto& warp=transforms[eye];
                check(std::memcmp(&views[eye].pose.position,&source[eye].pose.position,sizeof(XrVector3f))==0,"invented translation without depth");
                check(std::memcmp(&views[eye].fov,&source[eye].fov,sizeof(XrFovf))==0,"source FOV changed");
                check(std::memcmp(&views[eye].subImage,&source[eye].subImage,sizeof(XrSwapchainSubImage))==0,"eye image association changed");
                const XrQuaternionf correction{warp.rotation[0],warp.rotation[1],warp.rotation[2],warp.rotation[3]};
                for(Vector ray:{Vector{.3,.1,-1},Vector{-.2,-.4,-1},Vector{.1,.2,-1}}){
                    const auto actual=rotate(source[eye].pose.orientation,rotate(correction,ray));
                    const auto expected=rotate(target[eye],ray);
                    check(distance(actual,expected)<1e-6,"world ray moved after late correction");
                    check(distance(rotate(views[eye].pose.orientation,ray),expected)<1e-6,"submitted pose would apply the correction twice");
                    negative_error=std::max(negative_error,distance(rotate(source[eye].pose.orientation,ray),expected));++checked;}
                check(std::abs(warp.fov[0]+std::tan(source[eye].fov.angleLeft))<1e-6,"asymmetric source FOV lost");
            }
        }
    }
    check(queries==16&&negative_error>.3,"negative control did not exercise stale source orientation");
    const auto source=source_views(0);
    for(unsigned failure=0;failure<7;++failure){auto views=source;
        target={rotation(0,.2,0),rotation(.1,-.3,.1)};flags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        result=XR_SUCCESS;returned_count=2;
        if(failure==0)result=XR_ERROR_TIME_INVALID;
        if(failure==1)flags=0;
        if(failure==2)returned_count=1;
        if(failure==3)target[1]={0,0,0,0};
        if(failure==4)target[1].x=std::numeric_limits<float>::quiet_NaN();
        if(failure==5)result=XR_SESSION_LOSS_PENDING;
        const auto before=queries;
        check(!late_projection_timewarp(handle<XrSession>(1),handle<XrSpace>(2),failure==6?0:display_time,locate,views,transforms),"invalid target accepted");
        check(std::memcmp(views.data(),source.data(),sizeof(Views))==0,"failed stereo query partially relabelled source");
        check(transforms[0].fov[0]==0&&transforms[1].fov[0]==0,"previous correction survived fallback");
        check(queries==before+(failure==6?0:1),"invalid display time queried runtime");
    }
    std::cout<<"late-timewarp: "<<checked<<" world-ray checks, new/retained frames, stereo fallback passed; stale-source error="<<negative_error<<'\n';
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
