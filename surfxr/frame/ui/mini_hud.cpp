#include "surfxr/mini_hud.hpp"
#include <cmath>
#include <cstdio>

namespace surfxr {
namespace {
constexpr unsigned padding=12,row_height=28;
VideoFrame texture(std::uint64_t generation){
    VideoFrame frame;frame.width=MiniHud::width;frame.height=MiniHud::height;
    frame.stride=MiniHud::width*4;frame.generation=generation;
    frame.rgba.resize(std::size_t(frame.stride)*frame.height);
    return frame;
}
bool normalized_pose(UiPose& pose) noexcept {
    const auto& p=pose.position;auto& q=pose.orientation;
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
       !std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)||!std::isfinite(q.w))return false;
    const double norm=std::sqrt(double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w);
    if(norm<1e-6)return false;
    q.x=float(q.x/norm);q.y=float(q.y/norm);q.z=float(q.z/norm);q.w=float(q.w/norm);
    return true;
}
} // namespace

MiniHud::Layout MiniHud::layout(const InputPreferences& preferences) noexcept {
    Layout result;
    result.rows=unsigned(preferences.mini_hud_fps)+unsigned(preferences.mini_hud_latency)+
                unsigned(preferences.mini_hud_frame_time)+unsigned(preferences.mini_hud_decode);
    result.active_height=result.rows?padding*2+row_height*result.rows:0;
    const float scale=std::clamp(preferences.mini_hud_scale,50,200)/100.f;
    result.width_m=.30f*scale;result.height_m=result.width_m*float(result.active_height)/width;
    // The whole panel, including its maximum scale, stays within these extents.
    const float x=(std::clamp(preferences.mini_hud_x,0,100)/50.f-1.f)*(.65f-result.width_m*.5f);
    const float y=(1.f-std::clamp(preferences.mini_hud_y,0,100)/50.f)*(.45f-result.height_m*.5f);
    result.pose.position={x,y,-1.2f};
    return result;
}

MiniHud::Contents MiniHud::contents(const PerformanceOverlay& values,std::uint64_t now,
                                  const InputPreferences& preferences,const SurfxrPerformance::Snapshot& surfxr){
    Contents result;
    const auto add=[&](const char* label,double value,bool valid,const char* unit){
        char line[48];
        if(valid&&std::isfinite(value)&&value>=0&&value<1e6)
            std::snprintf(line,sizeof(line),"%-10s %6.1f%s",label,value,unit);
        else std::snprintf(line,sizeof(line),"%-10s     --%s",label,unit);
        result.lines[result.count++]=line;
    };
    if(preferences.mini_hud_fps)add("FPS",surfxr.fps,surfxr.available&&surfxr.fps_valid,"");
    // UP->XR ends at XR submission, not photons or predicted display time.
    if(preferences.mini_hud_latency)add("LAT UP->XR",surfxr.total_ms,surfxr.available," MS");
    if(preferences.mini_hud_frame_time){const auto frame=values.summary(PerformanceOverlay::NewFrame,now);
        add("FRAME",frame.mean,frame.count>0," MS");}
    if(preferences.mini_hud_decode)add("DECODE",surfxr.decode_ms,surfxr.available," MS");
    return result;
}

VideoFrame MiniHud::raster(const PerformanceOverlay& values,std::uint64_t now,std::uint64_t generation,
                          const InputPreferences& preferences,const SurfxrPerformance::Snapshot& surfxr){
    auto frame=texture(generation);const auto area=layout(preferences);
    if(!area.rows)return frame;
    constexpr int radius=9;
    for(int y=0;y<int(area.active_height);++y)for(int x=0;x<int(width);++x){
        const int cx=std::clamp(x,radius,int(width)-radius-1),cy=std::clamp(y,radius,int(area.active_height)-radius-1);
        if((x-cx)*(x-cx)+(y-cy)*(y-cy)>radius*radius)continue;
        auto* pixel=frame.rgba.data()+y*frame.stride+x*4;
        pixel[0]=10;pixel[1]=18;pixel[2]=28;pixel[3]=174;
    }
    const auto text=contents(values,now,preferences,surfxr);
    for(unsigned row=0;row<text.count;++row)
        ui_text(frame.rgba.data(),width,height,frame.stride,padding,padding+row_height*row,
                text.lines[row].c_str(),2,row?std::array<std::uint8_t,4>{236,243,251,255}:
                                             std::array<std::uint8_t,4>{109,226,198,255});
    return frame;
}

