#include "display_private.hpp"
#include "surfxr/localization.hpp"
#include "surfxr/keyboard_hints.hpp"

namespace surfxr {
bool XrDisplay::Impl::stage_performance() {

    const auto now=Clock::now();if(now<next_performance_update_&&performance_surface_.have_frame)return false;
    next_performance_update_=now+std::chrono::milliseconds(stats_.eye_gaze_supported?50:250);
    gaze_hud_.supported=stats_.eye_gaze_supported;gaze_hud_.active=vr_foveated_;
    gaze_hud_.eye_width=stats_.vr_eye_width[0];gaze_hud_.eye_height=stats_.vr_eye_height[0];
    gaze_hud_.base_width=stats_.vr_decoded_eye_width[0];gaze_hud_.base_height=stats_.vr_decoded_eye_height[0];
    gaze_hud_.focus_width=stats_.vr_focus_width;gaze_hud_.focus_height=stats_.vr_focus_height;
    gaze_hud_.center_x=stats_.vr_focus_center_x;gaze_hud_.center_y=stats_.vr_focus_center_y;
    const auto f=PerformanceHud::raster(performance_,clock_ns(now),++performance_generation_,bandwidth_.summary(clock_ns(now)),gaze_hud_,surfxr_performance_.snapshot(clock_ns(now)));
    stage(performance_surface_,f);
    ++stats_.performance_overlay_updates;return true;
}

void XrDisplay::Impl::stage_ui() {
    const bool supported=!vr_was_active_||!callbacks_.keyboard_overlay||callbacks_.keyboard_overlay->supported.load();
    const bool active=supported&&(ui_hands_[0].active||ui_hands_[1].active);
    auto candidate=callbacks_.keyboard_overlay?callbacks_.keyboard_overlay->snapshot():nullptr;
    candidate_visible_=candidate&&Clock::now()-candidate->received<std::chrono::milliseconds(750)&&
        ui_.keyboard_panel().visible&&ui_.keyboard_panel().available&&!ui_.settings_visible()&&!pc_input_blocked_;
    if(candidate_visible_&&candidate!=candidate_image_){
        // Fixed allocation avoids swapchain recreation/device-idle waits when
        // the IME changes its window width on each keystroke. Crop at composition.
        VideoFrame f;f.width=keyboard_image::max_width;f.height=keyboard_image::max_height;f.stride=f.width*4;
        f.rgba.resize(std::size_t{f.stride}*f.height);
        for(unsigned y=0;y<candidate->height;++y)std::memcpy(f.rgba.data()+y*f.stride,candidate->rgba.data()+y*candidate->width*4,candidate->width*4);
        f.generation=++candidate_generation_;stage(surfaces_[11],f);candidate_image_=candidate;
    }
    const auto language=static_cast<unsigned>(i18n::system_language());
    if(ui_.keyboard_panel().visible&&(keyboard_language_!=language||keyboard_revision_!=ui_.revision()||keyboard_input_active_!=active||!surfaces_[4].have_frame)){
        VideoFrame f;f.width=2048;f.height=768;f.stride=f.width*4;f.rgba.resize(std::size_t{f.stride}*f.height);f.generation=++keyboard_generation_;
        ui_.raster_keyboard(f.rgba.data(),f.width,f.height,f.stride);
        keyboard_language_=language;
        if(!ui_.settings_visible()){
            const auto hint_y=static_cast<unsigned>(.098f*f.height);
            if(!active)ui_fill(f.rgba.data(),f.width,f.height,f.stride,0,hint_y,f.width,30,{126,53,29,255});
            ui_text(f.rgba.data(),f.width,f.height,f.stride,24,hint_y+4,active&&ui_.shortcut_hint()?ui_.shortcut_hint():keyboard_controller_hint(supported,active),3,{255,238,223,255});
        }
        stage(surfaces_[4],f);keyboard_revision_=ui_.revision();keyboard_input_active_=active;
    }
    for(unsigned i=0;i<2;++i)if(!surfaces_[5+i].have_frame){
        VideoFrame f;f.width=f.height=8;f.stride=32;f.generation=1;f.rgba.resize(256);
        ui_fill(f.rgba.data(),8,8,32,0,0,8,8,i?std::array<std::uint8_t,4>{255,185,65,255}:std::array<std::uint8_t,4>{55,210,255,255});stage(surfaces_[5+i],f);
    }
}

void XrDisplay::Impl::acquire(Surface& s,std::vector<Acquired>& acquired,const std::atomic_bool& stop) {
    // A pointer's laser and cursor reuse the same acquired/released image.
    for(const auto& a:acquired)if(a.surface==&s)return;
    const auto before_wait=desktop_measuring_frame_||vr_measuring_frame_?Clock::now():Clock::time_point{};
    auto info=xr_info<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    std::uint32_t index=0;xr_check(xrAcquireSwapchainImage(s.swapchain,&info,&index),"xrAcquireSwapchainImage");
    acquired.push_back({&s,index,false,false});
    auto wait=xr_info<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);wait.timeout=100'000'000;
    const auto deadline=Clock::now()+std::chrono::seconds(2);XrResult result;
    do{result=xrWaitSwapchainImage(s.swapchain,&wait);}while(result==XR_TIMEOUT_EXPIRED&&Clock::now()<deadline&&!stop.load());
    if(result==XR_TIMEOUT_EXPIRED)throw std::runtime_error("Swapchain image wait timed out");
    xr_check(result,"xrWaitSwapchainImage");acquired.back().waited=true;
    if(index>=s.images.size())throw std::runtime_error("Invalid swapchain image index");
    if(desktop_measuring_frame_){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-before_wait).count();
        ++stats_.desktop_swapchain_wait_calls;stats_.desktop_swapchain_wait_ms_sum+=ms;
        stats_.desktop_swapchain_wait_ms_max=std::max(stats_.desktop_swapchain_wait_ms_max,ms);}
    if(vr_measuring_frame_){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-before_wait).count();
        ++stats_.vr_swapchain_wait_calls;stats_.vr_swapchain_wait_ms_sum+=ms;
        stats_.vr_swapchain_wait_ms_max=std::max(stats_.vr_swapchain_wait_ms_max,ms);}
}

