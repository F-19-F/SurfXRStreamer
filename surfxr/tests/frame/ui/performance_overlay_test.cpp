#include "surfxr/performance_overlay.hpp"
#include "surfxr/performance_hud.hpp"
#include "surfxr/vr_ui.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void surfxr_statistics_test(){
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("surfxr_statistics_failed");};
    surfxr::VrStereoFrame pair;auto meta=std::make_shared<surfxr::VrVideoMetadata>();pair.metadata=meta;
    auto eye=std::make_shared<surfxr::VideoFrame>();pair.eyes={eye,eye};
    meta->linear_layout=true;meta->timestamp=1'000'000'000;meta->header_received_monotonic_ns=1'020'000'000;
    eye->decoder_output_monotonic_ns=1'026'000'000;
    surfxr::SurfxrPerformance meter;check(meter.presented(pair,1'030'000'000));
    const auto s=meter.snapshot(1'030'000'000);
    check(s.available&&!s.fps_valid&&s.total_ms==30&&s.upstream_ms==20&&s.decode_ms==6&&s.after_decode_ms==4);
    check(!meter.presented(pair,1'040'000'000)); // Cached submissions do not inflate FPS or age.
    check(!meter.snapshot(3'030'000'001).available);
    meta->linear_layout=false;meta->gaze_atlas={1024,1088,768,832};pair.foveated=true;
    auto focus=std::make_shared<surfxr::VideoFrame>();focus->decoder_output_monotonic_ns=1'028'000'000;pair.focus={focus,focus};
    pair.origin_epoch=1;check(meter.presented(pair,1'030'000'000));
    check(meter.snapshot(1'030'000'000).decode_ms==8&&meter.snapshot(1'030'000'000).after_decode_ms==2);
    pair.focus[1]={};meta->timestamp+=1;check(!meter.presented(pair,1'030'000'001));
    pair.focus[1]=focus;
    meta->timestamp=1'025'000'000;meta->header_received_monotonic_ns=1'024'000'000;
    check(!meter.presented(pair,1'030'000'001)); // Clock ordering must be real.
    // Image identity follows PC capture intervals; only the original locally
    // uploaded tracking ID can measure UP->RECV/UP->XR, even across clock drift.
    pair.foveated=false;meta->linear_layout=true;meta->gaze_atlas={};
    for(const auto image:{500'000'000ull,9'000'000'000ull}){
        surfxr::SurfxrPerformance mapped;
        meta->timestamp=image;meta->surfxr_tracking_timestamp=1'000'000'000;
        meta->header_received_monotonic_ns=1'020'000'000;
        check(mapped.presented(pair,1'030'000'000));
        const auto timing=mapped.snapshot(1'030'000'000);
        check(timing.total_ms==30&&timing.upstream_ms==20&&timing.decode_ms==6&&timing.after_decode_ms==4);
        check(!mapped.presented(pair,1'030'000'001));
        ++meta->timestamp;check(mapped.presented(pair,1'030'000'002)); // New image, same tracking.
        meta->surfxr_tracking_timestamp=1'021'000'000;
        ++meta->timestamp;check(!mapped.presented(pair,1'030'000'003));
    }
    meta->surfxr_tracking_timestamp=0; // Legacy headers use one timestamp.
    pair.foveated=false;meta->linear_layout=true;meta->gaze_atlas={};pair.origin_epoch=2;
    for(unsigned i=0;i<=120;++i){meta->timestamp=4'000'000'000ull+i*8'333'334ull;
        meta->header_received_monotonic_ns=meta->timestamp+20'000'000;
        eye->decoder_output_monotonic_ns=meta->timestamp+26'000'000;
        check(meter.presented(pair,meta->timestamp+30'000'000));}
    check(meter.snapshot(meta->timestamp+30'000'000).fps_valid&&std::abs(meter.snapshot(meta->timestamp+30'000'000).fps-120)<.001);
}

}
int main(int argc,char** argv){
    surfxr_statistics_test();
    using P=surfxr::PerformanceOverlay;P stats;
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("performance_overlay_test_failed");};
    stats.observe(1'000'000'000,7,900'000'000,990'000'000,1'010'000'000);
    check(stats.summary(P::NewFrame,1'000'000'000).count==0);
    check(stats.summary(P::PoseToDisplay,1'000'000'000).mean==110);
    check(stats.summary(P::DecodeToDisplay,1'000'000'000).mean==20);
    stats.observe(1'010'000'000,7,900'000'000,990'000'000,1'020'000'000);
    check(stats.summary(P::NewFrame,1'010'000'000).count==0);
    check(stats.summary(P::Submit,1'010'000'000).mean==10);
    stats.observe(1'020'000'000,8,0,0,0);
    check(stats.summary(P::NewFrame,1'020'000'000).mean==20);
    check(stats.summary(P::PoseToDisplay,1'020'000'000).count==2);
    check(stats.summary(P::PoseToDisplay,1'020'000'000).p95==120);
    check(stats.summary(P::PoseToDisplay,1'020'000'000).variance==25);
    check(stats.summary(P::PoseToDisplay,1'020'000'000).stddev==5);
    check(stats.summary(P::Submit,4'000'000'000).count==0);
    stats.observe(4'000'000'000,9,0,0,0);
    check(stats.summary(P::NewFrame,4'000'000'000).count==0);
    stats.reset();check(stats.summary(P::PoseToDisplay,4'000'000'000).count==0);
    for(unsigned i=0;i<1000;++i)stats.observe(5'000'000'000ull+i*1'000'000ull,i,0,0,0);
    check(stats.summary(P::Submit,5'999'000'000).count==512);
    check(std::abs(stats.summary(P::Submit,5'999'000'000).p95-1)<.001);
    stats.observe(6'000'000'000,7,0,0,0,1);
    check(stats.summary(P::Submit,6'000'000'000).count==0);
    stats.observe(6'010'000'000,7,0,0,0,2);
    check(stats.summary(P::Submit,6'010'000'000).count==0);
    check(stats.summary(P::NewFrame,6'010'000'000).count==0);
    stats.observe(6'020'000'000,8,6'030'000'000,6'030'000'000,6'025'000'000,2);
    check(stats.summary(P::PoseToDisplay,6'020'000'000).count==0);
    check(stats.summary(P::DecodeToDisplay,6'020'000'000).count==0);
    stats.observe(6'015'000'000,9,0,0,0,2); // Monotonic rollback discards continuity.
    check(stats.summary(P::Submit,6'015'000'000).count==0);
    stats.reset();
    stats.observe(7'000'000'000,1,6'955'000'000,6'995'000'000,7'008'000'000);
    stats.observe(7'008'000'000,2,6'962'000'000,7'002'000'000,7'016'000'000);
    const surfxr::BandwidthOverlay::Summary rates{};
    surfxr::GazeHudState gaze;gaze.supported=gaze.active=true;gaze.current_epoch=2;
    gaze.sample.sequence=11;gaze.sample.origin_epoch=2;gaze.sample.sampled_monotonic_ns=7'000'000'000;
    gaze.sample.valid={true,true};gaze.sample.positions={{{{.2f,-.2f}},{{-.4f,.4f}}}};
    gaze.eye_width=3072;gaze.eye_height=3264;gaze.base_width=1024;gaze.base_height=1088;
    gaze.focus_width=768;gaze.focus_height=832;gaze.center_x={.4f,.5f};gaze.center_y={.3f,.6f};
    std::array<float,2> point{};check(surfxr::GazeHud::live_point(gaze,0,7'008'000'000,point)&&point[0]==.6f&&point[1]==.4f);
    const surfxr::UiPose eye{{-.03f,0,0},{0,std::sin(.15f),0,std::cos(.15f)}};
    const std::array<float,4> fov{-.7f,.9f,.8f,-.6f};surfxr::UiPose marker;
    check(surfxr::GazeHud::marker_pose(gaze,0,7'008'000'000,eye,fov,marker));
    const auto local=surfxr::ui_compose(surfxr::ui_inverse(eye),marker);
    check(std::abs(local.position.x-(std::tan(fov[0])+.6f*(std::tan(fov[1])-std::tan(fov[0]))))<1e-6f);
    check(std::abs(local.position.y-(std::tan(fov[2])+.4f*(std::tan(fov[3])-std::tan(fov[2]))))<1e-6f&&std::abs(local.position.z+1)<1e-6f);
    check(!surfxr::GazeHud::marker_pose(gaze,0,7'101'000'000,eye,fov,marker));
    auto bad_fov=fov;bad_fov[0]=bad_fov[1];check(!surfxr::GazeHud::marker_pose(gaze,0,7'008'000'000,eye,bad_fov,marker));
    const auto ring=surfxr::GazeHud::marker_texture();check(ring.rgba.size()==64*64*4&&ring.rgba[(32*64+32)*4+3]==0&&ring.rgba[3]==0);
    check(!surfxr::GazeHud::live_point(gaze,2,7'008'000'000,point));
    check(!surfxr::GazeHud::live_point(gaze,0,7'101'000'000,point)); // Expired, never retain an old dot.
    check(!surfxr::GazeHud::live_point(gaze,0,6'999'000'000,point)); // Future sample.
    gaze.current_epoch=3;check(!surfxr::GazeHud::live_point(gaze,0,7'008'000'000,point));gaze.current_epoch=2;
    const auto frame=surfxr::PerformanceHud::raster(stats,7'008'000'000,11,rates,gaze);
    check(frame.generation==11&&frame.width==720&&frame.height==559&&frame.rgba.size()==720*559*4);
    check(frame.rgba[3]==0&&frame.rgba[(558*720+719)*4+3]==0);
    const auto live_pixel=(487*720+96)*4,video_pixel=(478*720+71)*4;
    check(frame.rgba[live_pixel]==65&&frame.rgba[live_pixel+1]==205&&frame.rgba[live_pixel+2]==255);
    check(frame.rgba[video_pixel]==255&&frame.rgba[video_pixel+1]==185&&frame.rgba[video_pixel+2]==65);
    gaze.sample.valid[0]=false;
    const auto invalid=surfxr::PerformanceHud::raster(stats,7'008'000'000,12,rates,gaze);
    check(invalid.rgba[live_pixel]==24); // The other eye/crop cannot make this eye valid.
    gaze.sample.valid[0]=true;gaze.supported=false;check(!surfxr::GazeHud::live_point(gaze,0,7'008'000'000,point));
    gaze.supported=true;gaze.sample.positions[0][0]=NAN;check(!surfxr::GazeHud::live_point(gaze,0,7'008'000'000,point));
    check(frame.rgba[(105*720+710)*4+3]==180);
    unsigned opaque=0;for(std::size_t i=3;i<frame.rgba.size();i+=4)opaque+=frame.rgba[i]==255;
    check(opaque>1000&&opaque<50000); // Text and two bounded bars are present.
    if(argc==3&&(std::string(argv[1])=="--preview-rgba"||std::string(argv[1])=="--preview-surfxr-rgba")){
        surfxr::SurfxrPerformance::Snapshot surfxr_values;surfxr_values.available=surfxr_values.fps_valid=true;
        surfxr_values.total_ms=43.8;surfxr_values.upstream_ms=28.1;surfxr_values.decode_ms=7.3;surfxr_values.after_decode_ms=8.4;surfxr_values.fps=119.5;
        const auto preview=std::string(argv[1])=="--preview-rgba"?frame:surfxr::PerformanceHud::raster(stats,7'008'000'000,11,rates,gaze,surfxr_values);
        std::ofstream output(argv[2],std::ios::binary);
        output.write(reinterpret_cast<const char*>(preview.rgba.data()),preview.rgba.size());
        check(bool(output));
    }else check(argc==1);
    std::cout<<"performance-overlay: timing, bandwidth, live gaze validity and video crop maps passed\n";
}
