#include "viewer_private.hpp"

namespace surfxr::viewer {
std::string desktop_timing_json(const surfxr::XrDisplayStats& x){
    std::ostringstream out;out<<'{';const char* separator="";
    const auto field=[&](const char* name,auto value){out<<separator<<'"'<<name<<"\":"<<value;separator=",";};
#define DESKTOP_FIELD(name) field(#name,x.desktop_##name)
    DESKTOP_FIELD(submitted_frames);DESKTOP_FIELD(submit_intervals);
    DESKTOP_FIELD(submit_interval_ms_sum);DESKTOP_FIELD(submit_interval_ms_max);
    DESKTOP_FIELD(stage_calls);DESKTOP_FIELD(stage_ms_sum);DESKTOP_FIELD(stage_ms_max);
    DESKTOP_FIELD(stage_cached_calls);DESKTOP_FIELD(stage_coherent_calls);
    DESKTOP_FIELD(stage_copy_bytes);DESKTOP_FIELD(visible_mask);DESKTOP_FIELD(layout_fingerprint);
    DESKTOP_FIELD(direct_upload_frames);DESKTOP_FIELD(direct_upload_bytes);DESKTOP_FIELD(edge_patch_bytes);
    DESKTOP_FIELD(uploads);DESKTOP_FIELD(cached_upload_skips);
    DESKTOP_FIELD(stage_copy_ms_sum);DESKTOP_FIELD(stage_copy_ms_max);
    DESKTOP_FIELD(stage_flush_ms_sum);DESKTOP_FIELD(stage_flush_ms_max);
    DESKTOP_FIELD(swapchain_wait_calls);DESKTOP_FIELD(swapchain_wait_ms_sum);DESKTOP_FIELD(swapchain_wait_ms_max);
    DESKTOP_FIELD(projection_calls);DESKTOP_FIELD(projection_ms_sum);DESKTOP_FIELD(projection_ms_max);
    DESKTOP_FIELD(end_frame_ms_sum);DESKTOP_FIELD(end_frame_ms_max);
    DESKTOP_FIELD(frame_work_ms_sum);DESKTOP_FIELD(frame_work_ms_max);
    DESKTOP_FIELD(deferred_copy_batches);DESKTOP_FIELD(deferred_copy_covered_completions);
    DESKTOP_FIELD(gpu_timestamps);DESKTOP_FIELD(mip_gpu_samples);DESKTOP_FIELD(projection_gpu_samples);
    DESKTOP_FIELD(mip_gpu_ms_sum);DESKTOP_FIELD(mip_gpu_ms_max);
    DESKTOP_FIELD(projection_gpu_ms_sum);DESKTOP_FIELD(projection_gpu_ms_max);
#undef DESKTOP_FIELD
    out<<",\"panels\":{";
    for(std::size_t i=0;i<x.desktop_new_frames.size();++i){if(i)out<<',';
        out<<'"'<<i<<"\":{\"new_frames\":"<<x.desktop_new_frames[i]
            <<",\"decoded_width\":"<<x.desktop_decoded_width[i]<<",\"decoded_height\":"<<x.desktop_decoded_height[i]
            <<",\"new_frame_intervals\":"<<x.desktop_new_frame_intervals[i]
            <<",\"new_frame_interval_ms_sum\":"<<x.desktop_new_frame_interval_ms_sum[i]
            <<",\"new_frame_interval_ms_max\":"<<x.desktop_new_frame_interval_ms_max[i]<<'}';}
    out<<"}}";return out.str();
}
std::string haptics_json(const surfxr::HapticsStats& h){
    std::ostringstream out;out<<'{';const char* separator="";
    const auto field=[&](const char* name,auto value){out<<separator<<'"'<<name<<"\":"<<value;separator=",";};
#define HAPTICS_FIELD(name) field(#name,h.name)
    HAPTICS_FIELD(metadata);HAPTICS_FIELD(positive_samples);HAPTICS_FIELD(invalid);
    HAPTICS_FIELD(duplicates);HAPTICS_FIELD(out_of_order);HAPTICS_FIELD(coalesced);
    HAPTICS_FIELD(expired);HAPTICS_FIELD(discarded_disabled);HAPTICS_FIELD(pulses);
    HAPTICS_FIELD(stops);HAPTICS_FIELD(clears);
    HAPTICS_FIELD(stale_video);HAPTICS_FIELD(stale_positive_video);
    HAPTICS_FIELD(deferred_renewals);
#undef HAPTICS_FIELD
    out<<'}';return out.str();
}
std::string xr_json(const surfxr::XrDisplayStats& x){
    std::ostringstream out;
    out<<"{\"waited_frames\":"<<x.waited_frames<<",\"renderable_frames\":"<<x.renderable_frames
        <<",\"submitted_frames\":"<<x.submitted_frames<<",\"new_generations\":"<<x.new_generations
        <<",\"skipped_generations\":"<<x.skipped_generations
        <<",\"upload_count\":"<<x.upload_count<<",\"repeated_submissions\":"<<x.repeated_submissions
        <<",\"visible_frames\":"<<x.visible_frames<<",\"focused_frames\":"<<x.focused_frames
        <<",\"eye_gaze_extension_enabled\":"<<x.eye_gaze_extension_enabled<<",\"eye_gaze_supported\":"<<x.eye_gaze_supported
        <<",\"eye_gaze_samples\":"<<x.eye_gaze_samples<<",\"eye_gaze_invalid\":"<<x.eye_gaze_invalid
        <<",\"vr_focus_width\":"<<x.vr_focus_width<<",\"vr_focus_height\":"<<x.vr_focus_height
        <<",\"skipped_not_renderable\":"<<x.skipped_not_renderable<<",\"zero_layer_frames\":"<<x.zero_layer_frames
        <<",\"transition_frames\":"<<x.transition_frames
        <<",\"transition_cached_frames\":"<<x.transition_cached_frames
        <<",\"transition_placeholder_frames\":"<<x.transition_placeholder_frames
        <<",\"last_layer_count\":"<<x.last_layer_count
        <<",\"last_enabled_desktop_mask\":"<<x.last_enabled_desktop_mask
        <<",\"last_ready_desktop_mask\":"<<x.last_ready_desktop_mask
        <<",\"last_cached_desktop_mask\":"<<x.last_cached_desktop_mask
        <<",\"last_should_render\":"<<x.last_should_render
        <<",\"last_ui_anchored\":"<<x.last_ui_anchored
        <<",\"last_vr_requested\":"<<x.last_vr_requested
        <<",\"last_vr_pair_valid\":"<<x.last_vr_pair_valid
        <<",\"last_stage_locatable\":"<<x.last_stage_locatable
        <<",\"last_transition_pending\":"<<x.last_transition_pending
        <<",\"last_generation\":"<<x.last_generation<<",\"width\":"<<x.width<<",\"height\":"<<x.height
        <<",\"last_session_state\":"<<x.last_session_state<<",\"elapsed_seconds\":"<<x.elapsed_seconds
        <<",\"begin_frame_discarded\":"<<x.begin_frame_discarded<<",\"last_begin_result\":"<<x.last_begin_result<<",\"last_end_result\":"<<x.last_end_result
        <<",\"max_xr_wait_ms\":"<<x.max_xr_wait_ms
        <<",\"new_generation_age_samples\":"<<x.new_generation_age_samples
        <<",\"new_generation_age_ms_mean\":"<<x.new_generation_age_ms_mean
        <<",\"new_generation_age_ms_max\":"<<x.new_generation_age_ms_max
        <<",\"gpu_wait_calls\":"<<x.gpu_wait_calls
        <<",\"deferred_copy_batches\":"<<x.deferred_copy_batches<<",\"deferred_copy_covered_completions\":"<<x.deferred_copy_covered_completions
        <<",\"gpu_wait_ms_sum\":"<<x.gpu_wait_ms_sum<<",\"gpu_wait_ms_max\":"<<x.gpu_wait_ms_max
        <<",\"action_syncs\":"<<x.action_syncs<<",\"active_hand_frames\":"<<x.active_hand_frames
        <<",\"pointer_events\":"<<x.pointer_events<<",\"keyboard_events\":"<<x.keyboard_events
        <<",\"panel_drag_updates\":"<<x.panel_drag_updates<<",\"panel_scale_updates\":"<<x.panel_scale_updates
        <<",\"panel_layer_submissions\":"<<x.panel_layer_submissions<<",\"max_visible_panels\":"<<x.max_visible_panels
        <<",\"frame_controller_profile_enabled\":"<<(x.frame_controller_profile_enabled?"true":"false")
        <<",\"battery_extension_enabled\":"<<(x.battery_extension_enabled?"true":"false")
        <<",\"battery_query_samples\":"<<x.battery_query_samples<<",\"battery_query_failures\":"<<x.battery_query_failures
        <<",\"cylinder_layer_enabled\":"<<(x.cylinder_layer_enabled?"true":"false")
        <<",\"runtime_layer_limit\":"<<x.runtime_layer_limit
        <<",\"curved_layer_submissions\":"<<x.curved_layer_submissions
        <<",\"controls_layer_submissions\":"<<x.controls_layer_submissions
        <<",\"pointer_laser_budget_omissions\":"<<x.pointer_laser_budget_omissions
        <<",\"projection_mesh_enabled\":"<<(x.projection_mesh_enabled?"true":"false")
        <<",\"projection_width\":"<<x.projection_width<<",\"projection_height\":"<<x.projection_height
        <<",\"projection_msaa_samples\":"<<x.projection_msaa_samples<<",\"projection_frames\":"<<x.projection_frames
        <<",\"upload_format\":"<<x.upload_format
        <<",\"projection_gpu_wait_ms_sum\":"<<x.projection_gpu_wait_ms_sum
        <<",\"projection_gpu_wait_ms_max\":"<<x.projection_gpu_wait_ms_max
        <<",\"desktop\":"<<desktop_timing_json(x)
        <<",\"left_right_click_action_active_frames\":"<<x.right_click_action_active_frames[0]
        <<",\"right_right_click_action_active_frames\":"<<x.right_click_action_active_frames[1]
        <<",\"left_right_click_press_edges\":"<<x.right_click_press_edges[0]
        <<",\"right_right_click_press_edges\":"<<x.right_click_press_edges[1]
        <<",\"right_click_events\":"<<x.right_click_events<<",\"binding_profiles_rejected\":"<<x.binding_profiles_rejected
        <<",\"curve_segments\":{\"0\":"<<x.curve_segments[0]<<",\"1\":"<<x.curve_segments[1]<<",\"2\":"<<x.curve_segments[2]<<",\"3\":"<<x.curve_segments[3]<<"}"
        <<",\"layout_restored\":"<<(x.layout_restored?"true":"false")
        <<",\"last_action_sync_result\":"<<x.last_action_sync_result
        <<",\"action_not_focused_frames\":"<<x.action_not_focused_frames
        <<",\"left_hand_active_frames\":"<<x.hand_active_frames[0]<<",\"right_hand_active_frames\":"<<x.hand_active_frames[1]
        <<",\"stage_supported\":"<<(x.stage_supported?"true":"false")
        <<",\"tracking_samples\":"<<x.tracking_samples<<",\"stage_locatable_samples\":"<<x.stage_locatable_samples
        <<",\"stage_origin_changes\":"<<x.stage_origin_changes
        <<",\"boundary_samples\":"<<x.boundary_samples
        <<",\"ipd_samples\":"<<x.ipd_samples<<",\"ipd_events\":"<<x.ipd_events<<",\"ipd_fallback_samples\":"<<x.ipd_fallback_samples
        <<",\"timespec_time_extension_enabled\":"<<(x.timespec_time_extension_enabled?"true":"false")
        <<",\"adaptive_prediction_enabled\":"<<(x.adaptive_prediction_enabled?"true":"false")
        <<",\"prediction_time_valid_samples\":"<<x.prediction_time_valid_samples<<",\"prediction_time_invalid_samples\":"<<x.prediction_time_invalid_samples
        <<",\"prediction_time_conversion_failures\":"<<x.prediction_time_conversion_failures
        <<",\"prediction_time_past_samples\":"<<x.prediction_time_past_samples<<",\"prediction_time_over_limit_samples\":"<<x.prediction_time_over_limit_samples
        <<",\"prediction_lead_ms_mean\":"<<x.prediction_lead_ms_mean<<",\"prediction_lead_ms_max\":"<<x.prediction_lead_ms_max
        <<",\"prediction_target_display_samples\":"<<x.prediction_target_display_samples
        <<",\"prediction_target_display_delta_ms_mean\":"<<(x.prediction_target_display_samples?x.prediction_target_display_delta_ms_sum/double(x.prediction_target_display_samples):0)
        <<",\"prediction_target_display_delta_ms_min\":"<<x.prediction_target_display_delta_ms_min
        <<",\"prediction_target_display_delta_ms_max\":"<<x.prediction_target_display_delta_ms_max
        <<",\"prediction_stage_boundary_rejections\":"<<x.prediction_stage_boundary_rejections
        <<",\"prediction_locate_time_invalid\":"<<x.prediction_locate_time_invalid
        <<",\"prediction_head_invalid\":"<<x.prediction_head_invalid<<",\"prediction_optics_invalid\":"<<x.prediction_optics_invalid
        <<",\"prediction_controller_invalid\":"<<x.prediction_controller_invalid
        <<",\"motion_sync_updates\":"<<x.motion_sync_updates<<",\"motion_sync_rejected_feedback\":"<<x.motion_sync_rejected_feedback
        <<",\"motion_sync_latency_ms\":"<<x.motion_sync_latency_ms<<",\"motion_sync_previous_draw_ms\":"<<x.motion_sync_previous_draw_ms
        <<",\"surfxr_timing_available\":"<<(x.surfxr_timing_available?"true":"false")<<",\"surfxr_total_ms\":"<<x.surfxr_total_ms
        <<",\"surfxr_upstream_ms\":"<<x.surfxr_upstream_ms<<",\"surfxr_decode_ms\":"<<x.surfxr_decode_ms<<",\"surfxr_after_decode_ms\":"<<x.surfxr_after_decode_ms
        <<",\"motion_sync_gpu_estimated_frames\":"<<x.motion_sync_gpu_estimated_frames<<",\"motion_sync_gpu_unavailable_frames\":"<<x.motion_sync_gpu_unavailable_frames
        <<",\"motion_sync_gpu_tail_ms_sum\":"<<x.motion_sync_gpu_tail_ms_sum<<",\"motion_sync_gpu_tail_ms_max\":"<<x.motion_sync_gpu_tail_ms_max
        <<",\"performance_metrics_extension_enabled\":"<<(x.performance_metrics_extension_enabled?"true":"false")
        <<",\"performance_overlay_frames\":"<<x.performance_overlay_frames
        <<",\"gaze_debug_frames\":"<<x.gaze_debug_frames<<",\"gaze_debug_eye_layers\":"<<x.gaze_debug_eye_layers
        <<",\"gaze_debug_budget_omissions\":"<<x.gaze_debug_budget_omissions
        <<",\"performance_overlay_updates\":"<<x.performance_overlay_updates
        <<",\"performance_overlay_budget_omissions\":"<<x.performance_overlay_budget_omissions
        <<",\"render_latency_supported\":"<<(x.render_latency_supported?"true":"false")
        <<",\"render_latency_enabled\":"<<(x.render_latency_enabled?"true":"false")
        <<",\"render_latency_valid\":"<<(x.render_latency_valid?"true":"false")
        <<",\"render_latency_samples\":"<<x.render_latency_samples<<",\"render_latency_errors\":"<<x.render_latency_errors
        <<",\"render_latency_invalid_samples\":"<<x.render_latency_invalid_samples<<",\"render_latency_expirations\":"<<x.render_latency_expirations
        <<",\"render_latency_ms\":"<<x.render_latency_ms<<",\"render_latency_ms_mean\":"<<x.render_latency_ms_mean
        <<",\"render_latency_ms_max\":"<<x.render_latency_ms_max
        <<",\"shortcut_single_clicks\":"<<x.shortcut_single_clicks<<",\"shortcut_double_clicks\":"<<x.shortcut_double_clicks
        <<",\"tracking_binding_profiles_rejected\":"<<x.tracking_binding_profiles_rejected
        <<",\"current_refresh_hz\":"<<x.current_refresh_hz
        <<",\"requested_refresh_hz\":"<<x.requested_refresh_hz
        <<",\"refresh_rate_pending\":"<<(x.refresh_rate_pending?"true":"false")
        <<",\"refresh_rate_requests\":"<<x.refresh_rate_requests
        <<",\"refresh_rate_request_failures\":"<<x.refresh_rate_request_failures
        <<",\"refresh_rate_query_failures\":"<<x.refresh_rate_query_failures
        <<",\"last_refresh_request_result\":"<<x.last_refresh_request_result
        <<",\"max_render_width\":"<<x.max_render_width<<",\"max_render_height\":"<<x.max_render_height
        <<",\"left_system_binding_supported\":"<<(x.left_system_binding_supported?"true":"false")
        <<",\"left_system_active_frames\":"<<x.left_system_active_frames
        <<",\"left_system_press_edges\":"<<x.left_system_press_edges
        <<",\"haptics\":{\"left_frame_binding\":"<<(x.frame_haptic_binding_supported[0]?"true":"false")
        <<",\"right_frame_binding\":"<<(x.frame_haptic_binding_supported[1]?"true":"false")
        <<",\"left_apply_requests\":"<<x.haptic_apply_requests[0]<<",\"right_apply_requests\":"<<x.haptic_apply_requests[1]
        <<",\"left_apply_accepted\":"<<x.haptic_apply_accepted[0]<<",\"right_apply_accepted\":"<<x.haptic_apply_accepted[1]
        <<",\"left_stop_requests\":"<<x.haptic_stop_requests[0]<<",\"right_stop_requests\":"<<x.haptic_stop_requests[1]
        <<",\"left_stop_accepted\":"<<x.haptic_stop_accepted[0]<<",\"right_stop_accepted\":"<<x.haptic_stop_accepted[1]
        <<",\"suppressed\":"<<x.haptic_suppressed<<",\"errors\":"<<x.haptic_errors<<",\"last_result\":"<<x.last_haptic_result<<"}"
        <<",\"vr_submitted_frames\":"<<x.vr_submitted_frames<<",\"vr_new_stereo_frames\":"<<x.vr_new_stereo_frames
        <<",\"vr_timewarp_frames\":"<<x.vr_timewarp_frames<<",\"vr_timewarp_fallback_frames\":"<<x.vr_timewarp_fallback_frames
        <<",\"vr_reused_stereo_frames\":"<<x.vr_reused_stereo_frames<<",\"vr_reuse_cache_resets\":"<<x.vr_reuse_cache_resets
        <<",\"vr_late_snapshot_checks\":"<<x.vr_late_snapshot_checks<<",\"vr_late_snapshot_replacements\":"<<x.vr_late_snapshot_replacements
        <<",\"vr_late_snapshot_invalidations\":"<<x.vr_late_snapshot_invalidations
        <<",\"vr_submit_intervals\":"<<x.vr_submit_intervals<<",\"vr_submit_interval_ms_sum\":"<<x.vr_submit_interval_ms_sum
        <<",\"vr_submit_interval_ms_max\":"<<x.vr_submit_interval_ms_max
        <<",\"vr_decoded_age_samples\":"<<x.vr_decoded_age_samples<<",\"vr_decoded_age_ms_sum\":"<<x.vr_decoded_age_ms_sum
        <<",\"vr_decoded_age_ms_max\":"<<x.vr_decoded_age_ms_max<<",\"vr_eye_decode_skew_ms_max\":"<<x.vr_eye_decode_skew_ms_max
        <<",\"vr_pose_sample_age_samples\":"<<x.vr_pose_sample_age_samples<<",\"vr_pose_sample_age_ms_sum\":"<<x.vr_pose_sample_age_ms_sum
        <<",\"vr_pose_sample_age_ms_max\":"<<x.vr_pose_sample_age_ms_max
        <<",\"vr_submitted_pose_age_samples\":"<<x.vr_submitted_pose_age_samples
        <<",\"vr_submitted_pose_age_ms_sum\":"<<x.vr_submitted_pose_age_ms_sum
        <<",\"vr_submitted_pose_age_ms_max\":"<<x.vr_submitted_pose_age_ms_max
        <<",\"vr_rejected_stereo_frames\":"<<x.vr_rejected_stereo_frames<<",\"vr_last_timestamp\":"<<x.vr_last_timestamp
        <<",\"vr_foveation_frames\":"<<x.vr_foveation_frames
        <<",\"vr_foveation_gpu_wait_ms_sum\":"<<x.vr_foveation_gpu_wait_ms_sum
        <<",\"vr_foveation_gpu_wait_ms_max\":"<<x.vr_foveation_gpu_wait_ms_max
        <<",\"vr_deferred_gpu_submissions\":"<<x.vr_deferred_gpu_submissions<<",\"vr_deferred_gpu_completions\":"<<x.vr_deferred_gpu_completions
        <<",\"vr_upload_wait_calls\":"<<x.vr_upload_wait_calls<<",\"vr_upload_wait_ms_sum\":"<<x.vr_upload_wait_ms_sum<<",\"vr_upload_wait_ms_max\":"<<x.vr_upload_wait_ms_max
        <<",\"vr_foveation_gpu_samples\":"<<x.vr_foveation_gpu_samples<<",\"vr_foveation_gpu_ms_sum\":"<<x.vr_foveation_gpu_ms_sum<<",\"vr_foveation_gpu_ms_max\":"<<x.vr_foveation_gpu_ms_max
        <<",\"vr_stage_calls\":"<<x.vr_stage_calls<<",\"vr_stage_ms_sum\":"<<x.vr_stage_ms_sum<<",\"vr_stage_ms_max\":"<<x.vr_stage_ms_max
        <<",\"vr_stage_copy_ms_sum\":"<<x.vr_stage_copy_ms_sum<<",\"vr_stage_copy_ms_max\":"<<x.vr_stage_copy_ms_max
        <<",\"vr_direct_upload_frames\":"<<x.vr_direct_upload_frames<<",\"vr_direct_upload_bytes\":"<<x.vr_direct_upload_bytes
        <<",\"vr_direct_sample_frames\":"<<x.vr_direct_sample_frames<<",\"vr_upload_avoided_bytes\":"<<x.vr_upload_avoided_bytes
        <<",\"vr_swapchain_wait_calls\":"<<x.vr_swapchain_wait_calls<<",\"vr_swapchain_wait_ms_sum\":"<<x.vr_swapchain_wait_ms_sum<<",\"vr_swapchain_wait_ms_max\":"<<x.vr_swapchain_wait_ms_max
        <<",\"vr_end_frame_ms_sum\":"<<x.vr_end_frame_ms_sum<<",\"vr_end_frame_ms_max\":"<<x.vr_end_frame_ms_max
        <<",\"vr_frame_work_ms_sum\":"<<x.vr_frame_work_ms_sum<<",\"vr_frame_work_ms_max\":"<<x.vr_frame_work_ms_max
        <<",\"desktop_nv12_encoded_mip_updates\":"<<x.desktop_nv12_encoded_mip_updates
        <<",\"desktop_nv12_base_updates\":"<<x.desktop_nv12_base_updates<<",\"desktop_nv12_base_reuses\":"<<x.desktop_nv12_base_reuses
        <<",\"desktop_nv12_encoded_base_updates\":"<<x.desktop_nv12_encoded_base_updates
        <<",\"desktop_nv12_mip_updates\":"<<x.desktop_nv12_mip_updates<<",\"desktop_nv12_intermediate_bytes_avoided\":"<<x.desktop_nv12_intermediate_bytes_avoided
        <<",\"desktop_quality_frames\":"<<x.desktop_quality_frames<<",\"desktop_mip_updates\":"<<x.desktop_mip_updates<<",\"desktop_mip_reuses\":"<<x.desktop_mip_reuses
        <<",\"vr_enhancement_frames\":"<<x.vr_enhancement_frames<<",\"vr_upscaled_frames\":"<<x.vr_upscaled_frames
        <<",\"vr_enhancement_gpu_wait_ms_sum\":"<<x.vr_enhancement_gpu_wait_ms_sum<<",\"vr_enhancement_gpu_wait_ms_max\":"<<x.vr_enhancement_gpu_wait_ms_max
        <<",\"vr_enhancement_gpu_samples\":"<<x.vr_enhancement_gpu_samples<<",\"vr_enhancement_gpu_ms_sum\":"<<x.vr_enhancement_gpu_ms_sum
        <<",\"vr_enhancement_gpu_ms_max\":"<<x.vr_enhancement_gpu_ms_max<<",\"vr_raw_enhancement_frames\":"<<x.vr_raw_enhancement_frames
        <<",\"vr_fused_postprocess_frames\":"<<x.vr_fused_postprocess_frames
        <<",\"vr_enhanced_width\":"<<x.vr_enhanced_width<<",\"vr_enhanced_height\":"<<x.vr_enhanced_height
        <<",\"vr_decoded_eye_width\":["<<x.vr_decoded_eye_width[0]<<','<<x.vr_decoded_eye_width[1]<<"]"
        <<",\"vr_decoded_eye_height\":["<<x.vr_decoded_eye_height[0]<<','<<x.vr_decoded_eye_height[1]<<"]"
        <<",\"vr_eye_width\":["<<x.vr_eye_width[0]<<','<<x.vr_eye_width[1]<<"]"
        <<",\"vr_eye_height\":["<<x.vr_eye_height[0]<<','<<x.vr_eye_height[1]<<"]"
        <<",\"refresh_rates_hz\":[";
    for(std::size_t i=0;i<x.refresh_rates_hz.size();++i){if(i)out<<',';out<<x.refresh_rates_hz[i];}
    out<<"]"
        <<",\"session_state_events\":{";
    for(std::size_t i=0;i<x.session_state_events.size();++i){if(i)out<<',';out<<'"'<<i<<"\":"<<x.session_state_events[i];}
    out<<"}}";return out.str();
}
std::string quoted_error(const std::string& input){
    std::string result="\"";constexpr char hex[]="0123456789abcdef";
    for(std::size_t i=0;i<input.size() && i<256;++i){
        const auto c=static_cast<unsigned char>(input[i]);
        if(c=='"' || c=='\\'){result+='\\';result+=static_cast<char>(c);}
        else if(c<32 || c>=127){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=static_cast<char>(c);
    }
    result+='"';return result;
}
std::string decoder_json(const surfxr::DecoderStats& decoder){
    std::ostringstream out;
    out<<"{\"packets\":"<<decoder.packets<<",\"frames\":"<<decoder.frames
        <<",\"configuration_changes\":"<<decoder.configuration_changes<<",\"decode_time_ns\":"<<decoder.decode_time_ns
        <<",\"avcodec_send_time_ns\":"<<decoder.avcodec_send_time_ns<<",\"avcodec_receive_time_ns\":"<<decoder.avcodec_receive_time_ns
        <<",\"color_space\":"<<decoder.color_space<<",\"color_range\":"<<decoder.color_range
        <<",\"effective_color_space\":"<<decoder.effective_color_space<<",\"effective_color_range\":"<<decoder.effective_color_range
        <<",\"rgba_conversion_time_ns\":"<<decoder.rgba_conversion_time_ns
        <<",\"rgba_converted_frames\":"<<decoder.rgba_converted_frames
        <<",\"nv12_published_frames\":"<<decoder.nv12_published_frames<<",\"nv12_copy_time_ns\":"<<decoder.nv12_copy_time_ns<<",\"nv12_pool_bytes\":"<<decoder.nv12_pool_bytes
        <<",\"nv12_zero_copy_frames\":"<<decoder.nv12_zero_copy_frames<<",\"nv12_zero_copy_time_ns\":"<<decoder.nv12_zero_copy_time_ns<<",\"nv12_zero_copy_fallback_frames\":"<<decoder.nv12_zero_copy_fallback_frames
        <<",\"rgba_skipped_frames\":"<<decoder.rgba_skipped_frames
        <<",\"rgba_suppressed_publications\":"<<decoder.rgba_suppressed_publications
        <<",\"rgba_conversion_threads\":"<<decoder.rgba_conversion_threads
        <<",\"rgba_buffer_allocations\":"<<decoder.rgba_buffer_allocations<<",\"rgba_buffer_reuses\":"<<decoder.rgba_buffer_reuses
        <<",\"rgba_temporary_allocations\":"<<decoder.rgba_temporary_allocations
        <<",\"rgba_pool_buffers\":"<<decoder.rgba_pool_buffers<<",\"rgba_pool_bytes\":"<<decoder.rgba_pool_bytes
        <<",\"metadata_from_opaque\":"<<decoder.metadata_from_opaque<<",\"metadata_from_pts\":"<<decoder.metadata_from_pts
        <<",\"metadata_unmatched\":"<<decoder.metadata_unmatched
        <<",\"async_queue_packets_max\":"<<decoder.async_queue_packets_max<<",\"async_queue_bytes_max\":"<<decoder.async_queue_bytes_max
        <<",\"async_backpressure_wait_ns\":"<<decoder.async_backpressure_wait_ns<<",\"async_discarded_packets\":"<<decoder.async_discarded_packets
        <<",\"sequence_changes\":"<<decoder.sequence_changes<<",\"sequence_restarts\":"<<decoder.sequence_restarts
        <<",\"waiting_for_keyframe_packets\":"<<decoder.waiting_for_keyframe_packets
        <<",\"hardware_busy_recoveries\":"<<decoder.hardware_busy_recoveries
        <<",\"backend\":"<<quoted_error(surfxr::decoder_backend_name(decoder.backend))
        <<",\"selected_decoder\":"<<quoted_error(decoder.selected_decoder)
        <<",\"pixel_format\":"<<quoted_error(decoder.pixel_format)
        <<",\"width\":"<<decoder.width<<",\"height\":"<<decoder.height<<",\"failed\":"<<(decoder.failed?"true":"false")
        <<",\"ffmpeg_code\":"<<decoder.ffmpeg_code<<"}";
    return out.str();
}
#ifdef SURFXR_HAVE_MICROPHONE
std::string microphone_json(const surfxr::MicrophoneStats& m){
    std::ostringstream out;out<<"{";
    const char* separator="";
    const auto field=[&](const char* name,auto value){out<<separator<<'"'<<name<<"\":"<<value;separator=",";};
    out<<std::boolalpha;
#define FIELD(name) field(#name,m.name)
    FIELD(initialized);FIELD(failed);FIELD(stopped);FIELD(source_validated);FIELD(source_is_monitor);FIELD(pulse_error);
    FIELD(capture_frames);FIELD(consumed_frames);FIELD(nonzero_samples);FIELD(sample_square_sum);FIELD(peak);
    FIELD(dropped_old_frames);FIELD(dropped_contention_frames);FIELD(hole_frames);FIELD(invalid_chunks);FIELD(read_callbacks);
    FIELD(queued_frames);FIELD(max_queued_frames);FIELD(server_buffer_bytes);FIELD(server_fragment_bytes);
#undef FIELD
    field("rms",m.capture_frames?std::sqrt(static_cast<double>(m.sample_square_sum)/static_cast<double>(m.capture_frames))/32768.0:0.0);
    out<<"}";return out.str();
}
#endif
#ifdef SURFXR_HAVE_AUDIO_OUTPUT
std::string audio_json(const surfxr::AudioStats& a){
    std::ostringstream out;
    out<<"{\"initialized\":"<<(a.initialized?"true":"false")<<",\"failed\":"<<(a.failed?"true":"false")
        <<",\"stopped\":"<<(a.stopped?"true":"false")<<",\"pulse_error\":"<<a.pulse_error
        <<",\"ready\":"<<(a.ready?"true":"false")<<",\"reconnecting\":"<<(a.reconnecting?"true":"false")
        <<",\"context_state\":"<<a.context_state<<",\"stream_state\":"<<a.stream_state
        <<",\"connection_attempts\":"<<a.connection_attempts<<",\"successful_connections\":"<<a.successful_connections
        <<",\"reconnects\":"<<a.reconnects<<",\"connection_failures\":"<<a.connection_failures<<",\"queue_resets\":"<<a.queue_resets
        <<",\"submitted_bytes\":"<<a.submitted_bytes<<",\"pcm_frames_consumed\":"<<a.pcm_frames_consumed
        <<",\"silence_frames\":"<<a.silence_frames<<",\"dropped_old_frames\":"<<a.dropped_old_frames
        <<",\"dropped_contention_frames\":"<<a.dropped_contention_frames<<",\"underrun_callbacks\":"<<a.underrun_callbacks
        <<",\"server_underflows\":"<<a.server_underflows<<",\"invalid_submissions\":"<<a.invalid_submissions
        <<",\"write_callbacks\":"<<a.write_callbacks<<",\"written_frames\":"<<a.written_frames
        <<",\"max_queued_frames\":"<<a.max_queued_frames<<",\"server_buffer_bytes\":"<<a.server_buffer_bytes
        <<",\"server_target_bytes\":"<<a.server_target_bytes<<",\"buffering_target_frames\":"<<a.buffering_target_frames
        <<",\"rebuffer_callbacks\":"<<a.rebuffer_callbacks<<",\"rebuffer_events\":"<<a.rebuffer_events
        <<",\"transition_frames\":"<<a.transition_frames<<"}";
    return out.str();
}
#endif
}