XrCompositionLayerQuad XrDisplay::Impl::quad(Surface& s,UiPose pose,float width,float height) {
    auto q=xr_info<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);q.space=space_;q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain=s.swapchain;q.subImage.imageRect.extent={static_cast<std::int32_t>(s.width),static_cast<std::int32_t>(s.height)};
    q.pose=xr_pose(pose);q.size={width,height};return q;
}

void XrDisplay::Impl::panel_layers(unsigned id,std::vector<Layer>& layers) {
    auto& s=surfaces_[id];const auto& p=ui_.panels()[id];
    stats_.curve_segments[id]=0;
    if(p.curvature_radians<=.001f){auto q=quad(s,p.pose,p.width,p.height);
        q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        layers.emplace_back(q);return;}
    const float radius=p.width/p.curvature_radians;
    if(stats_.cylinder_layer_enabled){
        auto cylinder=xr_info<XrCompositionLayerCylinderKHR>(XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR);
        cylinder.space=space_;cylinder.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        cylinder.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        cylinder.subImage.swapchain=s.swapchain;
        cylinder.subImage.imageRect.extent={static_cast<std::int32_t>(s.width),static_cast<std::int32_t>(s.height)};
        // Cylinder pose is the viewing center; the model pose is the
        // middle point on the arc, at local z=-radius from that center.
        cylinder.pose=xr_pose(ui_compose(p.pose,{{0,0,radius},{}}));
        cylinder.radius=radius;cylinder.centralAngle=p.curvature_radians;cylinder.aspectRatio=p.width/p.height;
        layers.emplace_back(cylinder,p.pose.position);return;
    }
    throw std::runtime_error("Curved panel requires projection renderer");
}

XrCompositionLayerQuad XrDisplay::Impl::controls_layer(unsigned id,const UiPanelControls& controls) {
    const auto& p=ui_.panels()[id];const auto inverse=ui_inverse(p.pose);
    const auto top=ui_compose(inverse,controls.top.pose),bottom=ui_compose(inverse,controls.bottom.pose);
    const float upper=top.position.y+controls.top.height*.5f,lower=bottom.position.y-controls.bottom.height*.5f;
    const float height=upper-lower,width=controls.top.width;
    if(!std::isfinite(height)||height<=0||!std::isfinite(width)||width<=0)
        throw std::runtime_error("Invalid external control geometry");
    auto& s=surfaces_[7+id];
    const unsigned image_limit=std::min({max_height_,max_image_,4096u});
    const unsigned w=std::max(32u,std::min({768u,max_width_,max_image_,static_cast<unsigned>(image_limit*width/height)}));
    const unsigned h=std::max(1u,static_cast<unsigned>(std::ceil(w*height/width)));
    if(controls_revision_[id]!=ui_.revision()||!s.have_frame||s.width!=w||s.height!=h){
        VideoFrame frame;frame.width=w;frame.height=h;frame.stride=w*4;frame.generation=++controls_generation_[id];
        frame.rgba.assign(std::size_t{frame.stride}*h,0); // The space over the desktop stays transparent.
        const auto draw=[&](const UiPanel& control,UiPose local,bool is_top){
            const auto y=std::min(h-1,static_cast<unsigned>(std::max(0.f,(upper-local.position.y-control.height*.5f)*h/height)));
            const auto rows=std::max(1u,std::min(h-y,static_cast<unsigned>(std::round(control.height*h/height))));
            ui_.raster_controls(id,is_top,frame.rgba.data()+std::size_t{y}*frame.stride,w,rows,frame.stride);
        };
        draw(controls.top,top,true);draw(controls.bottom,bottom,false);stage(s,frame);controls_revision_[id]=ui_.revision();
    }
    const UiPose pose=ui_compose(p.pose,{{0,(upper+lower)*.5f,top.position.z},{}});
    auto q=quad(s,pose,width,height);
    q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    return q;
}

