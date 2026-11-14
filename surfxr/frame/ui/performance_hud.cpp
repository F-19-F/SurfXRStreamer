#include "surfxr/performance_hud.hpp"
#include "surfxr/vr_ui.hpp"
#include <cstdio>

namespace surfxr {
VideoFrame PerformanceHud::raster(const PerformanceOverlay& values,std::uint64_t now,std::uint64_t generation,const BandwidthOverlay::Summary& bandwidth,const GazeHudState& gaze,const SurfxrPerformance::Snapshot& surfxr_timing){
    VideoFrame frame;frame.width=width;frame.height=height;frame.stride=width*4;frame.generation=generation;
    frame.rgba.resize(std::size_t(frame.stride)*height);
    // Straight alpha, with rounded transparent corners and a quiet background.
    constexpr int radius=10;
    for(int y=0;y<int(height);++y)for(int x=0;x<int(width);++x){
        const int cx=std::clamp(x,radius,int(width)-radius-1),cy=std::clamp(y,radius,int(height)-radius-1);
        if((x-cx)*(x-cx)+(y-cy)*(y-cy)>radius*radius)continue;
        auto* pixel=frame.rgba.data()+y*frame.stride+x*4;
        pixel[0]=10;pixel[1]=18;pixel[2]=28;pixel[3]=180;
    }
    char fps[16],line[112];
    {
        if(surfxr_timing.fps_valid)std::snprintf(fps,sizeof(fps),"%.1f",surfxr_timing.fps);else std::snprintf(fps,sizeof(fps),"--");
        if(surfxr_timing.available)std::snprintf(line,sizeof(line),"SURFXR TIMING | UP->XR %.1f MS | NEW FPS %s",surfxr_timing.total_ms,fps);
        else std::snprintf(line,sizeof(line),"SURFXR TIMING | UP->XR -- MS | NEW FPS --");
        ui_text(frame.rgba.data(),width,height,frame.stride,14,12,line,2,{109,226,198,255});
        if(surfxr_timing.available)std::snprintf(line,sizeof(line),"UP->RECV %.1f | RECV->DEC %.1f | DEC->XR %.1f MS",
            surfxr_timing.upstream_ms,surfxr_timing.decode_ms,surfxr_timing.after_decode_ms);
        else std::snprintf(line,sizeof(line),"UP->RECV -- | RECV->DEC -- | DEC->XR -- MS");
        ui_text(frame.rgba.data(),width,height,frame.stride,14,37,line,2,{236,243,251,255});
        ui_text(frame.rgba.data(),width,height,frame.stride,14,62,"UP->RECV INCLUDES PC + NETWORK | XR: SUBMIT",1,{176,200,222,255});
    }
    auto text=[&](unsigned row,const char* value,std::array<std::uint8_t,4> color){
        ui_text(frame.rgba.data(),width,height,frame.stride,14,87+row*25,value,2,color);
    };
    text(0,"VR TIMING | 2S MEAN/P95/SD MS | VAR MS2",{109,226,198,255});
    const char* labels[]{"NEW FRAME", "XR FRAME", "POSE->EST", "DEC->EST"};
    for(unsigned i=0;i<PerformanceOverlay::Count;++i){
        const auto summary=values.summary(static_cast<PerformanceOverlay::Metric>(i),now);char timing_line[64];
        if(summary.count)std::snprintf(timing_line,sizeof(timing_line),"%-10s %6.1f / %6.1f / %6.1f / %8.1f",labels[i],summary.mean,summary.p95,summary.stddev,summary.variance);
        else std::snprintf(timing_line,sizeof(timing_line),"%-10s --",labels[i]);
        text(i+1,timing_line,{236,243,251,255});
    }
    ui_text(frame.rgba.data(),width,height,frame.stride,14,215,"NEW/XR: INTERVAL | EST: PREDICTED DISPLAY",1,{176,200,222,255});
    (void)bandwidth;
    GazeHud::raster(frame,gaze,now,75);
    return frame;
}
} // namespace surfxr
