#include "surfxr/gaze_hud.hpp"
#include "ui_helpers.hpp"
#include <cmath>
#include <cstdio>

namespace surfxr {
bool GazeHud::live_point(const GazeHudState& gaze,unsigned eye,std::uint64_t now,std::array<float,2>& point) noexcept {
    const auto& sample=gaze.sample;
    if(eye>=2||!gaze.supported||!sample.valid[eye]||!sample.sequence||!gaze.current_epoch||
       sample.origin_epoch!=gaze.current_epoch||!sample.sampled_monotonic_ns||now<sample.sampled_monotonic_ns||
       now-sample.sampled_monotonic_ns>100'000'000ull)return false;
    for(float value:sample.positions[eye])if(!std::isfinite(value)||std::abs(value)>.5f)return false;
    point={.5f+.5f*sample.positions[eye][0],.5f+.5f*sample.positions[eye][1]};return true;
}
bool GazeHud::marker_pose(const GazeHudState& gaze,unsigned eye,std::uint64_t now,const UiPose& eye_pose,
                          const std::array<float,4>& fov,UiPose& output) noexcept {
    std::array<float,2> point{};if(!live_point(gaze,eye,now,point))return false;
    if(!ui_detail::valid_pose(eye_pose))return false;
    for(float angle:fov)if(!std::isfinite(angle)||std::abs(angle)>=1.570796327f)return false;
    const float l=std::tan(fov[0]),r=std::tan(fov[1]),u=std::tan(fov[2]),d=std::tan(fov[3]);
    if(!(l<r&&d<u))return false;
    output=ui_compose(eye_pose,{{l+(r-l)*point[0],u+(d-u)*point[1],-1.f},{}});return true;
}
VideoFrame GazeHud::marker_texture(){
    VideoFrame frame;frame.width=frame.height=64;frame.stride=256;frame.generation=1;frame.rgba.assign(64*256,0);
    // Thin cyan ring with a dark outline; keep its center transparent.
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
        const float dx=float(x)-31.5f,dy=float(y)-31.5f,r=std::sqrt(dx*dx+dy*dy);
        const bool cross=(std::abs(dx)<=1.5f&&std::abs(dy)>=22.f&&std::abs(dy)<=29.f)||
                         (std::abs(dy)<=1.5f&&std::abs(dx)>=22.f&&std::abs(dx)<=29.f);
        if(cross||(r>=17.f&&r<=23.f))ui_fill(frame.rgba.data(),64,64,256,x,y,1,1,
            cross||(r>=19.f&&r<=21.f)?std::array<std::uint8_t,4>{65,205,255,255}:std::array<std::uint8_t,4>{8,16,24,220});
    }
    return frame;
}
void GazeHud::raster(VideoFrame& frame,const GazeHudState& gaze,std::uint64_t now,int y_offset){
    constexpr std::array<std::uint8_t,4> muted{176,200,222,255};
    constexpr std::array<std::uint8_t,4> live{65,205,255,255},crop{255,185,65,255};
    const auto text=[&](int x,int y,const char* value,unsigned scale=2,std::array<std::uint8_t,4> color={236,243,251,255}){
        ui_text(frame.rgba.data(),frame.width,frame.height,frame.stride,x,y+y_offset,value,scale,color);
    };
    const auto fill=[&](int x,int y,int w,int h,std::array<std::uint8_t,4> color){
        ui_fill(frame.rgba.data(),frame.width,frame.height,frame.stride,x,y+y_offset,w,h,color);
    };
    fill(14,292,692,1,{45,58,73,255});
    const char* state=gaze.active?(gaze.requested?"ACTIVE":"STOPPING"):
        !gaze.requested?"OFF":!gaze.supported?"UNAVAILABLE":"WAITING";
    char line[96];std::snprintf(line,sizeof(line),"GAZE %s | BLUE LIVE / AMBER VIDEO",state);
    text(14,302,line);
    if(gaze.active)std::snprintf(line,sizeof(line),"CENTER %uX%u | BASE %uX%u | EYE %uX%u",
        gaze.focus_width,gaze.focus_height,gaze.base_width,gaze.base_height,gaze.eye_width,gaze.eye_height);
    else std::snprintf(line,sizeof(line),"VIDEO CROP -- | LIVE / VIDEO COORDINATES: EYE UV 0..1");
    text(14,329,line);
    for(unsigned eye=0;eye<2;++eye){
        const int x=20+eye*354,y=374,w=128,h=96;
        text(x,353,eye?"RIGHT":"LEFT",2,muted);
        fill(x,y,w,h,{24,38,52,220});
        fill(x,y,w,1,muted);fill(x,y+h-1,w,1,muted);fill(x,y,1,h,muted);fill(x+w-1,y,1,h,muted);
        fill(x+w/2,y,1,h,{45,58,73,255});fill(x,y+h/2,w,1,{45,58,73,255});
        const auto mark=[&](float u,float v,int radius,std::array<std::uint8_t,4> color){
            const int px=x+int(std::lround(u*(w-1))),py=y+int(std::lround(v*(h-1)));
            if(px>=x&&px<x+w&&py>=y&&py<y+h)fill(px-radius,py-radius,radius*2+1,radius*2+1,color);
        };
        const bool video=gaze.active&&gaze.eye_width&&gaze.eye_height&&gaze.focus_width&&gaze.focus_height&&
            gaze.focus_width<=gaze.eye_width&&gaze.focus_height<=gaze.eye_height&&
            std::isfinite(gaze.center_x[eye])&&std::isfinite(gaze.center_y[eye])&&
            gaze.center_x[eye]>=0&&gaze.center_x[eye]<=1&&gaze.center_y[eye]>=0&&gaze.center_y[eye]<=1;
        if(video){
            const float rx=float(gaze.focus_width)/gaze.eye_width*.5f,ry=float(gaze.focus_height)/gaze.eye_height*.5f;
            for(unsigned i=0;i<96;++i){const float angle=i*6.28318530718f/96;
                mark(gaze.center_x[eye]+rx*std::cos(angle),gaze.center_y[eye]+ry*std::sin(angle),0,crop);
            }
            mark(gaze.center_x[eye],gaze.center_y[eye],1,crop);
            std::snprintf(line,sizeof(line),"VIDEO %.2f %.2f",gaze.center_x[eye],gaze.center_y[eye]);
        }else std::snprintf(line,sizeof(line),"VIDEO --");
        text(x+142,y+27,line,2,crop);
        std::array<float,2> point{};
        if(live_point(gaze,eye,now,point)){
            mark(point[0],point[1],3,live);
            std::snprintf(line,sizeof(line),"LIVE  %.2f %.2f",point[0],point[1]);text(x+142,y+2,line,2,live);
            std::snprintf(line,sizeof(line),"AGE %llu MS",static_cast<unsigned long long>((now-gaze.sample.sampled_monotonic_ns)/1'000'000));
        }else{
            text(x+142,y+2,"LIVE INVALID",2,muted);std::snprintf(line,sizeof(line),"NO FRESH SAMPLE");
        }
        text(x+142,y+58,line,1,muted);
    }
}
} // namespace surfxr
