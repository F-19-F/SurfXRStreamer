#pragma once
#include "surfxr/vr_ui.hpp"
#include "surfxr/latest_frame.hpp"
#include <cmath>
#include <algorithm>

namespace surfxr {
// Per-transition dismissal; the persistent enable preference is supplied by XR.
class StartupHud {
public:
    static UiPanel panel(){
        UiPanel p;p.pose.position={.26f,-.18f,-1.2f};p.width=.36f;p.height=.09f;return p;
    }
    static unsigned animation_phase(std::uint64_t now_ns){return static_cast<unsigned>((now_ns/80'000'000)%12);}
    static VideoFrame raster(bool vr_requested,std::uint64_t generation,unsigned phase=0){
        VideoFrame f;f.width=480;f.height=120;f.stride=f.width*4;
        f.rgba.resize(std::size_t{f.stride}*f.height);f.generation=generation;
        ui_fill(f.rgba.data(),f.width,f.height,f.stride,0,0,f.width,f.height,{10,18,28,174});
        for(unsigned dot=0;dot<12;++dot){
            const double angle=dot*6.283185307179586/12;
            const double cx=32+16*std::sin(angle),cy=36-16*std::cos(angle);
            const auto age=(dot+12-phase%12)%12;
            const auto alpha=static_cast<std::uint8_t>(255-age*14);
            for(int y=static_cast<int>(cy)-3;y<=static_cast<int>(cy)+3;++y)
                for(int x=static_cast<int>(cx)-3;x<=static_cast<int>(cx)+3;++x){
                    const auto dx=x+.5-cx,dy=y+.5-cy;
                    if(dx*dx+dy*dy<=7)ui_fill(f.rgba.data(),f.width,f.height,f.stride,x,y,1,1,{109,226,198,alpha});
                }
        }
        ui_text_centered(f.rgba.data(),f.width,f.height,f.stride,64,12,400,48,
                vr_requested?"Switching to SteamVR":"Switching to desktop",2,{109,226,198,255});
        ui_text_centered(f.rgba.data(),f.width,f.height,f.stride,16,78,448,32,"Right Menu: hide",2,{236,243,251,255});
        return f;
    }
    void update(bool pending,bool target_vr,bool enabled){
        if(pending&&(!pending_||target_vr!=target_vr_||(!enabled_&&enabled)))dismissed_=false;
        pending_=pending;target_vr_=target_vr;enabled_=enabled;
    }
    bool visible()const{return pending_&&enabled_&&!dismissed_;}
    void dismiss(){dismissed_=true;}
private:
    bool pending_=false,target_vr_=false,enabled_=true,dismissed_=false;
};
}
