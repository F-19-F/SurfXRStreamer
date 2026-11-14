#include "viewer_report.hpp"

namespace surfxr::viewer {
int ViewerReport::print(){
    const bool presented=(xr.submitted_frames>0 && xr.new_generations>0 && decoders[0].frames>0)
        ||(xr.vr_submitted_frames>0&&xr.vr_new_stereo_frames>0&&vr_decoders[0].frames>0&&
           (backend==surfxr::SessionBackend::Surfxr||vr_decoders[1].frames>0));
    const bool success=(presented||receiver.server_stopped) && !xr_failed && !worker_failed && worker_completed;
    if(runtime_status)runtime_status->finish(xr_failed||worker_failed||!worker_completed);
    const char* status=receiver.server_stopped&&success?"server_stopped":success?"frames_presented":((xr_failed || worker_failed)?"viewer_error":"no_frames_presented");
    std::cout<<"{\"status\":\""<<status<<"\",\"stage\":\""<<stage<<"\",\"connection_started\":"<<(connection_started.load()?"true":"false")
        <<",\"backend\":\""<<"surfxr"<<'"'
        <<",\"connection_closed\":"<<(connection_closed?"true":"false")<<",\"worker_completed\":"<<(worker_completed?"true":"false")
        <<",\"worker_failed\":"<<(worker_failed?"true":"false")<<",\"xr_failed\":"<<(xr_failed?"true":"false")
        <<",\"cpu_affinity\":{\"available_requested\":"<<(cpu_affinity.available_requested?"true":"false")
        <<",\"attempted\":"<<(cpu_affinity.attempted?"true":"false")<<",\"applied\":"<<(cpu_affinity.applied?"true":"false")
        <<",\"changed\":"<<(cpu_affinity.changed?"true":"false")<<",\"effective_known\":"<<(cpu_affinity.effective_known?"true":"false")
        <<",\"before_count\":"<<cpu_affinity.before_count<<",\"requested_count\":"<<cpu_affinity.requested_count
        <<",\"effective_count\":"<<cpu_affinity.effective_count<<",\"before_mask_low64\":"<<cpu_affinity.before_mask_low64
        <<",\"requested_mask_low64\":"<<cpu_affinity.requested_mask_low64<<",\"effective_mask_low64\":"<<cpu_affinity.effective_mask_low64
        <<",\"error\":"<<cpu_affinity.error<<'}'
        <<",\"render_cpu_affinity\":{\"available_requested\":"<<(render_cpu_affinity.available_requested?"true":"false")
        <<",\"attempted\":"<<(render_cpu_affinity.attempted?"true":"false")<<",\"applied\":"<<(render_cpu_affinity.applied?"true":"false")
        <<",\"changed\":"<<(render_cpu_affinity.changed?"true":"false")<<",\"effective_known\":"<<(render_cpu_affinity.effective_known?"true":"false")
        <<",\"before_count\":"<<render_cpu_affinity.before_count<<",\"requested_count\":"<<render_cpu_affinity.requested_count
        <<",\"effective_count\":"<<render_cpu_affinity.effective_count<<",\"before_mask_low64\":"<<render_cpu_affinity.before_mask_low64
        <<",\"requested_mask_low64\":"<<render_cpu_affinity.requested_mask_low64<<",\"effective_mask_low64\":"<<render_cpu_affinity.effective_mask_low64
        <<",\"error\":"<<render_cpu_affinity.error<<'}'
        <<",\"worker_error\":"<<quoted_error(worker_error)<<",\"xr_error\":"<<quoted_error(xr_error)<<",\"startup_error\":"<<quoted_error(startup_error)
        <<",\"audio_requested\":"<<(audio_requested?"true":"false")<<",\"audio_error\":"<<quoted_error(audio_error)<<",\"audio\":"<<audio_report
        <<",\"microphone_requested\":"<<(microphone_requested?"true":"false")<<",\"microphone_error\":"<<quoted_error(microphone_error)<<",\"microphone\":"<<microphone_report
        <<",\"layout_load\":"<<quoted_error(layout_load)<<",\"layout_save\":"<<quoted_error(layout_save)
        <<",\"settings_load\":"<<quoted_error(settings_load)<<",\"settings_adjusted\":"<<settings_adjusted
        <<",\"image_quality\":{\"desktop_filter\":"<<final_image_quality.desktop_filter<<",\"vr_upscaling\":"<<(final_image_quality.vr_upscaling?"true":"false")
        <<",\"vr_sharpening\":"<<final_image_quality.vr_sharpening<<",\"vr_timewarp\":"<<(final_image_quality.vr_timewarp?"true":"false")<<",\"vr_sharpening_enabled\":"<<(final_image_quality.vr_sharpening_enabled?"true":"false")
        <<",\"load\":"<<quoted_error(image_quality_load)<<",\"writes\":"<<image_quality_store_stats.writes<<",\"failures\":"<<image_quality_store_stats.failures<<"}"
        <<",\"input_preferences\":{\"vr_controllers_enabled\":"<<(final_input_preferences.vr_controllers_enabled?"true":"false")
        <<",\"performance_overlay\":"<<(final_input_preferences.performance_overlay?"true":"false")
        <<",\"gaze_debug_overlay\":"<<(final_input_preferences.gaze_debug_overlay?"true":"false")
        <<",\"boundary_sync_enabled\":"<<(final_input_preferences.boundary_sync_enabled?"true":"false")
        <<",\"sync_raw_battery\":"<<(final_input_preferences.sync_raw_battery?"true":"false")
        <<",\"transition_hud_enabled\":"<<(final_input_preferences.transition_hud_enabled?"true":"false")
        <<",\"mini_hud_enabled\":"<<(final_input_preferences.mini_hud_enabled?"true":"false")
        <<",\"mini_hud_fps\":"<<(final_input_preferences.mini_hud_fps?"true":"false")
        <<",\"mini_hud_latency\":"<<(final_input_preferences.mini_hud_latency?"true":"false")
        <<",\"mini_hud_frame_time\":"<<(final_input_preferences.mini_hud_frame_time?"true":"false")
        <<",\"mini_hud_decode\":"<<(final_input_preferences.mini_hud_decode?"true":"false")
        <<",\"mini_hud_gaze_only\":"<<(final_input_preferences.mini_hud_gaze_only?"true":"false")
        <<",\"mini_hud_gaze_marker\":"<<(final_input_preferences.mini_hud_gaze_marker?"true":"false")
        <<",\"mini_hud_x\":"<<final_input_preferences.mini_hud_x<<",\"mini_hud_y\":"<<final_input_preferences.mini_hud_y
        <<",\"mini_hud_scale\":"<<final_input_preferences.mini_hud_scale
        <<",\"load\":"<<quoted_error(input_preferences_load)<<",\"writes\":"<<input_preferences_store_stats.writes<<",\"failures\":"<<input_preferences_store_stats.failures<<'}'
        <<",\"battery\":{\"hmd_percent\":"<<final_battery.hmd.percent<<",\"left_percent\":"<<final_battery.controllers[0].percent<<",\"right_percent\":"<<final_battery.controllers[1].percent
        <<",\"hmd_charging\":"<<(final_battery.hmd.charging?"true":"false")<<",\"left_charging\":"<<(final_battery.controllers[0].charging?"true":"false")
        <<",\"right_charging\":"<<(final_battery.controllers[1].charging?"true":"false")<<'}'
        <<",\"settings_store\":{\"enabled\":"<<(settings_store_stats.enabled?"true":"false")
        <<",\"submitted\":"<<settings_store_stats.submitted<<",\"coalesced\":"<<settings_store_stats.coalesced
        <<",\"writes\":"<<settings_store_stats.writes<<",\"failures\":"<<settings_store_stats.failures
        <<",\"rejected\":"<<settings_store_stats.rejected<<",\"status\":"<<quoted_error(surfxr::settings_store_status_name(settings_store_stats.last_status))<<"}"
        <<",\"input_requested\":"<<(input_requested?"true":"false")<<",\"input\":{\"accepted\":"<<input_stats.accepted
        <<",\"rejected\":"<<input_stats.rejected<<",\"snapshots\":"<<input_stats.snapshots<<",\"release_snapshots\":"<<input_stats.release_snapshots
        <<",\"coalesced_moves\":"<<input_stats.coalesced_moves<<",\"queue_overflows\":"<<input_stats.queue_overflows
        <<",\"right_button_down_snapshots\":"<<input_stats.right_button_down_snapshots
        <<",\"right_button_up_snapshots\":"<<input_stats.right_button_up_snapshots<<"}"
        <<",\"haptics\":"<<haptics_json(haptics->statistics())
        <<",\"decoder\":"<<decoder_json(decoders[0])<<",\"decoders\":{";
    for(std::size_t i=0;i<decoders.size();++i){if(i)std::cout<<',';std::cout<<'"'<<i<<"\":"<<decoder_json(decoders[i]);}
    std::cout<<"},\"vr_decoders\":[";
    for(unsigned id=0;id<vr_decoders.size();++id){if(id)std::cout<<',';std::cout<<decoder_json(vr_decoders[id]);}
    std::cout<<"],\"vr\":{";
    const char* separator="";
    const auto field=[&](const char* name,auto value){std::cout<<separator<<'"'<<name<<"\":"<<value;separator=",";};
#define VR_FIELD(name) field(#name,vr_stats.name)
    VR_FIELD(enter_requests);VR_FIELD(desktop_requests);VR_FIELD(launch_requests);VR_FIELD(exit_requests);
    VR_FIELD(paired_frames);VR_FIELD(invalid_frames);VR_FIELD(unmatched_epochs);VR_FIELD(old_epochs);
    VR_FIELD(queued_presentations);VR_FIELD(paired_queue_overflows);VR_FIELD(paired_queue_stale);VR_FIELD(paired_queue_skipped);
    VR_FIELD(playout_reserve_ns);VR_FIELD(playout_buffer_holds);
    VR_FIELD(origin_changes);VR_FIELD(start_timeouts);VR_FIELD(settings_rejected);
    VR_FIELD(start_commands_sent);VR_FIELD(start_command_pending);
    VR_FIELD(duplicate_enter_requests);VR_FIELD(duplicate_desktop_requests);VR_FIELD(vr_source_retries);
    VR_FIELD(vr_keyframe_recoveries);VR_FIELD(vr_restart_recoveries);VR_FIELD(desktop_source_retries);
    VR_FIELD(desktop_timeouts);VR_FIELD(source_mismatches);VR_FIELD(desktop_pending);
    VR_FIELD(desktop_keyframe_recoveries);VR_FIELD(desktop_restart_recoveries);VR_FIELD(desktop_failed);
    VR_FIELD(missing_metadata);VR_FIELD(invalid_pose);VR_FIELD(invalid_fov);VR_FIELD(mismatched_timestamp);
#undef VR_FIELD
    field("error",quoted_error(vr_stats.error));field("control_dequeued",control_stats.dequeued);field("control_rejected",control_stats.rejected);
    field("has_vr",final_settings.capabilities.has_vr?"true":"false");field("can_launch_vr",final_settings.capabilities.can_launch_vr?"true":"false");
    std::cout<<"},\"settings\":{";
    for(std::size_t i=0;i<surfxr::kRemoteSettingCount;++i){const auto& value=final_settings.values[i];if(i)std::cout<<',';
        std::cout<<'"'<<i<<"\":{\"requested\":"<<value.requested<<",\"observed\":"<<(value.actual?"true":"false")
            <<",\"actual\":"<<value.actual.value_or(0)<<",\"pending\":"<<(value.pending?"true":"false")<<'}';}
    std::cout<<"},\"initial_settings\":{";
    for(std::size_t i=0;i<surfxr::kRemoteSettingCount;++i){if(i)std::cout<<',';std::cout<<'"'<<i<<"\":"<<initial_preferences.values[i];}
    std::cout<<"},\"max_vr_quality\":"<<final_settings.capabilities.max_vr_quality;
    std::cout<<",\"xr\":"<<xr_json(xr)<<",\"controller_profiles\":{\"left\":"<<quoted_error(xr.interaction_profiles[0])
        <<",\"right\":"<<quoted_error(xr.interaction_profiles[1])<<"},\"receiver\":"<<(receiver.sanitized_json.empty()?"{}":receiver.sanitized_json)<<"}\n";
    return success?0:1;
}
}