UiVec3 XrDisplay::Impl::add3(UiVec3 a,UiVec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}

UiVec3 XrDisplay::Impl::mul3(UiVec3 p,float v){return {p.x*v,p.y*v,p.z*v};}

UiVec3 XrDisplay::Impl::cross(UiVec3 a,UiVec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}

UiVec3 XrDisplay::Impl::norm(UiVec3 p){const auto n=std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);return n>1e-6f?mul3(p,1/n):UiVec3{};}

UiQuat XrDisplay::Impl::axes(UiVec3 x,UiVec3 y,UiVec3 z) {
    const float trace=x.x+y.y+z.z;UiQuat q;
    if(trace>0){const float s=std::sqrt(trace+1)*2;q={(y.z-z.y)/s,(z.x-x.z)/s,(x.y-y.x)/s,s/4};}
    else if(x.x>y.y&&x.x>z.z){const float s=std::sqrt(1+x.x-y.y-z.z)*2;q={s/4,(y.x+x.y)/s,(z.x+x.z)/s,(y.z-z.y)/s};}
    else if(y.y>z.z){const float s=std::sqrt(1+y.y-x.x-z.z)*2;q={(y.x+x.y)/s,s/4,(z.y+y.z)/s,(z.x-x.z)/s};}
    else{const float s=std::sqrt(1+z.z-x.x-y.y)*2;q={(z.x+x.z)/s,(z.y+y.z)/s,s/4,(x.y-y.x)/s};}
    return q;
}

void XrDisplay::Impl::pointer_layers(unsigned hand,std::vector<Layer>& layers,bool include_laser) {
    const auto& h=ui_hands_[hand];if(!h.active)return;
    const auto& hit=ui_.hits()[hand];
    const auto direction=hit.valid?norm(add3(hit.point,mul3(h.aim.position,-1))):ui_rotate(h.aim.orientation,{0,0,-1});
    const auto length=hit.valid?hit.distance:2.f;const auto center=add3(h.aim.position,mul3(direction,length*.5f));
    const auto toward=norm(add3(head_pose_.position,mul3(center,-1)));
    auto x=norm(cross(direction,toward));
    if(x.x*x.x+x.y*x.y+x.z*x.z<.5f)x=ui_rotate(h.aim.orientation,{1,0,0});
    const auto z=norm(cross(x,direction));
    if(include_laser)layers.emplace_back(quad(surfaces_[5+hand],{center,axes(x,direction,z)},.0035f,length));
    if(hit.valid){
        const auto& p=hit.keyboard?ui_.keyboard_panel():ui_.panels().at(hit.panel_id);
        auto orientation=p.pose.orientation;
        if(hit.region==UiHitRegion::Video&&p.curvature_radians>.001f){
            const float u=p.curve_segments?((std::min(p.curve_segments-1,static_cast<unsigned>(hit.u*p.curve_segments))+.5f)/p.curve_segments):hit.u;
            const float angle=(u-.5f)*p.curvature_radians;
            orientation=ui_multiply(orientation,{0,-std::sin(angle*.5f),0,std::cos(angle*.5f)});
        }
        const auto position=add3(hit.point,ui_rotate(orientation,{0,0,.008f}));
        layers.emplace_back(quad(surfaces_[5+hand],{position,orientation},.015f,.015f));
    }else if(!include_laser){
        // Even on a constrained compositor keep an aiming indication.
        const auto tip=add3(h.aim.position,mul3(direction,length));
        layers.emplace_back(quad(surfaces_[5+hand],{tip,axes(x,direction,z)},.008f,.025f));
    }
}

} // namespace surfxr
