#include "viewer_ui.hpp"
#include "dashboard_wakeup.hpp"
#include <iostream>
#include "surfxr/surfxr_modes.hpp"
#include "surfxr/surfxr_feedback.hpp"

namespace surfxr::viewer {
bool ViewerUi::apply_quality(surfxr::ImageQualityChange change){
        const bool accepted=surfxr::apply_image_quality_change(image_quality,change);
        if(accepted)image_quality_store.submit(image_quality);
        next_status_sample={};return accepted;
    }
bool ViewerUi::apply_input_preference(surfxr::InputPreferenceChange change){
        const bool accepted=surfxr::apply_input_preference_change(input_preferences,change);
        if(accepted){
            controllers_enabled.store(input_preferences.vr_controllers_enabled,std::memory_order_relaxed);
            battery_monitor.set_sync_raw_battery(input_preferences.sync_raw_battery);
            input_preferences_store.submit(input_preferences);
        }
        next_status_sample={};return accepted;
    }

surfxr::XrUiCallbacks ViewerUi::callbacks(){
    surfxr::XrUiCallbacks ui;if(opts.session.backend==SessionBackend::Surfxr)ui.keyboard_overlay=opts.session.keyboard_overlay;
    ui.renderable=[this](bool value){vr.set_renderable(value);if(!value&&desktop_demand.enabled()){desktop_demand.set_mask(0);vr.set_desktop_renderable(false);}};
    if(desktop_demand.enabled())ui.desktop_demand=[this](unsigned mask){desktop_demand.set_mask(mask);vr.set_desktop_renderable((mask&1)!=0);};
    ui.desktop_capture_status=[this]{return surfxr::DesktopCaptureStatus{desktop_demand.capture_supported.load(),desktop_demand.capture_requested.load(),desktop_demand.capture_accepted.load()};};
    ui.desktop_capture_request=[this](unsigned mask){desktop_demand.capture_requested.store(mask);};
    ui.frame_presented=[this](bool vr_mode){
        if(result.runtime_status->presented(vr_mode)){
            fast_status_until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            next_status_sample={};
        }
    };
    ui.frame_observed=[this]{
        const auto now=std::chrono::steady_clock::now();
        if(!opts.status_path.empty()&&now>=next_status_sample){
            next_status_sample=now+(now<fast_status_until?std::chrono::milliseconds(50):std::chrono::milliseconds(2000));
            const auto stats=display.statistics();surfxr::ViewerTelemetry metrics;
            if(const auto& keyboard=opts.session.keyboard_overlay){metrics.keyboard_active=keyboard->active.load();
                metrics.keyboard_supported=keyboard->supported.load();metrics.keyboard_images_received=keyboard->received_images.load();}
            metrics.keyboard_candidate_frames=stats.keyboard_candidate_frames;
            if(opts.session.surfxr_feedback)metrics.surfxr_receive=opts.session.surfxr_feedback->receive.snapshot();
            metrics.sample_monotonic_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
            metrics.decode=decode_telemetry.snapshot();
            metrics.test_head_motion=test_head_motion.snapshot();
            metrics.haptics=result.haptics->statistics();
            metrics.desktop_frames=stats.desktop_submitted_frames;metrics.desktop_intervals=stats.desktop_submit_intervals;
            metrics.desktop_interval_ms=stats.desktop_submit_interval_ms_sum;metrics.stage_calls=stats.desktop_stage_calls;
            metrics.stage_ms=stats.desktop_stage_ms_sum;metrics.projection_calls=stats.desktop_projection_calls;
            metrics.projection_ms=stats.desktop_projection_ms_sum;metrics.desktop_new_frames=stats.desktop_new_frames[0];
            metrics.desktop_new_intervals=stats.desktop_new_frame_intervals[0];metrics.desktop_new_interval_ms=stats.desktop_new_frame_interval_ms_sum[0];
#define COPY_XR_TELEMETRY(name) metrics.name=stats.name
            COPY_XR_TELEMETRY(projection_width);COPY_XR_TELEMETRY(projection_height);COPY_XR_TELEMETRY(desktop_visible_mask);
            COPY_XR_TELEMETRY(desktop_layout_fingerprint);COPY_XR_TELEMETRY(desktop_decoded_width);COPY_XR_TELEMETRY(desktop_decoded_height);
            metrics.desktop_panel_new_frames=stats.desktop_new_frames;
            COPY_XR_TELEMETRY(desktop_stage_copy_bytes);COPY_XR_TELEMETRY(desktop_stage_copy_ms_sum);
            COPY_XR_TELEMETRY(desktop_direct_upload_frames);COPY_XR_TELEMETRY(desktop_direct_upload_bytes);COPY_XR_TELEMETRY(desktop_edge_patch_bytes);
            COPY_XR_TELEMETRY(desktop_uploads);COPY_XR_TELEMETRY(desktop_cached_upload_skips);
            COPY_XR_TELEMETRY(desktop_swapchain_wait_calls);COPY_XR_TELEMETRY(desktop_swapchain_wait_ms_sum);
            COPY_XR_TELEMETRY(desktop_end_frame_ms_sum);COPY_XR_TELEMETRY(desktop_frame_work_ms_sum);
            COPY_XR_TELEMETRY(desktop_gpu_timestamps);COPY_XR_TELEMETRY(desktop_mip_gpu_samples);COPY_XR_TELEMETRY(desktop_projection_gpu_samples);
            COPY_XR_TELEMETRY(desktop_mip_gpu_ms_sum);COPY_XR_TELEMETRY(desktop_mip_gpu_ms_max);
            COPY_XR_TELEMETRY(desktop_projection_gpu_ms_sum);COPY_XR_TELEMETRY(desktop_projection_gpu_ms_max);
            COPY_XR_TELEMETRY(desktop_nv12_encoded_mip_updates);COPY_XR_TELEMETRY(desktop_nv12_mip_updates);COPY_XR_TELEMETRY(desktop_nv12_intermediate_bytes_avoided);
            COPY_XR_TELEMETRY(desktop_nv12_base_updates);COPY_XR_TELEMETRY(desktop_nv12_base_reuses);COPY_XR_TELEMETRY(desktop_nv12_encoded_base_updates);
            COPY_XR_TELEMETRY(desktop_nv12_gpu_samples);COPY_XR_TELEMETRY(desktop_nv12_gpu_ms_sum);
            COPY_XR_TELEMETRY(vr_timewarp_frames);COPY_XR_TELEMETRY(vr_timewarp_fallback_frames);
            COPY_XR_TELEMETRY(vr_submitted_frames);COPY_XR_TELEMETRY(vr_new_stereo_frames);COPY_XR_TELEMETRY(vr_reused_stereo_frames);
            COPY_XR_TELEMETRY(vr_submit_intervals);COPY_XR_TELEMETRY(vr_submit_interval_ms_sum);
            COPY_XR_TELEMETRY(vr_decoded_age_samples);COPY_XR_TELEMETRY(vr_decoded_age_ms_sum);
            COPY_XR_TELEMETRY(vr_pose_sample_age_samples);COPY_XR_TELEMETRY(vr_pose_sample_age_ms_sum);
            COPY_XR_TELEMETRY(vr_submitted_pose_age_samples);COPY_XR_TELEMETRY(vr_submitted_pose_age_ms_sum);COPY_XR_TELEMETRY(vr_submitted_pose_age_ms_max);
            COPY_XR_TELEMETRY(vr_foveation_frames);COPY_XR_TELEMETRY(vr_foveation_gpu_wait_ms_sum);
            COPY_XR_TELEMETRY(vr_deferred_gpu_submissions);COPY_XR_TELEMETRY(vr_deferred_gpu_completions);COPY_XR_TELEMETRY(vr_foveation_gpu_samples);
            COPY_XR_TELEMETRY(vr_upload_wait_calls);COPY_XR_TELEMETRY(vr_upload_wait_ms_sum);COPY_XR_TELEMETRY(vr_upload_wait_ms_max);
            COPY_XR_TELEMETRY(vr_foveation_gpu_ms_sum);COPY_XR_TELEMETRY(vr_foveation_gpu_ms_max);
            COPY_XR_TELEMETRY(vr_stage_calls);COPY_XR_TELEMETRY(vr_stage_ms_sum);COPY_XR_TELEMETRY(vr_stage_ms_max);
            COPY_XR_TELEMETRY(vr_stage_copy_ms_sum);COPY_XR_TELEMETRY(vr_stage_copy_ms_max);
            COPY_XR_TELEMETRY(vr_direct_upload_frames);COPY_XR_TELEMETRY(vr_direct_upload_bytes);
            COPY_XR_TELEMETRY(vr_direct_sample_frames);COPY_XR_TELEMETRY(vr_upload_avoided_bytes);
            COPY_XR_TELEMETRY(vr_swapchain_wait_calls);COPY_XR_TELEMETRY(vr_swapchain_wait_ms_sum);COPY_XR_TELEMETRY(vr_swapchain_wait_ms_max);
            COPY_XR_TELEMETRY(vr_end_frame_ms_sum);COPY_XR_TELEMETRY(vr_end_frame_ms_max);COPY_XR_TELEMETRY(vr_frame_work_ms_sum);COPY_XR_TELEMETRY(vr_frame_work_ms_max);
            COPY_XR_TELEMETRY(motion_sync_updates);COPY_XR_TELEMETRY(motion_sync_latency_ms);COPY_XR_TELEMETRY(motion_sync_previous_draw_ms);
            COPY_XR_TELEMETRY(surfxr_timing_available);COPY_XR_TELEMETRY(surfxr_total_ms);COPY_XR_TELEMETRY(surfxr_upstream_ms);
            COPY_XR_TELEMETRY(surfxr_decode_ms);COPY_XR_TELEMETRY(surfxr_after_decode_ms);
            COPY_XR_TELEMETRY(motion_sync_gpu_estimated_frames);COPY_XR_TELEMETRY(motion_sync_gpu_unavailable_frames);
            COPY_XR_TELEMETRY(motion_sync_gpu_tail_ms_sum);COPY_XR_TELEMETRY(motion_sync_gpu_tail_ms_max);
            COPY_XR_TELEMETRY(prediction_time_valid_samples);COPY_XR_TELEMETRY(prediction_lead_ms_mean);
            COPY_XR_TELEMETRY(ipd_samples);COPY_XR_TELEMETRY(ipd_events);COPY_XR_TELEMETRY(ipd_fallback_samples);
            COPY_XR_TELEMETRY(current_refresh_hz);COPY_XR_TELEMETRY(render_latency_ms);COPY_XR_TELEMETRY(render_latency_valid);
            COPY_XR_TELEMETRY(requested_refresh_hz);COPY_XR_TELEMETRY(refresh_rate_pending);
            COPY_XR_TELEMETRY(refresh_rate_requests);COPY_XR_TELEMETRY(refresh_rate_request_failures);COPY_XR_TELEMETRY(last_refresh_request_result);
            COPY_XR_TELEMETRY(system_click_available);COPY_XR_TELEMETRY(system_touch_available);COPY_XR_TELEMETRY(system_capture_active);
            COPY_XR_TELEMETRY(haptic_apply_requests);COPY_XR_TELEMETRY(haptic_apply_accepted);
            COPY_XR_TELEMETRY(haptic_stop_requests);COPY_XR_TELEMETRY(haptic_stop_accepted);
            COPY_XR_TELEMETRY(haptic_suppressed);COPY_XR_TELEMETRY(haptic_errors);COPY_XR_TELEMETRY(last_haptic_result);COPY_XR_TELEMETRY(focused_frames);
            COPY_XR_TELEMETRY(system_capture_exit_serial);COPY_XR_TELEMETRY(controller_skeleton_active_mask);
            COPY_XR_TELEMETRY(controller_skeleton_left_frames);COPY_XR_TELEMETRY(controller_skeleton_right_frames);
            COPY_XR_TELEMETRY(transition_frames);
            COPY_XR_TELEMETRY(transition_cached_frames);
            COPY_XR_TELEMETRY(transition_placeholder_frames);
            COPY_XR_TELEMETRY(last_layer_count);
            COPY_XR_TELEMETRY(last_enabled_desktop_mask);
            COPY_XR_TELEMETRY(last_ready_desktop_mask);
            COPY_XR_TELEMETRY(last_cached_desktop_mask);
            COPY_XR_TELEMETRY(last_should_render);
            COPY_XR_TELEMETRY(last_ui_anchored);
            COPY_XR_TELEMETRY(last_vr_requested);
            COPY_XR_TELEMETRY(last_vr_pair_valid);
            COPY_XR_TELEMETRY(last_stage_locatable);
            COPY_XR_TELEMETRY(last_transition_pending);
#undef COPY_XR_TELEMETRY
            metrics.vr_eye_width=stats.vr_eye_width[0];metrics.vr_eye_height=stats.vr_eye_height[0];
            metrics.vr_decoded_eye_width=stats.vr_decoded_eye_width[0];metrics.vr_decoded_eye_height=stats.vr_decoded_eye_height[0];
            const auto mode_stats=vr.statistics();
            metrics.vr_paired_frames=mode_stats.paired_frames;metrics.vr_queued_presentations=mode_stats.queued_presentations;
            metrics.vr_pair_queue_overflows=mode_stats.paired_queue_overflows;
            metrics.vr_pair_queue_stale=mode_stats.paired_queue_stale;metrics.vr_pair_queue_skipped=mode_stats.paired_queue_skipped;
            metrics.vr_pair_queue_depth=mode_stats.paired_queue_depth;metrics.vr_playout_buffer_holds=mode_stats.playout_buffer_holds;
            metrics.vr_playout_reserve_ms=mode_stats.playout_reserve_ns/1e6;
            metrics.vr_keyframe_recoveries=mode_stats.vr_keyframe_recoveries;
            metrics.vr_restart_recoveries=mode_stats.vr_restart_recoveries;
            metrics.mode_source_mismatches=mode_stats.source_mismatches;
            metrics.desktop_source_retries=mode_stats.desktop_source_retries;
            metrics.desktop_keyframe_recoveries=mode_stats.desktop_keyframe_recoveries;
            metrics.desktop_restart_recoveries=mode_stats.desktop_restart_recoveries;
            metrics.desktop_timeouts=mode_stats.desktop_timeouts;
            metrics.desktop_capture_supported=desktop_demand.capture_supported.load();metrics.desktop_capture_requested_mask=desktop_demand.capture_requested.load();metrics.desktop_capture_accepted_mask=desktop_demand.capture_accepted.load();
            metrics.desktop_pending=mode_stats.desktop_pending;metrics.desktop_failed=mode_stats.desktop_failed;
            const auto remote=settings.snapshot();
            metrics.requested_desktop_pointer_priority=remote.at(surfxr::SettingId::DesktopPointerPriority).requested;
            metrics.requested_vr_fps=remote.at(surfxr::SettingId::VrFramerate).requested;
            metrics.requested_vr_quality=remote.at(surfxr::SettingId::VrGraphicsQuality).requested;
            metrics.requested_vr_bitrate_mbps=remote.at(surfxr::SettingId::VrBitrateMbps).requested;
            metrics.requested_foveated_streaming=remote.at(surfxr::SettingId::FoveatedStreaming).requested!=0;
            metrics.eye_gaze_available=remote.capabilities.eye_gaze_available;
            metrics.eye_gaze_samples=stats.eye_gaze_samples;metrics.eye_gaze_invalid=stats.eye_gaze_invalid;
            metrics.vr_foveated_streaming=remote.capabilities.vr_active&&remote.at(surfxr::SettingId::FoveatedStreaming).actual.value_or(0)!=0;
            metrics.vr_focus_width=stats.vr_focus_width;metrics.vr_focus_height=stats.vr_focus_height;
            metrics.vr_focus_center_x=stats.vr_focus_center_x;metrics.vr_focus_center_y=stats.vr_focus_center_y;
            metrics.active_codec=remote.capabilities.active_codec;
            metrics.max_vr_bitrate_mbps=remote.capabilities.surfxr_video_controls?1000:surfxr::vr_bitrate_max_mbps(metrics.active_codec);
            metrics.max_vr_quality=remote.capabilities.max_vr_quality;
            metrics.last_setting_request_id=last_setting_id;metrics.last_setting_request_value=last_setting_value;
            metrics.last_setting_request_accepted=last_setting_accepted;metrics.last_setting_request_serial=last_setting_serial;
            metrics.desktop_filter=image_quality.desktop_filter;metrics.vr_upscaling=image_quality.vr_upscaling;
            metrics.vr_timewarp=image_quality.vr_timewarp;metrics.vr_sharpening=image_quality.vr_sharpening;metrics.vr_sharpening_enabled=image_quality.vr_sharpening_enabled;
            metrics.last_quality_request_id=last_quality_id;metrics.last_quality_request_value=last_quality_value;
            metrics.last_quality_request_accepted=last_quality_accepted;metrics.last_quality_request_serial=last_quality_serial;
            metrics.vr_controllers_enabled=input_preferences.vr_controllers_enabled;
            metrics.performance_overlay=input_preferences.performance_overlay;
            metrics.gaze_debug_overlay=input_preferences.gaze_debug_overlay;
            metrics.boundary_sync_enabled=input_preferences.boundary_sync_enabled;
            metrics.sync_raw_battery=input_preferences.sync_raw_battery;
            metrics.transition_hud_enabled=input_preferences.transition_hud_enabled;
            metrics.mini_hud_enabled=input_preferences.mini_hud_enabled;
            metrics.mini_hud_fps=input_preferences.mini_hud_fps;
            metrics.mini_hud_latency=input_preferences.mini_hud_latency;
            metrics.mini_hud_frame_time=input_preferences.mini_hud_frame_time;
            metrics.mini_hud_decode=input_preferences.mini_hud_decode;
            metrics.mini_hud_gaze_only=input_preferences.mini_hud_gaze_only;
            metrics.mini_hud_gaze_marker=input_preferences.mini_hud_gaze_marker;
            metrics.mini_hud_x=input_preferences.mini_hud_x;
            metrics.mini_hud_y=input_preferences.mini_hud_y;
            metrics.mini_hud_scale=input_preferences.mini_hud_scale;
            metrics.gaze_debug_frames=stats.gaze_debug_frames;metrics.gaze_debug_eye_layers=stats.gaze_debug_eye_layers;
            metrics.gaze_debug_budget_omissions=stats.gaze_debug_budget_omissions;
            metrics.performance_overlay_frames=stats.performance_overlay_frames;
            metrics.performance_overlay_updates=stats.performance_overlay_updates;
            metrics.performance_overlay_budget_omissions=stats.performance_overlay_budget_omissions;
            metrics.last_input_request_id=last_input_id;metrics.last_input_request_value=last_input_value;
            metrics.last_input_request_serial=last_input_serial;metrics.last_input_request_accepted=last_input_accepted;
            const auto battery=battery_monitor.snapshot();
            metrics.hmd_raw_battery_percent=battery_monitor.raw_headset().percent;
            metrics.hmd_battery_percent=battery.hmd.percent;metrics.hmd_battery_charging=battery.hmd.charging;
            metrics.left_battery_percent=battery.controllers[0].percent;metrics.left_battery_charging=battery.controllers[0].charging;
            metrics.right_battery_percent=battery.controllers[1].percent;metrics.right_battery_charging=battery.controllers[1].charging;
            metrics.desktop_quality_frames=stats.desktop_quality_frames;metrics.desktop_mip_updates=stats.desktop_mip_updates;
            metrics.desktop_mip_reuses=stats.desktop_mip_reuses;metrics.vr_enhancement_frames=stats.vr_enhancement_frames;
            metrics.vr_upscaled_frames=stats.vr_upscaled_frames;metrics.vr_enhancement_gpu_wait_ms_sum=stats.vr_enhancement_gpu_wait_ms_sum;
            metrics.vr_enhancement_gpu_samples=stats.vr_enhancement_gpu_samples;metrics.vr_enhancement_gpu_ms_sum=stats.vr_enhancement_gpu_ms_sum;
            metrics.vr_raw_enhancement_frames=stats.vr_raw_enhancement_frames;
            metrics.vr_fused_postprocess_frames=stats.vr_fused_postprocess_frames;
            metrics.vr_enhanced_width=stats.vr_enhanced_width;metrics.vr_enhanced_height=stats.vr_enhanced_height;
            result.runtime_status->telemetry(metrics);
            if(timing_trace_enabled||timing_trace_pending){
                result.runtime_status->timing(display.timing_snapshot());timing_trace_pending=false;
            }
        }
    };
    ui.haptics=result.haptics;
    ui.event=[this](const surfxr::UiEvent& event){
        if(event.type==surfxr::UiEventType::Exit){input->release_all();stop_requested.store(true);}
        else if(event.type==surfxr::UiEventType::ShowDashboard){input->release_all();if(!show_dashboard())std::cerr<<"dashboard_wakeup_unavailable\n";}
        else if(event.type==surfxr::UiEventType::ImageQualityChanged)apply_quality(event.image_quality);
        else if(event.type==surfxr::UiEventType::ShortcutChanged){
            auto& shortcuts=input_preferences.input_shortcuts;
            if(!event.pressed&&event.button<shortcuts.size()){shortcuts.erase(shortcuts.begin()+event.button);input_preferences_store.submit(input_preferences);}
            else if(event.pressed&&event.button<=shortcuts.size()&&event.button<surfxr::kMaxKeyboardShortcuts&&surfxr::valid_keyboard_shortcut(event.shortcut)){
                if(event.button==shortcuts.size())shortcuts.push_back(event.shortcut);else shortcuts[event.button]=event.shortcut;
                input_preferences_store.submit(input_preferences);
            }
        }
        else if(event.type==surfxr::UiEventType::InputPreferenceChanged)apply_input_preference(event.input_preference);
        else if(event.type==surfxr::UiEventType::RemoteSettingChanged){
            if(vr.setting(event.setting)){
                if(event.setting.id==surfxr::SettingId::MicrophoneEnabled)microphone_enabled.store(event.setting.value!=0);
                settings_store.submit(settings.snapshot());
            }
        }
        else if(event.type==surfxr::UiEventType::LaunchVr){mode_commands.poll();requested_mode.reset();input->release_all();vr.launch();}
        else if(event.type==surfxr::UiEventType::ExitVr){mode_commands.poll();requested_mode.reset();input->release_all();vr.exit_vr();}
        else if(event.type==surfxr::UiEventType::ToggleMode){mode_commands.poll();requested_mode.reset();input->release_all();if(opts.session.surfxr_modes)opts.session.surfxr_modes->request(!opts.session.surfxr_modes->desired_desktop());else vr.toggle();}
        else if(event.type==surfxr::UiEventType::ShowDesktop){mode_commands.poll();requested_mode.reset();input->release_all();vr.desktop();}
        else if(event.type==surfxr::UiEventType::AddMonitor||event.type==surfxr::UiEventType::RemoveMonitor){requested_monitors.reset();vr.monitor(event.type==surfxr::UiEventType::AddMonitor);}
        else if(event.type==surfxr::UiEventType::Recenter)vr.recenter();
        else if(opts.input)surfxr::queue_ui_input(event,*input);
    };
    ui.toggle_mode=[this]{mode_commands.poll();requested_mode.reset();result.runtime_status->mode_requested(opts.session.surfxr_modes?opts.session.surfxr_modes->desired_desktop():!vr.settings_snapshot().capabilities.vr_requested);input->release_all();if(opts.session.surfxr_modes)opts.session.surfxr_modes->request(!opts.session.surfxr_modes->desired_desktop());else vr.toggle();};
    ui.tracking=[this](const auto& frame){
        vr.tracking(frame);
        if(stop_requested.load(std::memory_order_relaxed)){requested_mode.reset();return;}
        if(const auto command=mode_commands.poll())requested_mode=command;
        if(mode_commands.poll_exit()){requested_mode.reset();input->release_all();vr.exit_vr();}
        if(const auto command=mode_commands.poll_timing()){
            timing_trace_enabled=*command;timing_trace_pending=true;
            display.set_timing_trace_enabled(*command);next_status_sample={};
        }
        if(const auto command=mode_commands.poll_pacing())display.set_vr_presentation_pacing(*command);
        if(const auto command=mode_commands.poll_nv12_fused())display.set_desktop_nv12_fused(*command);
        if(const auto command=mode_commands.poll_monitors())if(opts.session.surfxr_desktop||(opts.session.surfxr_modes&&opts.session.surfxr_modes->desktop()))requested_monitors=*command;
        if(requested_monitors){const auto caps=vr.settings_snapshot().capabilities;
            if(!caps.connected||caps.desktop_monitor_count==*requested_monitors)requested_monitors.reset();
            else if(!caps.monitor_pending&&!vr.monitor(caps.desktop_monitor_count<*requested_monitors))requested_monitors.reset();
        }
        while(const auto command=mode_commands.poll_setting()){
            last_setting_id=static_cast<int>(command->first);last_setting_value=command->second;++last_setting_serial;
            last_setting_accepted=vr.setting({static_cast<surfxr::SettingId>(last_setting_id),double(last_setting_value)});
            if(last_setting_accepted)settings_store.submit(settings.snapshot());
            next_status_sample={};
        }
        while(const auto command=mode_commands.poll_quality()){
            last_quality_id=static_cast<int>(command->first);last_quality_value=command->second;++last_quality_serial;
            last_quality_accepted=apply_quality({static_cast<surfxr::ImageQualityId>(last_quality_id),last_quality_value});
        }
        while(const auto command=mode_commands.poll_input()){
            last_input_id=static_cast<int>(command->first);last_input_value=command->second;++last_input_serial;
            last_input_accepted=apply_input_preference({static_cast<surfxr::InputPreferenceId>(last_input_id),last_input_value});
        }
        if(requested_mode&&opts.session.surfxr_modes){result.runtime_status->mode_requested(*requested_mode);input->release_all();opts.session.surfxr_modes->request(!*requested_mode);requested_mode.reset();}
        if(requested_mode){const auto state=vr.settings_snapshot();
            if(!*requested_mode&&state.capabilities.connected){
                requested_mode.reset();input->release_all();vr.desktop();
            }else if(*requested_mode&&state.capabilities.connected&&state.capabilities.stage_active&&(state.capabilities.has_vr||state.capabilities.can_launch_vr)){
                requested_mode.reset();
                if(!state.capabilities.vr_requested){input->release_all();vr.launch();}
            }
        }
    };
    ui.settings=[this]{
        auto state=vr.settings_snapshot();
        // Requested mode is independent of the source still being presented.
        if(opts.session.surfxr_modes)state.capabilities.vr_requested=!opts.session.surfxr_modes->desired_desktop();
        return state;
    };
    ui.image_quality=[this]{return image_quality;};
    ui.input_preferences=[this]{return input_preferences;};
    ui.controller_batteries=[this](const auto& values){battery_monitor.publish_controllers(values);};
    ui.headset_battery=[this](auto value,auto sampled_ns){battery_monitor.publish_headset_runtime(value,sampled_ns);};
    ui.adaptive_prediction=opts.adaptive_prediction;
    ui.vr_frame=[this]{return vr.presentation_stereo();};
    ui.vr_frame_presented=[this](std::shared_ptr<const surfxr::VrStereoFrame> frame){
        vr.stereo_presented(frame);
        if(frame&&opts.session.surfxr_feedback)opts.session.surfxr_feedback->presented(*frame,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    };
    return ui;
}
}
