#include "display_private.hpp"
#include "desktop_input.hpp"
#include "boundary_sampling.hpp"
#include "refresh_rate.hpp"
#include "controller_tracking.hpp"

namespace surfxr {
void XrDisplay::Impl::refresh_stage_bounds(){
    stage_bounds_valid_=false;stage_bounds_={};if(!stage_space_)return;
    const auto result=xrGetReferenceSpaceBoundsRect(session_,XR_REFERENCE_SPACE_TYPE_STAGE,&stage_bounds_);
    xr_check(result,"xrGetReferenceSpaceBoundsRect(STAGE)");
    stage_bounds_valid_=result==XR_SUCCESS&&std::isfinite(stage_bounds_.width)&&std::isfinite(stage_bounds_.height)&&stage_bounds_.width>0&&stage_bounds_.height>0;
    if(!stage_bounds_valid_)stage_bounds_={};
}

TrackingPose XrDisplay::Impl::locate_tracking(XrSpace tracked,XrTime time,bool& target_valid){
    TrackingPose result;if(!stage_space_||!target_valid)return result;
    auto velocity=xr_info<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    auto location=xr_info<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);location.next=&velocity;
    const auto located=xrLocateSpace(tracked,stage_space_,time,&location);
    if(located==XR_ERROR_TIME_INVALID){++stats_.prediction_locate_time_invalid;target_valid=false;return result;}
    xr_check(located,"xrLocateSpace(Stage tracking)");
    result.flags=static_cast<std::uint32_t>(location.locationFlags)&15u;
    if(result.flags&OrientationValid){const auto q=location.pose.orientation;result.pose.orientation={q.x,q.y,q.z,q.w};}
    if(result.flags&PositionValid){const auto p=location.pose.position;result.pose.position={p.x,p.y,p.z};}
    const auto finite=[](XrVector3f v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
    if((velocity.velocityFlags&XR_SPACE_VELOCITY_LINEAR_VALID_BIT)&&finite(velocity.linearVelocity)){
        const auto v=velocity.linearVelocity;result.linear_velocity={v.x,v.y,v.z};result.linear_velocity_valid=true;}
    if((velocity.velocityFlags&XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)&&finite(velocity.angularVelocity)){
        const auto v=velocity.angularVelocity;result.angular_velocity={v.x,v.y,v.z};result.angular_velocity_valid=true;}
    return result;
}

TrackingProfile XrDisplay::Impl::tracking_profile(const std::string& name){
    if(name=="steam_frame")return TrackingProfile::SteamFrame;
    if(name=="oculus_touch")return TrackingProfile::OculusTouch;
    if(name=="valve_index")return TrackingProfile::ValveIndex;
    if(name=="simple_controller")return TrackingProfile::Simple;
    return name=="none"?TrackingProfile::None:TrackingProfile::Other;
}

void XrDisplay::Impl::discover_performance_metrics() noexcept {
    if(!stats_.performance_metrics_extension_enabled)return;
    try{
        const auto resolve=[&](const char* name){
            PFN_xrVoidFunction fn=nullptr;
            if(xrGetInstanceProcAddr(instance_,name,&fn)!=XR_SUCCESS||!fn){++stats_.render_latency_errors;return PFN_xrVoidFunction{};}
            return fn;
        };
        const auto enumerate=reinterpret_cast<PFN_xrEnumeratePerformanceMetricsCounterPathsMETA>(resolve("xrEnumeratePerformanceMetricsCounterPathsMETA"));
        set_metrics_state_=reinterpret_cast<PFN_xrSetPerformanceMetricsStateMETA>(resolve("xrSetPerformanceMetricsStateMETA"));
        query_metrics_counter_=reinterpret_cast<PFN_xrQueryPerformanceMetricsCounterMETA>(resolve("xrQueryPerformanceMetricsCounterMETA"));
        if(!enumerate||!set_metrics_state_||!query_metrics_counter_)return;
        std::uint32_t count=0;
        if(enumerate(instance_,0,&count,nullptr)!=XR_SUCCESS||count>4096){++stats_.render_latency_errors;return;}
        if(!count)return;
        std::vector<XrPath> paths(count);
        const auto capacity=count;
        if(enumerate(instance_,capacity,&count,paths.data())!=XR_SUCCESS||count>capacity){++stats_.render_latency_errors;return;}
        paths.resize(count);XrPath wanted=XR_NULL_PATH;
        if(xrStringToPath(instance_,"/perfmetrics_meta/app/motion_to_photon_latency",&wanted)!=XR_SUCCESS){++stats_.render_latency_errors;return;}
        if(std::find(paths.begin(),paths.end(),wanted)==paths.end())return;
        render_latency_path_=wanted;stats_.render_latency_supported=true;
    }catch(...){++stats_.render_latency_errors;render_latency_path_=XR_NULL_PATH;stats_.render_latency_supported=false;}
}

void XrDisplay::Impl::start_performance_metrics() noexcept {
    if(!stats_.render_latency_supported||!set_metrics_state_||stats_.render_latency_enabled)return;
    auto state=xr_info<XrPerformanceMetricsStateMETA>(XR_TYPE_PERFORMANCE_METRICS_STATE_META);state.enabled=XR_TRUE;
    if(set_metrics_state_(session_,&state)!=XR_SUCCESS){++stats_.render_latency_errors;return;}
    stats_.render_latency_enabled=true;next_metrics_query_={};
}

void XrDisplay::Impl::update_performance_metrics() noexcept {
    const auto now=Clock::now();
    if(stats_.render_latency_valid&&now-last_metrics_success_>std::chrono::seconds(1)){
        stats_.render_latency_valid=false;stats_.render_latency_ms=0;++stats_.render_latency_expirations;
    }
    if(!running_||!stats_.render_latency_enabled||!query_metrics_counter_||now<next_metrics_query_)return;
    next_metrics_query_=now+std::chrono::milliseconds(200);
    auto counter=xr_info<XrPerformanceMetricsCounterMETA>(XR_TYPE_PERFORMANCE_METRICS_COUNTER_META);
    if(query_metrics_counter_(session_,render_latency_path_,&counter)!=XR_SUCCESS){
        ++stats_.render_latency_errors;stats_.render_latency_valid=false;stats_.render_latency_ms=0;return;
    }
    // The standard META unit enum defines milliseconds, not seconds.
    // GENERIC and unknown units must not be guessed to mean seconds.
    double milliseconds=std::numeric_limits<double>::quiet_NaN();
    if(counter.counterFlags&XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)milliseconds=counter.floatValue;
    else if(counter.counterFlags&XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META)milliseconds=counter.uintValue;
    if(counter.counterUnit!=XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META
        ||!std::isfinite(milliseconds)||milliseconds<0||milliseconds>999){
        ++stats_.render_latency_invalid_samples;stats_.render_latency_valid=false;stats_.render_latency_ms=0;return;
    }
    // This is freshness of our successful query; the API does not expose
    // the counter's underlying sensor/compositor sample timestamp.
    last_metrics_success_=Clock::now();stats_.render_latency_valid=true;stats_.render_latency_ms=milliseconds;
    const auto count=++stats_.render_latency_samples;
    stats_.render_latency_ms_mean+=(milliseconds-stats_.render_latency_ms_mean)/double(count);
    stats_.render_latency_ms_max=std::max(stats_.render_latency_ms_max,milliseconds);
}

void XrDisplay::Impl::observe_refresh_rate(float actual){
    if(!std::isfinite(actual)||actual<=0||actual>1000)return;
    stats_.current_refresh_hz=actual;
    if(stats_.refresh_rate_pending&&std::abs(actual-stats_.requested_refresh_hz)<.01f)stats_.refresh_rate_pending=false;
}

void XrDisplay::Impl::update_refresh_rate(const RemoteSettingsSnapshot* settings){
    const double desired=xr_detail::display_refresh_target(settings);
    if(settings&&settings->capabilities.vr_only){
        if(std::abs(desired-last_refresh_preference_)>=.01){
            last_refresh_preference_=static_cast<float>(desired);stats_.requested_refresh_hz=desired;
            stats_.refresh_rate_pending=desired>0;
            if(desired>0)++stats_.refresh_rate_requests;
            if(!boundary_||!boundary_->request_refresh(static_cast<float>(desired))){
                stats_.last_refresh_request_result=-3;stats_.refresh_rate_pending=false;
                if(desired>0)++stats_.refresh_rate_request_failures;
            }
        }
        if(boundary_){
            const auto reply=boundary_->refresh();
            if(reply.serial&&reply.serial!=last_refresh_reply_&&std::abs(reply.requested-desired)<.01){
                last_refresh_reply_=reply.serial;stats_.last_refresh_request_result=reply.result;
                if(reply.result){++stats_.refresh_rate_request_failures;stats_.refresh_rate_pending=false;}
            }
            if(reply.actual>0)observe_refresh_rate(reply.actual);
        }
    }
    else if(!desired){last_refresh_preference_=0;stats_.refresh_rate_pending=false;}
    else if(request_refresh_rate_&&get_refresh_rate_){
        if(std::isfinite(desired)&&desired>0&&desired<=1000&&std::abs(desired-last_refresh_preference_)>=.01){
            last_refresh_preference_=static_cast<float>(desired);stats_.requested_refresh_hz=last_refresh_preference_;
            const auto supported=std::find_if(stats_.refresh_rates_hz.begin(),stats_.refresh_rates_hz.end(),
                [&](float rate){return std::abs(double(rate)-desired)<.01;});
            if(supported==stats_.refresh_rates_hz.end()){
                stats_.last_refresh_request_result=XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
                stats_.refresh_rate_pending=false;++stats_.refresh_rate_request_failures;
            }else if(std::abs(*supported-stats_.current_refresh_hz)>=.01f){
                ++stats_.refresh_rate_requests;
                const auto result=request_refresh_rate_(session_,*supported);stats_.last_refresh_request_result=static_cast<int>(result);
                stats_.refresh_rate_pending=result==XR_SUCCESS;
                if(result!=XR_SUCCESS)++stats_.refresh_rate_request_failures;
                // A successful request alone is not proof of a mode change.
                float actual=0;
                if(get_refresh_rate_(session_,&actual)==XR_SUCCESS)observe_refresh_rate(actual);
                else ++stats_.refresh_rate_query_failures;
            }else{stats_.last_refresh_request_result=XR_SUCCESS;stats_.refresh_rate_pending=false;}
        }
    }
    if(callbacks_.refresh_rate_changed&&stats_.current_refresh_hz>0&&std::abs(stats_.current_refresh_hz-last_reported_refresh_)>=.01f){
        if(callbacks_.refresh_rate_changed(stats_.current_refresh_hz))last_reported_refresh_=stats_.current_refresh_hz;
    }
}

void XrDisplay::Impl::input(XrTime local_display_time) {
    shortcut_timing_={};shortcut_timing_.presented_vr=vr_was_active_;
    const auto callback=[&](const UiEvent& e){dispatch(e);};
    bool vr_mode=vr_was_active_,frame_inputs=false,frame_session=false;
    if(callbacks_.settings){const auto settings=callbacks_.settings();ui_.set_remote_settings(settings);vr_mode|=settings.capabilities.vr_active;frame_inputs=settings.capabilities.frame_controller_inputs;update_refresh_rate(&settings);
        frame_session=settings.capabilities.connected&&settings.capabilities.vr_requested&&frame_inputs;
        shortcut_timing_.requested_vr=settings.capabilities.vr_requested;
        gaze_hud_.requested=settings.at(SettingId::FoveatedStreaming).requested!=0;}
    else update_refresh_rate(nullptr);
    if(shortcut_vr_requested_!=bool(shortcut_timing_.requested_vr)){
        shortcuts_.mode_changed();shortcut_vr_requested_=shortcut_timing_.requested_vr;
    }
    if(callbacks_.image_quality)ui_.set_image_quality(callbacks_.image_quality());
    if(callbacks_.input_preferences){
        const auto preferences=callbacks_.input_preferences();ui_.set_input_preferences(preferences);
        const bool collecting=performance_enabled_||mini_hud_preferences_.mini_hud_enabled;
        if(preferences!=mini_hud_preferences_){
            mini_hud_preferences_=preferences;next_mini_hud_update_={};mini_hud_visibility_.reset();
        }
        gaze_debug_enabled_=preferences.gaze_debug_overlay;
        if(preferences.performance_overlay!=performance_enabled_){
            performance_enabled_=preferences.performance_overlay;next_performance_update_={};
        }
        if(collecting!=(performance_enabled_||mini_hud_preferences_.mini_hud_enabled))performance_.reset();
    }
    while(!stage_changes_.empty()&&stage_changes_.front()<=local_display_time){
        invalidate_vr_reuse();
        stage_epoch_begin_time_=stage_changes_.front();
        ++stage_epoch_;++stats_.stage_origin_changes;stage_changes_.erase(stage_changes_.begin());
        refresh_stage_bounds();shortcuts_.reset();system_input_.suspend();ui_.release_inputs(callback);
    }
    motion_sync_.set_epoch(stage_epoch_);
    stats_.motion_sync_latency_ms=motion_sync_.statistics().latency_seconds*1000.;
    stats_.motion_sync_previous_draw_ms=motion_sync_.statistics().previous_draw_seconds*1000.;
    update_performance_metrics();
    TrackingFrame tracking;
    // Never subtract steady_clock's epoch directly from an opaque XrTime.
    // Bracket the system read to keep the sender's snapshot-age origin near
    // the exact CLOCK_MONOTONIC instant converted by the runtime.
    timespec monotonic{};const auto before_clock=Clock::now();
    const bool system_time_valid=clock_gettime(CLOCK_MONOTONIC,&monotonic)==0;
    const auto after_clock=Clock::now();const auto sampled=before_clock+(after_clock-before_clock)/2;
    tracking.sampled_monotonic_ns=clock_ns(sampled);
    XrTime current_xr=0;local_render_query_ns_=0;
    if(timespec_to_xr_&&system_time_valid&&timespec_to_xr_(instance_,&monotonic,&current_xr)==XR_SUCCESS){
        local_render_query_ns_=clock_ns(before_clock);
        const auto target=motion_sync_.target(current_xr);
        tracking.query_xr_time=current_xr;
        if(timestamped_tracking_.load(std::memory_order_acquire))tracking.predicted_xr_time=current_xr;
        else if(callbacks_.adaptive_prediction){
            if(target.valid)tracking.predicted_xr_time=target.xr_time;
        }else tracking.predicted_xr_time=local_display_time;
        const auto timing=set_tracking_prediction_time(tracking,current_xr);
        if(timing==TrackingPredictionStatus::Past)++stats_.prediction_time_past_samples;
        else if(timing==TrackingPredictionStatus::TooFar)++stats_.prediction_time_over_limit_samples;
        else if(timing==TrackingPredictionStatus::InvalidTime)++stats_.prediction_time_conversion_failures;
    }else ++stats_.prediction_time_conversion_failures;
    bool remote_target_valid=tracking.prediction_time_valid;
    if(remote_target_valid&&!motion_target_in_epoch(tracking.predicted_xr_time,stage_epoch_begin_time_,stage_changes_.empty()?0:stage_changes_.front())){
        // A future query can cross the next origin change, while an early
        // query can precede a change already applied for local display.
        // Neither can be labelled with the current presentation epoch.
        remote_target_valid=false;++stats_.prediction_stage_boundary_rejections;
    }
    if(remote_target_valid){
        tracking.query_started_monotonic_ns=clock_ns(before_clock);
        const auto count=++stats_.prediction_target_display_samples;
        const double delta=(double(tracking.predicted_xr_time)-double(local_display_time))*1e-6;
        stats_.prediction_target_display_delta_ms_sum+=delta;
        if(count==1)stats_.prediction_target_display_delta_ms_min=stats_.prediction_target_display_delta_ms_max=delta;
        else{
            stats_.prediction_target_display_delta_ms_min=std::min(stats_.prediction_target_display_delta_ms_min,delta);
            stats_.prediction_target_display_delta_ms_max=std::max(stats_.prediction_target_display_delta_ms_max,delta);
        }
    }
    tracking.sequence=++tracking_sequence_;tracking.origin_epoch=stage_epoch_;
    tracking.visible=state_==XR_SESSION_STATE_VISIBLE||state_==XR_SESSION_STATE_FOCUSED;
    tracking.stage_supported=static_cast<bool>(stage_space_);tracking.origin=stage_space_?TrackingOrigin::Stage:TrackingOrigin::Unavailable;
    tracking.stage_bounds_valid=stage_bounds_valid_;tracking.stage_width=stage_bounds_.width;tracking.stage_depth=stage_bounds_.height;
    if(remote_target_valid){
        tracking.head=locate_tracking(head_space_,tracking.predicted_xr_time,remote_target_valid);
        tracking.stage_locatable=tracking_pose_valid(tracking.head);
        if(!tracking.stage_locatable)++stats_.prediction_head_invalid;
    }
    if(stage_space_&&remote_target_valid){
        auto locate=xr_info<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;locate.displayTime=tracking.predicted_xr_time;locate.space=stage_space_;
        auto views_state=xr_info<XrViewState>(XR_TYPE_VIEW_STATE);std::array<XrView,2> views;for(auto& view:views)view=xr_info<XrView>(XR_TYPE_VIEW);
        std::uint32_t count=0;const auto located=xrLocateViews(session_,&locate,&views_state,2,&count,views.data());
        if(located==XR_ERROR_TIME_INVALID){++stats_.prediction_locate_time_invalid;remote_target_valid=false;}
        else xr_check(located,"xrLocateViews(Stage optics)");
        const auto valid=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if(remote_target_valid&&count==2&&(views_state.viewStateFlags&valid)==valid){
            for(unsigned eye=0;eye<2;++eye){tracking.eye_poses[eye]=ui_pose(views[eye].pose);const auto f=views[eye].fov;tracking.eye_fov[eye]={f.angleLeft,f.angleRight,f.angleUp,f.angleDown};}
            tracking.ipd_meters=boundary_?boundary_->ipd_meters():0;
            if(!tracking.ipd_meters){
                if(!boundary_||ipd_fallback_.due(tracking.sampled_monotonic_ns)){
                    const auto a=views[0].pose.position,b=views[1].pose.position;
                    ipd_fallback_.sampled(std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)),tracking.sampled_monotonic_ns);
                    ++stats_.ipd_fallback_samples;
                }
                tracking.ipd_meters=ipd_fallback_.value();
            }
            tracking.optics_valid=std::isfinite(tracking.ipd_meters)&&tracking.ipd_meters>0&&tracking.ipd_meters<.2f;
            for(unsigned eye=0;eye<2;++eye){
                TrackingPose pose;pose.pose=tracking.eye_poses[eye];pose.flags=OrientationValid|PositionValid;
                tracking.optics_valid&=tracking_pose_valid(pose);
                const auto& f=tracking.eye_fov[eye];
                tracking.optics_valid&=std::all_of(f.begin(),f.end(),[](float angle){return std::isfinite(angle)&&std::abs(angle)<1.570796327f;})
                    &&f[0]<f[1]&&f[3]<f[2];
            }
        }
        if(!tracking.optics_valid)++stats_.prediction_optics_invalid;
    }
    tracking.boundary_sync_enabled=ui_.input_preferences().boundary_sync_enabled;
    if(boundary_)tracking.boundary=boundary_->snapshot(stage_epoch_,tracking.boundary_sync_enabled&&
        tracking.optics_valid&&tracking_pose_tracked(tracking.head)&&
        xr_detail::boundary_time_ready(current_xr,stage_epoch_begin_time_,stage_changes_.empty()?0:stage_changes_.front()));
    if(boundary_){stats_.boundary_samples=boundary_->sample_count();
        stats_.ipd_samples=boundary_->ipd_samples();stats_.ipd_events=boundary_->ipd_events();}
    // UI geometry remains at the local display target, regardless of the
    // different prediction horizon used for the remote game render.
    auto head=xr_info<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    const auto local_head_result=xrLocateSpace(head_space_,space_,local_display_time,&head);
    if(local_head_result==XR_ERROR_TIME_INVALID){++stats_.prediction_locate_time_invalid;remote_target_valid=false;}
    else xr_check(local_head_result,"xrLocateSpace(head)");
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if((head.locationFlags&valid)==valid){head_pose_=ui_pose(head.pose);if(!ui_.anchored()){
        ui_.reset_anchor(head_pose_);
        if(initial_layout_){stats_.layout_restored=ui_.restore_layout(*initial_layout_);initial_layout_.reset();}
    }}
    allocate_curve_segments();
    std::array<UiHand,2> hands{};std::array<bool,2> menu_active{};
    XrActiveActionSet active{action_set_,XR_NULL_PATH};
    auto sync=xr_info<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);sync.countActiveActionSets=1;sync.activeActionSets=&active;
    const auto result=xrSyncActions(session_,&sync);++stats_.action_syncs;stats_.last_action_sync_result=static_cast<int>(result);
    if(result==XR_SESSION_NOT_FOCUSED)++stats_.action_not_focused_frames;
    else xr_check(result,"xrSyncActions");
    tracking.focused=state_==XR_SESSION_STATE_FOCUSED&&result==XR_SUCCESS;
    sample_gaze(tracking,local_display_time);
    update_haptics(tracking.focused);
    const auto battery_now=Clock::now();
    const bool query_battery=(stats_.battery_extension_enabled||boundary_)&&battery_now>=next_battery_query_;
    if(query_battery)next_battery_query_=battery_now+std::chrono::seconds(1);
    if(query_battery&&callbacks_.headset_battery){
        const auto status=boundary_?boundary_->headset_battery():xr_detail::HeadsetBatteryStatus{};
        callbacks_.headset_battery(status.battery(),status.sampled_ns);
    }
    std::array<DeviceBattery,2> controller_batteries{};
    const auto controller_status=boundary_?boundary_->controllers():xr_detail::ControllerStatus{};
    const auto controller_time=xr_detail::controller_tracking_time(tracking,
        stage_epoch_begin_time_,stage_changes_.empty()?0:stage_changes_.front());
    for(unsigned i=0;i<2;++i){
        auto profile=xr_info<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
        auto battery=xr_info<XrBatteryStateDisplayEXT>(XR_TYPE_BATTERY_STATE_DISPLAY_EXT);
        if(query_battery&&stats_.battery_extension_enabled)profile.next=&battery;
        auto profile_result=xrGetCurrentInteractionProfile(session_,hand_paths_[i],&profile);
        if(query_battery&&stats_.battery_extension_enabled){
            ++stats_.battery_query_samples;
            if(profile_result==XR_SUCCESS)controller_batteries[i]=battery_from_openxr(profile.interactionProfile,battery);
            else{
                // Optional telemetry must not stop tracking on a runtime
                // that fails the extension query. Retry ordinary profile.
                ++stats_.battery_query_failures;profile.next=nullptr;
                profile_result=xrGetCurrentInteractionProfile(session_,hand_paths_[i],&profile);
            }
        }
        xr_check(profile_result,"xrGetCurrentInteractionProfile");
        std::string name="other";
        if(profile.interactionProfile==XR_NULL_PATH)name="none";
        else for(const auto& item:std::array<std::pair<const char*,const char*>,4>{{
            {"/interaction_profiles/valve/frame_controller_valve","steam_frame"},
            {"/interaction_profiles/oculus/touch_controller","oculus_touch"},
            {"/interaction_profiles/valve/index_controller","valve_index"},
            {"/interaction_profiles/khr/simple_controller","simple_controller"}}})
            if(profile.interactionProfile==path(item.first)){name=item.second;break;}
        stats_.interaction_profiles[i]=name;
        auto& tracked_hand=tracking.hands[i];tracked_hand.profile=tracking_profile(name);
        controller_status.apply(i,tracked_hand,controller_batteries[i],tracking.sampled_monotonic_ns);
        if(result==XR_SESSION_NOT_FOCUSED||!ui_.anchored()){previous_right_click_[i]=false;if(i==0)previous_left_system_=false;continue;}
        auto get=xr_info<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);get.subactionPath=hand_paths_[i];
        get.action=aim_action_;auto pose=xr_info<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
        xr_check(xrGetActionStatePose(session_,&get,&pose),"xrGetActionStatePose");
        tracked_hand.aim_active=pose.isActive;
        if(tracked_hand.aim_active&&controller_time)tracked_hand.aim=locate_tracking(aim_spaces_[i],controller_time,remote_target_valid);
        get.action=grip_pose_action_;auto grip_pose=xr_info<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
        xr_check(xrGetActionStatePose(session_,&get,&grip_pose),"xrGetActionStatePose(grip)");
        tracked_hand.grip_active=grip_pose.isActive;
        if(tracked_hand.grip_active&&controller_time)tracked_hand.grip=locate_tracking(grip_spaces_[i],controller_time,remote_target_valid);
        auto located=xr_info<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        const auto local_aim_result=xrLocateSpace(aim_spaces_[i],space_,local_display_time,&located);
        if(local_aim_result==XR_ERROR_TIME_INVALID){++stats_.prediction_locate_time_invalid;remote_target_valid=false;}
        else xr_check(local_aim_result,"xrLocateSpace(aim)");
        // VALID alone can mean a stale/inferred pose after tracking loss.
        // Do not keep a PC mouse button or panel grab held on such a pose.
        constexpr auto tracked=valid|XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        hands[i].active=pose.isActive&&(located.locationFlags&tracked)==tracked;
        if(hands[i].active)hands[i].aim=ui_pose(located.pose);
        // Local UI attachments must share the panel/ray reference space and
        // display time, not the Stage pose predicted for PC video rendering.
        auto local_grip=xr_info<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        const auto grip_result=xrLocateSpace(grip_spaces_[i],space_,local_display_time,&local_grip);
        if(grip_result!=XR_ERROR_TIME_INVALID)xr_check(grip_result,"xrLocateSpace(local grip)");
        hands[i].attachment_valid=grip_pose.isActive&&grip_result==XR_SUCCESS&&(local_grip.locationFlags&tracked)==tracked;
        if(hands[i].attachment_valid)hands[i].attachment=ui_pose(local_grip.pose);
        const auto value=[&](XrAction a,std::uint32_t flag){get.action=a;auto v=xr_info<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);xr_check(xrGetActionStateFloat(session_,&get,&v),"xrGetActionStateFloat");
            if(v.isActive&&std::isfinite(v.currentState)){tracked_hand.analog_active|=flag;return std::clamp(v.currentState,0.f,1.f);}return 0.f;};
        tracked_hand.trigger=value(select_action_,TriggerActive);tracked_hand.squeeze=value(grip_action_,SqueezeActive);
        hands[i].trigger=tracked_hand.trigger;hands[i].squeeze=tracked_hand.squeeze;
        get.action=stick_action_;auto stick=xr_info<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
        xr_check(xrGetActionStateVector2f(session_,&get,&stick),"xrGetActionStateVector2f");
        if(stick.isActive&&std::isfinite(stick.currentState.x)&&std::isfinite(stick.currentState.y)){
            tracked_hand.analog_active|=ThumbstickActive;tracked_hand.stick_x=std::clamp(stick.currentState.x,-1.f,1.f);tracked_hand.stick_y=std::clamp(stick.currentState.y,-1.f,1.f);
            hands[i].stick_x=tracked_hand.stick_x;hands[i].stick_y=tracked_hand.stick_y;}
        get.action=menu_action_;auto menu=xr_info<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
        xr_check(xrGetActionStateBoolean(session_,&get,&menu),"xrGetActionStateBoolean");menu_active[i]=menu.isActive;hands[i].menu=menu.isActive&&menu.currentState;
        get.action=right_click_action_;auto right=xr_info<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
        xr_check(xrGetActionStateBoolean(session_,&get,&right),"xrGetActionStateBoolean(right_click)");
        hands[i].right_click=right.isActive&&right.currentState;
        for(const auto& raw:raw_buttons_){
            const auto read=[&](XrAction action,std::uint32_t& active_mask,std::uint32_t& state_mask){get.action=action;auto value=xr_info<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
                xr_check(xrGetActionStateBoolean(session_,&get,&value),"xrGetActionStateBoolean(raw tracking)");
                if(value.isActive){active_mask|=raw.bit;if(value.currentState)state_mask|=raw.bit;}};
            read(raw.click,tracked_hand.buttons_active,tracked_hand.buttons);read(raw.touch,tracked_hand.touches_active,tracked_hand.touches);
        }
        if(!vr_mode||ui_.keyboard_panel().visible)apply_desktop_buttons(hands[i],tracked_hand,i);
        if(right.isActive)++stats_.right_click_action_active_frames[i];
        if(hands[i].right_click&&!previous_right_click_[i])++stats_.right_click_press_edges[i];
        previous_right_click_[i]=hands[i].right_click;
        if(i==0){
            const bool active=tracked_hand.profile==TrackingProfile::SteamFrame&&(tracked_hand.buttons_active&ButtonSystem);
            const bool down=active&&(tracked_hand.buttons&ButtonSystem);
            if(active)++stats_.left_system_active_frames;
            if(down&&!previous_left_system_)++stats_.left_system_press_edges;
            previous_left_system_=down;
        }
        if(hands[i].active){++stats_.active_hand_frames;++stats_.hand_active_frames[i];}
    }
    if(query_battery&&callbacks_.controller_batteries)callbacks_.controller_batteries(controller_batteries);
    const bool capture_eligible=frame_session&&vr_mode&&tracking.focused&&tracking.visible&&
        tracking.prediction_time_valid&&tracking.stage_locatable&&tracking.optics_valid&&
        ui_.input_preferences().vr_controllers_enabled&&!exit_requested_&&
        tracking.hands[0].profile==TrackingProfile::SteamFrame&&tracking.hands[1].profile==TrackingProfile::SteamFrame;
    frame_input::Snapshot system;
    system_input_.session(frame_session);
    // Desktop still observes physical System clicks to cancel local gestures;
    // observation depends on local controllers, not a prior VR configuration.
    const bool frame_system=tracking.hands[0].profile==TrackingProfile::SteamFrame||tracking.hands[1].profile==TrackingProfile::SteamFrame;
    if(frame_system)system=system_input_.poll(capture_eligible,frame_input::monotonic_ns());
    else system_input_.close();
    tracking.system_capture_active=system.capture;
    stats_.system_click_available=system.available;stats_.system_touch_available=system.touch_available;
    stats_.system_capture_active=system.capture;stats_.system_capture_exit_serial=system.exit_serial;stats_.controller_skeleton_active_mask=0;
    for(unsigned hand=0;hand<2;++hand){const auto& source=system.skeletons[hand];auto& target=tracking.controller_skeletons[hand];
        target.sampled_monotonic_ns=source.sampled_ns;
        if(source.sampled_ns){stats_.controller_skeleton_active_mask|=1u<<hand;
            ++(hand?stats_.controller_skeleton_right_frames:stats_.controller_skeleton_left_frames);}
        if(source.sampled_ns)for(unsigned joint=0;joint<31;++joint){const auto& b=source.bones[joint];
            target.bones[joint].position={b.position[0],b.position[1],b.position[2]};
            target.bones[joint].orientation={b.orientation[0],b.orientation[1],b.orientation[2],b.orientation[3]};
        }
    }
    ui_.set_system_capture_status(capture_eligible&&system.available==3&&system.touch_available==3,system.capture);
    if(frame_inputs)for(unsigned i=0;i<2;++i){auto& hand=tracking.hands[i];
        hand.buttons&=~ButtonSystem;hand.buttons_active&=~ButtonSystem;
        hand.touches&=~ButtonSystem;hand.touches_active&=~ButtonSystem;
        if(system.available&(1u<<i)){hand.buttons_active|=ButtonSystem;if(system.clicks&(1u<<i))hand.buttons|=ButtonSystem;}
        if(system.touch_available&(1u<<i)){hand.touches_active|=ButtonSystem;if(system.touches&(1u<<i))hand.touches|=ButtonSystem;}
    }
    const std::array<TrackingProfile,2> profiles{{tracking.hands[0].profile,tracking.hands[1].profile}};
    if(profiles!=shortcut_profiles_){shortcuts_.reset();shortcut_profiles_=profiles;}
    const auto buttons=shortcut_buttons(tracking,vr_mode,menu_active,{{hands[0].menu,hands[1].menu}});
    const auto gesture=shortcuts_.update(tracking.sampled_monotonic_ns,buttons.active,buttons.down,buttons.system_down||system.clicks!=0);
    const auto mask=[](const std::array<bool,2>& bits){return unsigned(bits[0])|(unsigned(bits[1])<<1);};
    shortcut_timing_.input_sample_ns=tracking.sampled_monotonic_ns;shortcut_timing_.system_sample_ns=system.sampled_ns;
    shortcut_timing_.shortcut_active=mask(buttons.active);shortcut_timing_.shortcut_down=mask(buttons.down);shortcut_timing_.shortcut_keys=mask(gesture.keyboard);
    shortcut_timing_.system_available=system.available;shortcut_timing_.system_clicks=system.clicks;shortcut_timing_.system_capture=system.capture;
    shortcut_timing_.input_profile_left=static_cast<unsigned>(profiles[0]);shortcut_timing_.input_profile_right=static_cast<unsigned>(profiles[1]);
    shortcut_timing_.input_focused=tracking.focused;shortcut_timing_.shortcut_guard=shortcuts_.guarding();
    shortcut_timing_.shortcut_toggle=gesture.toggle_mode;shortcut_timing_.shortcut_reset_serial=shortcuts_.reset_serial();
    const bool startup_shown=startup_hud_presented_&&startup_hud_.visible()&&mini_hud_preferences_.transition_hud_enabled;
    if(startup_shown&&gesture.keyboard[1])startup_hud_.dismiss();
    for(unsigned i=0;i<2;++i){
        hands[i].menu=false;
        if(gesture.keyboard[i]){++stats_.shortcut_single_clicks;if(!(startup_shown&&i==1))ui_.toggle_keyboard(callback);}
    }
    if(gesture.toggle_mode){++stats_.shortcut_double_clicks;ui_.release_inputs(callback);if(callbacks_.toggle_mode)callbacks_.toggle_mode();}
    const auto now=Clock::now();const float dt=previous_frame_==Clock::time_point{}?0.f:std::chrono::duration<float>(now-previous_frame_).count();previous_frame_=now;
    const bool candidate_active=candidate_presented_&&candidate_image_&&callbacks_.keyboard_overlay&&callbacks_.keyboard_overlay->clickable.load()&&
        Clock::now()-candidate_image_->received<std::chrono::milliseconds(750)&&!pc_input_blocked_;
    ui_.candidate(candidate_active?candidate_image_->width:0,candidate_active?candidate_image_->height:0,candidate_active?candidate_image_->token:0);
    ui_.update(hands,dt,callback);ui_hands_=hands;
    tracking.ui_captured=ui_.input_capture_mask();
    if(callbacks_.keyboard_overlay)callbacks_.keyboard_overlay->active.store(
        tracking.focused&&tracking.visible&&ui_.keyboard_panel().visible&&ui_.keyboard_panel().available&&!ui_.settings_visible());
    for(auto& hand:tracking.hands){
        const bool invalid_aim=hand.aim_active&&!tracking_pose_valid(hand.aim);
        const bool invalid_grip=hand.grip_active&&!tracking_pose_valid(hand.grip);
        if(invalid_aim||invalid_grip)++stats_.prediction_controller_invalid;
        if(invalid_aim){hand.aim={};hand.aim_active=false;}
        // Aim drives the Quest wire pose; unavailable grip must not erase
        // an independently valid aim or the raw controller input snapshot.
        if(invalid_grip){hand.grip={};hand.grip_active=false;}
    }
    if(!remote_target_valid||!tracking.stage_locatable||!tracking.optics_valid){
        // Invalid future locations must not be filled with local-display
        // poses. A neutral uplink sample also cannot train the estimator.
        tracking.prediction_time_valid=false;tracking.prediction_lead_seconds=0;
        tracking.query_started_monotonic_ns=0;tracking.query_xr_time=0;tracking.predicted_xr_time=0;
        tracking.head={};tracking.eye_poses={};tracking.eye_fov={};tracking.hands={};
        tracking.stage_locatable=false;tracking.optics_valid=false;tracking.ipd_meters=0;
    }
    if(!tracking.prediction_time_valid||!tracking.optics_valid)tracking.gaze={};
    gaze_hud_.sample=tracking.gaze;gaze_hud_.current_epoch=tracking.origin_epoch;
    gaze_hud_.supported=stats_.eye_gaze_supported;
    gaze_debug_fov_=tracking.eye_fov;
    for(unsigned eye=0;eye<2;++eye)
        gaze_debug_eye_poses_[eye]=ui_compose(ui_inverse(tracking.head.pose),tracking.eye_poses[eye]);
    ++stats_.tracking_samples;if(tracking.stage_locatable)++stats_.stage_locatable_samples;
    stage_locatable_=tracking.stage_locatable;
    if(tracking.prediction_time_valid){
        const double lead_ms=double(tracking.prediction_lead_seconds)*1000.;++stats_.prediction_time_valid_samples;
        stats_.prediction_lead_ms_mean+=(lead_ms-stats_.prediction_lead_ms_mean)/double(stats_.prediction_time_valid_samples);
        stats_.prediction_lead_ms_max=std::max(stats_.prediction_lead_ms_max,lead_ms);
    }else ++stats_.prediction_time_invalid_samples;
    if(callbacks_.tracking)callbacks_.tracking(tracking);
    if(timestamped_tracking_.load(std::memory_order_acquire))
        current_tracking_.publish(instance_,timespec_to_xr_,xrLocateSpace,stage_space_,head_space_,grip_spaces_,tracking,
            stage_epoch_begin_time_,stage_changes_.empty()?0:stage_changes_.front());
    stats_.panel_drag_updates=ui_.drag_updates();stats_.panel_scale_updates=ui_.scale_updates();
}

} // namespace surfxr
