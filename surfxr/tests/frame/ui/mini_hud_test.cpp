#include "surfxr/mini_hud.hpp"
#include "surfxr/startup_hud.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace surfxr;
void check(bool passed,const char* reason){if(!passed)throw std::runtime_error(reason);}
bool has(const std::string& text,const char* value){return text.find(value)!=std::string::npos;}
SurfxrPerformance::Snapshot timing(){
    SurfxrPerformance::Snapshot value;value.available=value.fps_valid=true;
    value.fps=119.5;value.total_ms=43.8;value.decode_ms=7.3;return value;
}
UiPose looking_at(UiVec3 target){
    const float norm=std::sqrt(target.x*target.x+target.y*target.y+target.z*target.z);
    UiPose gaze;gaze.orientation={target.y/norm,-target.x/norm,0,1-target.z/norm};
    return gaze; // The ray API normalizes valid quaternion input.
}
void startup_notice(){
    for(bool vr:{false,true}){
        const auto image=StartupHud::raster(vr,7);
        check(image.width==480&&image.height==120&&image.generation==7&&image.rgba.size()==480*120*4,
              "startup_raster_extent_and_identity");
        check(image.rgba[3]==174,"startup_raster_translucent_background");
    }
    check(StartupHud::animation_phase(0)==0&&StartupHud::animation_phase(79'999'999)==0&&
        StartupHud::animation_phase(80'000'000)==1&&StartupHud::animation_phase(960'000'000)==0,"spinner_monotonic_phase_and_rate");
    const auto first=StartupHud::raster(false,1,0),next=StartupHud::raster(false,2,1);
    bool changed=false;
    for(unsigned y=0;y<first.height;++y)for(unsigned x=0;x<first.width;++x){
        const auto at=y*first.stride+x*4;
        const bool same=std::equal(first.rgba.begin()+at,first.rgba.begin()+at+4,next.rgba.begin()+at);
        if(x>=8&&x<56&&y>=12&&y<60)changed|=!same;
        else check(same,"spinner_keeps_title_and_footer_stable");
    }
    check(changed,"spinner_pixels_advance");
    StartupHud notice;
    notice.update(true,false,true);check(notice.visible(),"initial_wait_is_visible");
    notice.update(false,false,true);check(!notice.visible(),"completed_transition_hides_notice");
    notice.update(true,true,true);check(notice.visible(),"later_vr_switch_is_visible_over_live_desktop");
    notice.dismiss();notice.update(true,true,true);check(!notice.visible(),"dismiss_applies_to_current_switch");
    notice.update(true,false,true);check(notice.visible(),"reversed_pending_request_has_new_notice");
    notice.dismiss();notice.update(false,false,true);notice.update(true,false,true);
    check(notice.visible(),"later_wait_reappears_after_dismissal");
    notice.update(true,false,false);check(!notice.visible(),"persistent_disable_hides_pending_notice");
    notice.update(true,true,false);check(!notice.visible(),"disable_survives_mode_changes");
    notice.update(true,true,true);check(notice.visible(),"reenabling_during_transition_shows_notice");
    StartupHud click;click.update(true,false,true);const auto p=StartupHud::panel();
    check(p.pose.position.x-p.width*.5f>0&&p.pose.position.y+p.height*.5f<0&&
          p.pose.position.x<.35f&&p.pose.position.y>-.30f,
          "switching_hud_lower_right_near_center_without_covering_center");

}
void contents_and_raster(){
    InputPreferences preferences;PerformanceOverlay stats;
    stats.observe(1'000'000'000,1,0,0,0);
    stats.observe(1'004'000'000,1,0,0,0); // Reprojection must not shorten FRAME.
    stats.observe(1'008'000'000,2,0,0,0);
    auto text=MiniHud::contents(stats,1'008'000'000,preferences,timing());
    check(text.count==2&&has(text.lines[0],"FPS")&&has(text.lines[0],"119.5"),"default_new_video_fps");
    check(has(text.lines[1],"LAT UP->XR")&&has(text.lines[1],"43.8 MS"),"latency_has_submission_endpoint");
    for(unsigned mask=0;mask<16;++mask){
        preferences.mini_hud_fps=mask&1;preferences.mini_hud_latency=mask&2;
        preferences.mini_hud_frame_time=mask&4;preferences.mini_hud_decode=mask&8;
        text=MiniHud::contents(stats,1'008'000'000,preferences,timing());
        unsigned count=0;for(unsigned bit=0;bit<4;++bit)count+=bool(mask&(1u<<bit));
        check(text.count==count&&MiniHud::layout(preferences).rows==count,"independent_metric_selection");
        std::string joined;for(const auto& line:text.lines)joined+=line;
        check(has(joined,"FPS")==bool(mask&1)&&has(joined,"LAT")==bool(mask&2)&&
              has(joined,"FRAME")==bool(mask&4)&&has(joined,"DECODE")==bool(mask&8),"only_selected_metrics");
    }
    check(has(text.lines[2],"8.0 MS")&&has(text.lines[3],"7.3 MS"),"frame_interval_and_existing_decode");
    const auto all=MiniHud::raster(stats,1'008'000'000,71,preferences,timing());
    check(all.width==MiniHud::width&&all.height==MiniHud::height&&all.generation==71,"fixed_texture_extent");
    check(all.rgba[3]==0&&all.rgba[(all.height*all.width-1)*4+3]==0,"rounded_transparent_corners");
    auto missing=MiniHud::contents(stats,4'000'000'000,preferences,{});
    for(const auto& line:missing.lines)check(has(line,"--"),"unavailable_is_not_zero");
    auto invalid=timing();invalid.fps=std::numeric_limits<double>::quiet_NaN();invalid.total_ms=-1;invalid.decode_ms=INFINITY;
    missing=MiniHud::contents(stats,4'000'000'000,preferences,invalid);
    for(const auto& line:missing.lines)check(has(line,"--"),"nonfinite_metrics_are_unavailable");
    preferences=InputPreferences{};
    const auto small=MiniHud::raster(stats,1'008'000'000,72,preferences,timing());
    const auto marker=MiniHud::marker(preferences,73);const auto area=MiniHud::layout(preferences);
    check(small.rgba.size()==all.rgba.size()&&marker.rgba.size()==all.rgba.size(),"selection_never_resizes_texture");
    for(unsigned y=area.active_height;y<MiniHud::height;++y)for(unsigned x=0;x<MiniHud::width;++x)
        check(small.rgba[(y*MiniHud::width+x)*4+3]==0,"unused_texture_rows_transparent");
    check(marker.rgba[(area.active_height/2*MiniHud::width+MiniHud::width/2)*4+3]==0,"target_center_unobstructed");
    unsigned marked=0;for(std::size_t i=3;i<marker.rgba.size();i+=4)marked+=marker.rgba[i]!=0;
    check(marked>0&&marked<1000,"target_is_subtle_bounded_brackets");
}
void geometry_and_gaze(){
    InputPreferences p;auto area=MiniHud::layout(p);
    check(area.pose.position.x>0&&area.pose.position.y>0&&area.pose.position.z==-1.2f,"default_upper_right_view_space");
    const auto normal_width=area.width_m;p.mini_hud_scale=200;
    check(std::abs(MiniHud::layout(p).width_m-normal_width*2)<1e-6,"scale_changes_angular_size");
    for(int x:{-100,0,50,100,200})for(int y:{-100,0,50,100,200})for(int scale:{-1,50,100,200,1000}){
        p.mini_hud_x=x;p.mini_hud_y=y;p.mini_hud_scale=scale;p.mini_hud_frame_time=p.mini_hud_decode=true;
        area=MiniHud::layout(p);
        check(std::abs(area.pose.position.x)+area.width_m*.5f<=.65001f&&
              std::abs(area.pose.position.y)+area.height_m*.5f<=.45001f,"entire_panel_within_safe_extents");
        check(area.width_m>=.1499f&&area.width_m<=.6001f,"scale_defensively_clamped");
    }
    p=InputPreferences{};area=MiniHud::layout(p);auto gaze=looking_at(area.pose.position);
    check(MiniHud::gaze_hits(area,gaze),"raw_gaze_hits_off_center_target");
    check(!MiniHud::gaze_hits(area,{}),"forward_gaze_misses_upper_right");
    auto miss=gaze;miss.position.x=3;check(!MiniHud::gaze_hits(area,miss),"translated_ray_misses");
    miss=gaze;miss.orientation={0,1,0,0};check(!MiniHud::gaze_hits(area,miss),"backward_ray_rejected");
    miss=gaze;miss.position.z=-2;check(!MiniHud::gaze_hits(area,miss),"intersection_behind_ray_rejected");
    miss=gaze;miss.orientation={};miss.orientation.y=std::sqrt(.5f);miss.orientation.w=std::sqrt(.5f);
    check(!MiniHud::gaze_hits(area,miss),"parallel_ray_rejected");
    miss=gaze;miss.orientation={0,0,0,0};check(!MiniHud::gaze_hits(area,miss),"degenerate_pose_rejected");
    miss=gaze;miss.position.x=NAN;check(!MiniHud::gaze_hits(area,miss),"nan_position_rejected");
    miss=gaze;miss.orientation.w=INFINITY;check(!MiniHud::gaze_hits(area,miss),"infinite_orientation_rejected");
}
void gaze_visibility(){
    InputPreferences p;const auto area=MiniHud::layout(p);const auto gaze=looking_at(area.pose.position);
    MiniHudVisibility gate;
    const auto state=[&](bool hit,bool valid,std::uint64_t ms){return gate.update(p,area,hit?gaze:UiPose{},valid,ms*1'000'000);};
    check(!state(true,true,1000).values&&!state(true,true,1000).marker,"disabled_means_no_overlay");
    p.mini_hud_enabled=true;check(state(false,false,1000).values,"always_on_needs_no_eye_tracking");
    p.mini_hud_gaze_only=true;
    check(!state(true,true,1000).values&&state(true,true,1000).marker,"initial_dwell_keeps_target");
    check(!state(true,true,1079).values&&state(true,true,1080).values,"eighty_ms_dwell");
    check(state(false,true,1090).values&&state(false,true,1320).values,"short_look_away_grace");
    check(!state(false,true,1331).values,"sustained_look_away_hides");
    check(!state(true,true,1340).values&&state(true,true,1420).values,"reentry_requires_dwell");
    check(!state(true,false,1421).values,"tracking_loss_hides_immediately");
    check(!state(true,true,1422).values,"tracking_recovery_restarts_dwell");
    check(state(true,true,1502).values,"recovered_tracking_can_reveal");
    check(!state(true,true,2000).values,"long_update_gap_restarts_dwell");
    check(state(true,true,2080).values&&!state(true,true,2070).values,"clock_regression_discards_visibility");
    p.mini_hud_gaze_marker=false;check(!state(false,true,2080).marker,"target_can_be_hidden");
    p.mini_hud_gaze_only=false;check(state(false,false,2090).values,"turning_off_gaze_mode_is_immediate");
    p.mini_hud_gaze_only=true;check(!state(true,true,2100).values,"reenabling_gaze_mode_has_new_dwell");
    check(state(true,true,2180).values,"new_dwell_completes");
    auto bad=gaze;bad.orientation={0,0,0,0};
    check(!gate.update(p,area,bad,true,2'180'000'001).values,"malformed_pose_hides_without_grace");
    check(!state(true,true,2181).values&&state(true,true,2261).values,"malformed_pose_discards_dwell");
    bad=gaze;bad.position.x=NAN;
    check(!gate.update(p,area,bad,true,2'261'000'001).values,"nan_pose_hides_without_grace");
    gate.reset();check(!state(true,true,2181).values,"explicit_reset_clears_visibility");
    p.mini_hud_fps=p.mini_hud_latency=false;
    const auto empty=gate.update(p,MiniHud::layout(p),gaze,true,9'000'000'000);
    check(!empty.values&&!empty.marker,"no_selected_metrics_hides_all");
}
void gaze_freshness(){
    constexpr std::uint64_t now=1'000'000'000,bound=MiniHudVisibility::freshness_ns;
    const auto fresh=MiniHudVisibility::fresh_sample;
    check(fresh(now,now,now)&&fresh(now-bound,now-1,now),"current_and_past_gaze_fresh");
    check(!fresh(now-bound-1,now,now),"stale_sample_rejected");
    check(fresh(now+bound,now,now)&&!fresh(now+bound+1,now,now),"bounded_predicted_gaze_accepted");
    check(!fresh(std::numeric_limits<std::uint64_t>::max(),now,now),"absurd_future_time_no_overflow");
    check(!fresh(now,now-bound-1,now),"stale_query_rejected_even_with_current_sample");
    check(!fresh(now,now+1,now)&&!fresh(now,0,now),"future_and_missing_query_rejected");
    check(fresh(0,now-bound,now)&&!fresh(0,now-bound-1,now),"unavailable_sample_uses_tracked_query_freshness");
    check(!fresh(0,0,0),"zero_clock_cannot_establish_freshness");
}
void preview(const char* path,bool startup=false){
    InputPreferences p;PerformanceOverlay stats;stats.observe(1'000'000'000,1,0,0,0);stats.observe(1'008'000'000,2,0,0,0);
    const auto defaults=MiniHud::raster(stats,1'008'000'000,1,p,timing());const auto marker=MiniHud::marker(p,2);
    p.mini_hud_frame_time=p.mini_hud_decode=true;
    const std::vector<VideoFrame> frames=startup?std::vector<VideoFrame>{StartupHud::raster(false,1),StartupHud::raster(true,2)}:
        std::vector<VideoFrame>{defaults,MiniHud::raster(stats,1'008'000'000,3,p,timing()),marker};
    constexpr unsigned gap=16;unsigned w=gap,h=0;
    for(const auto& f:frames){w+=f.width+gap;h=std::max(h,f.height+gap*2);}
    std::ofstream file(path,std::ios::binary);file<<"P6\n"<<w<<' '<<h<<"\n255\n";
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const unsigned background=((x/16+y/16)%2)?38:28;std::array<unsigned char,3> rgb{};
        for(auto& value:rgb)value=static_cast<unsigned char>(background);
        unsigned left=gap;
        for(unsigned item=0;item<frames.size();left+=frames[item++].width+gap){
            if(x<left||x>=left+frames[item].width||y<gap||y>=gap+frames[item].height)continue;
            const auto* pixel=frames[item].rgba.data()+((y-gap)*frames[item].width+x-left)*4;
            for(unsigned c=0;c<3;++c)rgb[c]=static_cast<unsigned char>((pixel[c]*pixel[3]+background*(255-pixel[3]))/255);
        }
        file.write(reinterpret_cast<const char*>(rgb.data()),rgb.size());
    }
    check(bool(file),"write_preview");
}
} // namespace
int main(int argc,char** argv){
    startup_notice();contents_and_raster();geometry_and_gaze();gaze_visibility();gaze_freshness();
    if(argc==3&&std::string(argv[1])=="--preview-ppm")preview(argv[2]);
    else if(argc==3&&std::string(argv[1])=="--startup-preview-ppm")preview(argv[2],true);
    else check(argc==1,"expected_optional_preview_ppm_path");
    std::cout<<"mini-hud: metrics, fixed raster, placement, raw gaze and visibility passed\n";
}