VideoFrame MiniHud::marker(const InputPreferences& preferences,std::uint64_t generation){
    auto frame=texture(generation);const auto area=layout(preferences);
    if(!area.rows)return frame;
    // Four quiet corner brackets show exactly where looking reveals the meter.
    constexpr unsigned inset=5,length=15,thickness=2;
    for(unsigned y=inset;y<area.active_height-inset;++y)for(unsigned x=inset;x<width-inset;++x){
        const unsigned dx=std::min(x-inset,width-inset-1-x),dy=std::min(y-inset,area.active_height-inset-1-y);
        if(!((dx<thickness&&dy<length)||(dy<thickness&&dx<length)))continue;
        auto* pixel=frame.rgba.data()+y*frame.stride+x*4;
        pixel[0]=130;pixel[1]=205;pixel[2]=200;pixel[3]=150;
    }
    return frame;
}

bool MiniHud::gaze_hits(const Layout& area,const UiPose& gaze) noexcept {
    if(!area.rows||!std::isfinite(area.width_m)||!std::isfinite(area.height_m)||
       area.width_m<=0||area.height_m<=0)return false;
    auto origin=gaze,panel=area.pose;
    if(!normalized_pose(origin)||!normalized_pose(panel))return false;
    const auto local=ui_compose(ui_inverse(panel),origin);
    const auto direction=ui_rotate(local.orientation,{0,0,-1});
    if(direction.z>=-1e-6f)return false;
    const float distance=-local.position.z/direction.z;
    if(!std::isfinite(distance)||distance<=0)return false;
    const float x=local.position.x+distance*direction.x,y=local.position.y+distance*direction.y;
    return std::isfinite(x)&&std::isfinite(y)&&std::abs(x)<=area.width_m*.5f&&std::abs(y)<=area.height_m*.5f;
}

bool MiniHudVisibility::fresh_sample(std::uint64_t sample_ns,std::uint64_t query_ns,std::uint64_t now_ns) noexcept {
    if(!query_ns||query_ns>now_ns||now_ns-query_ns>freshness_ns)return false;
    if(!sample_ns)return true;
    return sample_ns<=now_ns?now_ns-sample_ns<=freshness_ns:sample_ns-now_ns<=freshness_ns;
}

MiniHudVisibility::State MiniHudVisibility::update(const InputPreferences& preferences,const MiniHud::Layout& area,
                                                const UiPose& gaze,bool valid,std::uint64_t now) noexcept {
    if(!preferences.mini_hud_enabled||!area.rows){reset();return {};}
    if(!preferences.mini_hud_gaze_only){reset();return {true,false};}
    auto checked=gaze;
    if(!valid||!normalized_pose(checked)){reset();return {false,preferences.mini_hud_gaze_marker};}
    if(has_update_&&(now<last_update_||now-last_update_>grace_ns))reset();
    has_update_=true;last_update_=now;
    if(MiniHud::gaze_hits(area,gaze)){
        if(!dwell_pending_){dwell_pending_=true;dwell_at_=now;}
        if(now-dwell_at_>=dwell_ns)visible_=true;
        last_hit_=now;
    }else{
        dwell_pending_=false;
        if(visible_&&now-last_hit_>grace_ns)visible_=false;
    }
    return {visible_,!visible_&&preferences.mini_hud_gaze_marker};
}
} // namespace surfxr
