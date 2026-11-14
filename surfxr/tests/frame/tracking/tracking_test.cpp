#include "surfxr/tracking.hpp"
#include "surfxr/shortcut.hpp"
#include "surfxr/vr_frame.hpp"
#include "surfxr/test_head_motion.hpp"
#include <cstring>
#include <cstddef>
#include <type_traits>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
// A C++ memberwise copy need not preserve padding. Snapshot actual bytes when
// checking a no-op; never compare the padding of two separately copied structs.
using TrackingBytes=std::array<unsigned char,sizeof(surfxr::TrackingFrame)>;
TrackingBytes tracking_bytes(const surfxr::TrackingFrame& frame){
    TrackingBytes bytes;std::memcpy(bytes.data(),&frame,sizeof(frame));return bytes;
}
struct ShortcutSample {unsigned ms;bool down=false,active=true,system=false;unsigned hand=1;};
struct ShortcutCounts {unsigned keyboard=0,toggles=0;};
ShortcutCounts replay_shortcut(std::initializer_list<ShortcutSample> samples){
    surfxr::ShortcutGesture gesture;ShortcutCounts counts;auto current=*samples.begin();auto next=samples.begin();++next;
    const auto end=(samples.end()-1)->ms+600;
    for(unsigned ms=0;ms<=end;++ms){
        if(next!=samples.end()&&ms==next->ms){current=*next;++next;}
        std::array<bool,2> down{},active{{current.active,current.active}};down[current.hand]=current.down;
        const auto r=gesture.update(ms*1'000'000ull,active,down,current.system);
        counts.keyboard+=r.keyboard[0]+r.keyboard[1];counts.toggles+=r.toggle_mode;
    }
    return counts;
}
unsigned shortcut_tests(){
    const auto check=[](std::initializer_list<ShortcutSample> input,unsigned keys,unsigned toggles,const char* name){
        const auto actual=replay_shortcut(input);require(actual.keyboard==keys&&actual.toggles==toggles,name);
    };
    check({{0},{100,true},{200}},1,0,"single_menu_once_after_window");
    check({{0},{100,true},{200},{550,true},{650}},0,1,"double_at_exact_350ms_edge");
    check({{0},{100,true},{200},{551,true},{651}},2,0,"outside_double_window_two_singles");
    check({{0},{100,true},{111},{122,true},{133}},0,0,"11ms_noise_is_not_two_clicks");
    check({{0},{100,true},{200},{211,true},{300}},1,0,"11ms_false_release_does_not_split_hold");
    check({{0},{100,true},{200},{300,true},{1790}},0,0,"1490ms_second_hold_is_not_double");
    check({{0},{100,true},{1600}},0,0,"long_first_hold_is_not_click");
    check({{0},{100,true},{600}},1,0,"500ms_click_is_valid");
    check({{0},{100,true},{601}},0,0,"501ms_hold_is_not_click");
    check({{0},{100,true},{129}},0,0,"29ms_press_is_noise");
    check({{0},{100,true},{131}},1,0,"31ms_press_settles");
    check({{0,true},{200}},0,0,"held_at_start_requires_release");
    check({{0},{100,true},{200,false,false},{211,true},{300}},0,0,"action_invalid_during_hold_cancels");
    check({{0},{100,true},{200},{250,false,false},{300,true},{400}},0,0,"action_loss_cancels_pending");
    check({{0},{100,true},{200},{250,false,true,true},{300,true,true,true},{400}},0,0,"system_cancels_pending_and_alias");
    check({{0},{100,true,true,true},{200},{231,true},{331}},1,0,"fresh_menu_after_30ms_release_is_not_swallowed");
    check({{0},{100,true,true,true},{200,true},{400}},0,0,"held_menu_after_system_cannot_click");
    check({{0},{100,true,true,true},{200},{220,true},{400}},0,0,"20ms_release_after_system_cannot_rearm");
    check({{0},{100,false,true,true},{200},{600,true},{700}},1,0,"menu_rearms_after_system_release");
    check({{0},{100,true,true,false,0},{200},{300,true},{400}},2,0,"different_hands_are_not_double");
    check({{0},{100,true},{200},{300,true},{400},{431,true},{550},{620,true},{700}},0,2,"two_explicit_doubles_can_reverse_immediately");
    check({{0},{100,true},{200},{300,true},{400},{431,true},{500}},1,1,"third_click_is_fresh_single_not_reverse");
    check({{0},{100,true},{200},{300,true},{400},{700,false,true,true},{720},{800,true},{900}},1,1,"system_cancels_without_delaying_fresh_menu");
    check({{0},{100,true},{200},{300,true},{400},{900,true},{1000},{1100,true},{1200}},0,2,"fresh_double_can_reverse");
    for(bool held:{false,true}){
        surfxr::ShortcutGesture g;unsigned toggles=0,keys=0;
        for(unsigned t=0;t<1100;++t){
            if(t==250)g.mode_changed();
            const bool down=(t>=100&&t<(held?300u:200u))||(t>=400&&t<500);
            const auto r=g.update(t*1'000'000ull,{{false,true}},{{false,down}});
            toggles+=r.toggle_mode;keys+=r.keyboard[1];
        }
        require(toggles==0&&keys==1,"mode_change_consumes_old_half_or_held_click");
    }
    {
        surfxr::TrackingFrame frame;frame.focused=true;
        auto& hand=frame.hands[1];hand.profile=surfxr::TrackingProfile::SteamFrame;hand.buttons_active=surfxr::ButtonMenu;
        surfxr::ShortcutGesture g;unsigned toggles=0,keys=0;
        for(unsigned t=0;t<1300;++t){
            // Neither hand has any valid aim/grip pose, but physical buttons do.
            hand.buttons=((t>=100&&t<200)||(t>=300&&t<400)||(t>=431&&t<500))?unsigned(surfxr::ButtonMenu):0u;
            const auto b=surfxr::shortcut_buttons(frame,true,{},{});
            const auto r=g.update(t*1'000'000ull,b.active,b.down,b.system_down);
            toggles+=r.toggle_mode;keys+=r.keyboard[1];
            if(r.toggle_mode)require(t==430,"double_fires_on_debounced_release_without_extra_wait");
            if(r.keyboard[1])require(t==851,"fresh_single_only_waits_double_disambiguation");
        }
        require(toggles==1&&keys==1,"valid_physical_buttons_work_without_spatial_tracking");
    }
    {
        surfxr::ShortcutGesture g;unsigned toggles=0;
        for(unsigned t=0;t<1500;++t){
            const bool down=(t>=100&&t<200)||(t>=300&&t<400)||(t>=470&&t<550)||(t>=620&&t<700);
            const auto result=g.update(t*1'000'000ull,{{true,true}},{{false,down}});
            toggles+=result.toggle_mode;
            if(t==431)g.mode_changed(); // Logical mode update after the consumed double.
            // Presentation arriving at t=600 must not reset a newer half gesture.
        }
        require(toggles==2,"logical_mode_consumes_old_clicks_without_delaying_fresh_double");
    }
    {
        surfxr::ShortcutGesture g;unsigned toggles=0,keys=0;
        for(unsigned t=0;t<2300;++t){
            // A slow switch finishes after the original post-double timeout.
            // A System click changes focus, followed by two fresh physical doubles.
            if(t==900||t==1020)g.reset();
            const bool down=(t>=100&&t<200)||(t>=300&&t<400)||(t>=1060&&t<1110)||(t>=1160&&t<1210)||
                (t>=1700&&t<1800)||(t>=1900&&t<2000);
            const auto result=g.update(t*1'000'000ull,{{false,true}},{{false,down}},t>=950&&t<1000);
            toggles+=result.toggle_mode;keys+=result.keyboard[0]+result.keyboard[1];
        }
        require(toggles==3&&keys==0,"slow_presentation_does_not_block_fresh_physical_doubles");
    }
    {
        surfxr::ShortcutGesture g;unsigned toggles=0,keys=0;
        for(unsigned t=0;t<2000;++t){
            if(t==100)g.reset();
            const bool active=t<100||t>=700;
            const bool down=(t>=800&&t<850)||(t>=900&&t<950);
            const auto result=g.update(t*1'000'000ull,{{false,active}},{{false,down}});
            toggles+=result.toggle_mode;keys+=result.keyboard[0]+result.keyboard[1];
        }
        require(toggles==1&&keys==0,"fresh_double_after_input_returns_has_no_quiet_lockout");
    }
    {
        surfxr::ShortcutGesture g;const std::array<bool,2> active{{true,true}},up{},down{{false,true}};
        for(unsigned t=0;t<=240;++t)g.update(t*1'000'000ull,active,{{false,t>=100&&t<200}});
        // No sample for >100 ms: do not carry pending first click across a stall.
        require(!g.update(400'000'000,active,down).toggle_mode,"gap_discards_pending");
        for(unsigned t=401;t<=600;++t)require(!g.update(t*1'000'000ull,active,{{false,t<500}}).toggle_mode,"gap_requires_release");
        g.reset();g.update(700'000'000,active,down);
        require(!g.update(1,active,up).toggle_mode,"backward_clock_resets");
    }
    {
        surfxr::TrackingFrame frame;frame.focused=true;
        frame.hands[0].profile=frame.hands[1].profile=surfxr::TrackingProfile::SteamFrame;
        const std::array<bool,2> both{{true,true}},up{};
        frame.hands[1].buttons_active=surfxr::ButtonMenu;
        auto buttons=surfxr::shortcut_buttons(frame,true,both,both);
        require(!buttons.down[1]&&!buttons.active[0]&&buttons.active[1],"generic_menu_alias_not_physical_frame_menu");
        frame.hands[1].buttons=surfxr::ButtonMenu;
        frame.hands[0].buttons=frame.hands[0].buttons_active=surfxr::ButtonSystem;
        buttons=surfxr::shortcut_buttons(frame,true,up,up);
        require(buttons.down[1]&&buttons.system_down,"either_system_blocks_physical_menu");
        frame.hands[1].buttons_active=0;
        require(!surfxr::shortcut_buttons(frame,true,both,both).active[1],"inactive_raw_menu_cancels");
        frame.hands[1].buttons_active=surfxr::ButtonMenu;frame.focused=false;
        require(!surfxr::shortcut_buttons(frame,true,both,both).active[1],"unfocused_cancels_even_with_valid_buttons");
        frame.focused=true;frame.hands[0].profile=surfxr::TrackingProfile::OculusTouch;
        require(surfxr::shortcut_buttons(frame,true,both,both).active[0],"touch_left_menu_preserved");
        require(!surfxr::shortcut_buttons(frame,true,up,both).active[0],"fallback_action_validity_required");
    }
    return 22;
}
unsigned tests(){
    unsigned groups=0;surfxr::TrackingPose p;require(!surfxr::tracking_pose_valid(p),"default_pose_invalid");p.flags=surfxr::OrientationValid|surfxr::PositionValid;
    require(surfxr::tracking_pose_valid(p)&&!surfxr::tracking_pose_tracked(p),"valid_not_tracked");p.flags|=surfxr::OrientationTracked|surfxr::PositionTracked;require(surfxr::tracking_pose_tracked(p),"tracked_pose");p.pose.position.x=std::numeric_limits<float>::quiet_NaN();require(!surfxr::tracking_pose_valid(p),"nonfinite_pose_rejected");++groups;
    surfxr::TrackingFrame f;f.visible=true;f.prediction_time_valid=true;f.hands[1].profile=surfxr::TrackingProfile::SteamFrame;f.hands[1].buttons_active=surfxr::ButtonA|surfxr::ButtonX|surfxr::ButtonY|surfxr::ButtonMenu;f.hands[1].buttons=surfxr::ButtonA|surfxr::ButtonX|surfxr::ButtonMenu;f.hands[1].touches_active=f.hands[1].touches=surfxr::ButtonY;
    f.hands[0].grip.pose.position.x=-.3f;f.hands[1].grip.pose.position.x=.3f;f.hands[0].trigger=.7f;
    const auto mapped=surfxr::touch_controller_mapping(f);require(mapped[0].buttons==surfxr::ButtonX&&mapped[1].buttons==surfxr::ButtonA&&mapped[0].touches==surfxr::ButtonY,"physical_frame_to_logical_touch");require(mapped[0].grip.pose.position.x==-.3f&&mapped[1].grip.pose.position.x==.3f&&mapped[0].trigger==.7f,"mapping_preserves_physical_poses_analogs");++groups;
    f={};f.visible=true;f.prediction_time_valid=true;f.hands[0].profile=surfxr::TrackingProfile::OculusTouch;f.hands[0].buttons_active=surfxr::ButtonX;f.hands[0].buttons=surfxr::ButtonX|surfxr::ButtonY;require(surfxr::touch_controller_mapping(f)[0].buttons==surfxr::ButtonX,"mapping_drops_inactive_and_preserves_touch");++groups;
    {surfxr::TrackingFrame frame;frame.hands[0].profile=frame.hands[1].profile=surfxr::TrackingProfile::SteamFrame;
        frame.hands[0].buttons_active=frame.hands[0].buttons=surfxr::ButtonView;
        frame.hands[1].buttons_active=frame.hands[1].buttons=surfxr::ButtonSystem|surfxr::ButtonMenu;
        const auto result=surfxr::touch_controller_mapping(frame);
        require(result[0].buttons==surfxr::ButtonMenu&&result[0].buttons_active==surfxr::ButtonMenu&&result[1].buttons==0,"left_view_maps_to_remote_menu_right_menu_remains_local");
        frame.hands[0].buttons_active=surfxr::ButtonSystem;
        require(surfxr::touch_controller_mapping(frame)[0].buttons==0,"inactive_view_not_forwarded");
        frame.hands[0].buttons=surfxr::ButtonSystem;
        require(surfxr::touch_controller_mapping(frame)[0].buttons==surfxr::ButtonMenu,"optional_system_alias_preserved");
        frame.hands[0].buttons_active=surfxr::ButtonSystem;frame.hands[0].buttons=0;
        require(surfxr::touch_controller_mapping(frame)[0].buttons==0,"system_release_propagates");
        frame.hands[0].profile=surfxr::TrackingProfile::OculusTouch;frame.hands[0].buttons=surfxr::ButtonSystem;
        require(surfxr::touch_controller_mapping(frame)[0].buttons==0,"system_mapping_is_frame_specific");}++groups;
    groups+=shortcut_tests();
    require(surfxr::tracking_shortcut_reserved(surfxr::TrackingProfile::SteamFrame,1,true)&&surfxr::tracking_shortcut_reserved(surfxr::TrackingProfile::OculusTouch,0,true)&&
            !surfxr::tracking_shortcut_reserved(surfxr::TrackingProfile::OculusTouch,1,true)&&!surfxr::tracking_shortcut_reserved(surfxr::TrackingProfile::ValveIndex,0,true)&&
            surfxr::tracking_shortcut_reserved(surfxr::TrackingProfile::ValveIndex,0,false),"game_a_not_a_vr_shortcut");++groups;
    const auto fixture=[](){
        surfxr::VrStereoFrame pair;pair.origin_epoch=7;
        auto metadata=std::make_shared<surfxr::VrVideoMetadata>();metadata->timestamp=1234;metadata->tracking_frame_id=15;
        metadata->header_present=metadata->pose_valid=metadata->fov_valid=true;
        metadata->fov={surfxr::VrFovTangents{1,1,.5f,2},surfxr::VrFovTangents{1,1,2,.5f}};pair.metadata=metadata;
        for(auto& eye:pair.eyes){auto frame=std::make_shared<surfxr::VideoFrame>();frame->width=frame->height=2;frame->stride=8;frame->rgba.resize(16);
            frame->generation=1;frame->has_source_timestamp=true;frame->source_timestamp=metadata->timestamp;frame->vr_metadata=metadata;eye=frame;}
        return pair;
    };
    {auto pair=fixture();require(surfxr::valid_vr_stereo_frame(pair,7),"valid_stereo_pair");require(!surfxr::valid_vr_stereo_frame(pair,8)&&!surfxr::valid_vr_stereo_frame(pair,0),"stale_or_unassociated_epoch_rejected");pair.eyes[1].reset();require(!surfxr::valid_vr_stereo_frame(pair,7),"partial_stereo_rejected");}++groups;
    {auto pair=fixture();auto bad=std::make_shared<surfxr::VideoFrame>(*pair.eyes[1]);++bad->source_timestamp;pair.eyes[1]=bad;require(!surfxr::valid_vr_stereo_frame(pair,7),"mismatched_eye_timestamp_rejected");
        --bad->source_timestamp;auto metadata=std::make_shared<surfxr::VrVideoMetadata>(*bad->vr_metadata);++metadata->tracking_frame_id;bad->vr_metadata=metadata;require(!surfxr::valid_vr_stereo_frame(pair,7),"mismatched_pose_frame_rejected");
        pair=fixture();bad=std::make_shared<surfxr::VideoFrame>(*pair.eyes[1]);bad->rgba.pop_back();pair.eyes[1]=bad;require(!surfxr::valid_vr_stereo_frame(pair,7),"short_eye_buffer_rejected");}++groups;
    {auto pair=fixture();auto metadata=std::make_shared<surfxr::VrVideoMetadata>(*pair.metadata);pair.metadata=metadata;metadata->eye_poses[1].orientation.w=0;
        require(!surfxr::valid_vr_stereo_frame(pair,7),"invalid_eye_pose_rejected");metadata->eye_poses[1].orientation.w=1;metadata->fov[0].up=std::numeric_limits<float>::infinity();
        require(!surfxr::valid_vr_stereo_frame(pair,7),"nonfinite_fov_rejected");metadata->fov[0].up=0;require(!surfxr::valid_vr_stereo_frame(pair,7),"zero_fov_rejected");}++groups;
    {const auto angles=surfxr::vr_eye_fov_radians({1,1,.5f,2});
        require(std::abs(angles[0]+std::atan(.5f))<1e-6f&&std::abs(angles[1]-std::atan(2.f))<1e-6f&&
                std::abs(angles[2]-.785398163f)<1e-6f&&std::abs(angles[3]+.785398163f)<1e-6f,"asymmetric_fov_signs_and_order");}++groups;
    {surfxr::TrackingFrame frame;frame.predicted_xr_time=1'000'000'000;
        require(surfxr::set_tracking_prediction_time(frame,frame.predicted_xr_time)==surfxr::TrackingPredictionStatus::Valid&&frame.prediction_time_valid&&frame.prediction_lead_seconds==0,"zero_prediction_lead_valid");
        require(surfxr::set_tracking_prediction_time(frame,989'000'000)==surfxr::TrackingPredictionStatus::Valid&&std::abs(frame.prediction_lead_seconds-.011f)<1e-8f,"prediction_lead_units");
        require(surfxr::set_tracking_prediction_time(frame,934'000'000)==surfxr::TrackingPredictionStatus::Valid&&frame.prediction_lead_seconds==.066f,"prediction_lead_upper_boundary");}++groups;
    {surfxr::TrackingFrame frame;frame.predicted_xr_time=100;frame.prediction_time_valid=true;frame.prediction_lead_seconds=.01f;
        require(surfxr::set_tracking_prediction_time(frame,101)==surfxr::TrackingPredictionStatus::Past&&!frame.prediction_time_valid&&frame.prediction_lead_seconds==0,"past_pose_clears_prediction_validity");
        require(surfxr::set_tracking_prediction_time(frame,0)==surfxr::TrackingPredictionStatus::InvalidTime,"zero_xr_now_invalid");
        frame.predicted_xr_time=-1;require(surfxr::set_tracking_prediction_time(frame,100)==surfxr::TrackingPredictionStatus::InvalidTime,"negative_pose_time_invalid");}++groups;
    {surfxr::TrackingFrame frame;frame.predicted_xr_time=1'000'000'000;
        require(surfxr::set_tracking_prediction_time(frame,933'999'999)==surfxr::TrackingPredictionStatus::TooFar&&!frame.prediction_time_valid&&frame.prediction_lead_seconds==0,"over_limit_not_clamped");
        frame.predicted_xr_time=std::numeric_limits<std::int64_t>::max();
        require(surfxr::set_tracking_prediction_time(frame,1)==surfxr::TrackingPredictionStatus::TooFar,"huge_duration_rejected_without_overflow");
        require(surfxr::set_tracking_prediction_time(frame,frame.predicted_xr_time-1)==surfxr::TrackingPredictionStatus::Valid&&std::abs(frame.prediction_lead_seconds-1e-9f)<1e-15f,"large_epoch_preserves_ns_difference");}++groups;
    const auto motion_fixture=[](){
        surfxr::TrackingFrame frame;frame.origin=surfxr::TrackingOrigin::Stage;frame.origin_epoch=7;
        frame.stage_locatable=frame.stage_supported=frame.optics_valid=frame.visible=true;
        frame.sequence=1;frame.sampled_monotonic_ns=2'000'000'000;frame.query_started_monotonic_ns=1'999'000'000;
        frame.predicted_xr_time=3'000'000'000;frame.query_xr_time=2'970'000'000;
        frame.head.flags=surfxr::OrientationValid|surfxr::PositionValid|surfxr::OrientationTracked|surfxr::PositionTracked;
        frame.head.pose.position={1,1.7f,2};frame.ipd_meters=.064f;
        frame.eye_poses[0]={{.968f,1.7f,2},{}};frame.eye_poses[1]={{1.032f,1.7f,2},{}};
        frame.hands[0].aim_active=true;frame.hands[0].buttons=frame.hands[0].buttons_active=surfxr::ButtonMenu;
        frame.hands[1].trigger=.75f;frame.ui_captured[0]=true;
        return frame;
    };
    {auto frame=motion_fixture();const auto original=frame;const auto original_bytes=tracking_bytes(frame);surfxr::TestHeadMotion disabled;
        require(!disabled.apply(frame)&&tracking_bytes(frame)==original_bytes&&!disabled.snapshot().enabled,"test_motion_default_is_exact_noop");
        surfxr::TestHeadMotion motion(true);require(motion.apply(frame),"test_motion_valid_sample");
        require(std::abs(frame.head.pose.orientation.w-1)<1e-6f&&std::abs(frame.eye_poses[1].position.x-1.032f)<1e-6f,"test_motion_starts_at_original_pose");
        frame=original;frame.sequence=2;frame.sampled_monotonic_ns+=1'000'000'000;const auto before=frame;const auto before_bytes=tracking_bytes(frame);
        require(motion.apply(frame),"test_motion_next_sample");const auto stats=motion.snapshot();
        require(std::abs(stats.yaw_degrees-25)<1e-6&&std::abs(stats.pitch_degrees-10.392304845)<1e-6&&stats.samples==2,"test_motion_known_trajectory_extrema");
        const auto dx=frame.eye_poses[1].position.x-frame.eye_poses[0].position.x;
        const auto dy=frame.eye_poses[1].position.y-frame.eye_poses[0].position.y;
        const auto dz=frame.eye_poses[1].position.z-frame.eye_poses[0].position.z;
        require(std::abs(std::sqrt(dx*dx+dy*dy+dz*dz)-.064f)<1e-6f&&dz<-.02f,"test_motion_rotates_ipd_offsets");
        require(frame.head.linear_velocity_valid&&frame.head.angular_velocity_valid&&frame.head.linear_velocity.x==0,"test_motion_analytic_velocity_valid");
        auto restored=tracking_bytes(frame);
        static_assert(std::is_standard_layout_v<surfxr::TrackingFrame>);
        for(auto region:{std::pair{offsetof(surfxr::TrackingFrame,head),sizeof(frame.head)},
                         std::pair{offsetof(surfxr::TrackingFrame,eye_poses),sizeof(frame.eye_poses)}})
            std::memcpy(restored.data()+region.first,before_bytes.data()+region.first,region.second);
        require(restored==before_bytes,"test_motion_preserves_controllers_actions_clocks_flags");
        auto repeated=before;motion.apply(repeated);require(motion.snapshot().samples==2&&repeated.head.pose.orientation.y==frame.head.pose.orientation.y,"test_motion_repeat_sample_does_not_advance");
        frame=original;frame.sampled_monotonic_ns+=12'000'000'000;motion.apply(frame);
        require(std::abs(frame.head.pose.orientation.x)<1e-6f&&std::abs(frame.head.pose.orientation.y)<1e-6f,"test_motion_common_period_twelve_seconds");
        frame=original;frame.origin_epoch=8;frame.sampled_monotonic_ns+=14'000'000'000;motion.apply(frame);
        require(motion.snapshot().elapsed_seconds==0&&std::abs(frame.head.pose.orientation.w-1)<1e-6f,"test_motion_rebases_stage_origin");
    }++groups;
    {auto original=motion_fixture();surfxr::TestHeadMotion motion(true);
        for(unsigned invalid=0;invalid<4;++invalid){auto frame=original;
            if(invalid==0)frame.stage_locatable=false;
            if(invalid==1)frame.head.flags=0;
            if(invalid==2)frame.eye_poses[0].orientation.w=0;
            if(invalid==3)frame.sampled_monotonic_ns=0;
            const auto before=tracking_bytes(frame);require(!motion.apply(frame)&&tracking_bytes(frame)==before,"test_motion_never_invents_valid_tracking");}
        motion.apply(original);motion.sent(original);motion.feedback(1,100,original.eye_poses[0].orientation);
        auto next=motion_fixture();next.sequence=2;next.sampled_monotonic_ns+=1'000'000'000;motion.apply(next);motion.sent(next);
        motion.feedback(2,200,next.eye_poses[0].orientation);motion.feedback(2,200,next.eye_poses[0].orientation);
        const auto stats=motion.snapshot();require(stats.sent_packets==2&&stats.feedback_frames==2&&stats.matched_feedback_frames==2&&
            stats.feedback_rotation_max_degrees>26&&stats.feedback_error_degrees<.1&&stats.feedback_tracking_id==2,"test_motion_server_pose_echo_correlates_without_raw_pose_logging");
    }++groups;
    {auto frame=motion_fixture();surfxr::TestHeadMotion motion(true);
        frame.eye_poses[0].orientation={0,.25881904f,0,.96592583f}; // Local eye yaw +30 degrees.
        motion.apply(frame);motion.sent(frame);motion.feedback(1,100,frame.head.pose.orientation);
        require(motion.snapshot().matched_feedback_frames==1&&motion.snapshot().feedback_error_degrees<.01,
            "test_motion_feedback_compares_uploaded_head_not_unsent_eye_rotation");
    }++groups;
    {auto frame=motion_fixture();surfxr::TestHeadMotion motion(true);motion.apply(frame);
        motion.feedback(1,100,frame.head.pose.orientation);motion.feedback(1,100,frame.head.pose.orientation);
        require(motion.snapshot().feedback_frames==1&&motion.snapshot().matched_feedback_frames==0,"test_motion_early_feedback_waits_for_actual_send");
        motion.sent(frame);motion.sent(frame);motion.feedback(1,100,frame.head.pose.orientation);
        require(motion.snapshot().feedback_frames==1&&motion.snapshot().matched_feedback_frames==1,"test_motion_early_feedback_reconciles_exactly_once");
        frame.sequence=2;motion.feedback(2,200,frame.head.pose.orientation);motion.feedback(2,201,frame.head.pose.orientation);
        motion.sent(frame);require(motion.snapshot().matched_feedback_frames==3,"test_motion_multiple_rendered_responses_for_repeated_pose_id");
        motion.feedback(3,300,frame.head.pose.orientation);frame.sequence=3;++frame.origin_epoch;++frame.sampled_monotonic_ns;
        motion.apply(frame);motion.sent(frame);
        require(motion.snapshot().matched_feedback_frames==3,"test_motion_epoch_change_discards_unmatched_old_feedback");
    }++groups;
    {auto frame=motion_fixture();surfxr::TestHeadMotion motion(true);motion.apply(frame);
        for(std::int32_t id=1;id<=1025;++id)motion.feedback(id,static_cast<std::uint64_t>(id),frame.head.pose.orientation);
        motion.sent(frame);require(motion.snapshot().matched_feedback_frames==0,"test_motion_pending_history_evicts_oldest_at_fixed_capacity");
        frame.sequence=2;motion.sent(frame);frame.sequence=1025;motion.sent(frame);
        require(motion.snapshot().feedback_frames==1025&&motion.snapshot().matched_feedback_frames==2,"test_motion_pending_history_keeps_recent_responses");
    }++groups;
    {const auto sample=[&](std::uint64_t delta){surfxr::TestHeadMotion motion(true);auto frame=motion_fixture();
            // Nonidentity anchor checks that velocity is in Stage coordinates.
            frame.head.pose.orientation={0,0,.38268343f,.92387953f};motion.apply(frame);
            frame.sampled_monotonic_ns+=delta;motion.apply(frame);return frame;};
        for(auto ns:{500'000'000ull,1'500'000'000ull,3'200'000'000ull}){
            const auto before=sample(ns-100'000),after=sample(ns+100'000),middle=sample(ns);
            const auto a=before.head.pose.orientation,b=after.head.pose.orientation,q=middle.head.pose.orientation;
            const double dx=(b.x-a.x)/.0002,dy=(b.y-a.y)/.0002,dz=(b.z-a.z)/.0002,dw=(b.w-a.w)/.0002;
            const surfxr::UiVec3 numerical{float(2*(-dw*q.x+dx*q.w-dy*q.z+dz*q.y)),
                float(2*(-dw*q.y+dx*q.z+dy*q.w-dz*q.x)),float(2*(-dw*q.z-dx*q.y+dy*q.x+dz*q.w))};
            const auto exact=middle.head.angular_velocity;
            require(std::abs(numerical.x-exact.x)<.001f&&std::abs(numerical.y-exact.y)<.001f&&std::abs(numerical.z-exact.z)<.001f,"test_motion_angular_velocity_matches_independent_quaternion_derivative");
        }
    }++groups;
    return groups;
}
}
int main(int argc,char** argv){if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::cout<<"surfxr-tracking-test --self-test\n";return argc==1?0:1;}
    try{const auto groups=tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"network_used\":false,\"openxr_session_started\":false}\n";return 0;}catch(const std::exception& e){std::cerr<<"tracking_self_test: "<<e.what()<<'\n';return 1;}}
