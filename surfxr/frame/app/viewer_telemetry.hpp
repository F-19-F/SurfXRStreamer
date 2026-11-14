#pragma once
#include "surfxr/viewer_commands.hpp"
#include "surfxr/surfxr_feedback.hpp"
#include "surfxr/haptics.hpp"
#include "surfxr/remote_settings.hpp"

namespace surfxr {
struct ViewerDecoderCounters {
    std::uint64_t packets=0,frames=0,configuration_changes=0,decode_time_ns=0;
    std::uint64_t avcodec_send_time_ns=0,avcodec_receive_time_ns=0,rgba_conversion_time_ns=0;
    std::uint64_t rgba_converted_frames=0,rgba_skipped_frames=0,rgba_suppressed_publications=0;
    std::uint64_t nv12_published_frames=0,nv12_copy_time_ns=0,nv12_pool_bytes=0;
    std::uint64_t nv12_zero_copy_frames=0,nv12_zero_copy_time_ns=0,nv12_zero_copy_fallback_frames=0;
    std::uint64_t rgba_buffer_allocations=0,rgba_buffer_reuses=0,rgba_temporary_allocations=0,rgba_pool_bytes=0;
    std::uint64_t metadata_unmatched=0,sequence_restarts=0;
    std::uint64_t async_queue_packets_max=0,async_queue_bytes_max=0,async_backpressure_wait_ns=0,async_discarded_packets=0;
    std::uint64_t callback_packets=0,packet_intervals=0,packet_interval_ns_sum=0;
    std::uint32_t width=0,height=0;
    int color_space=-1,color_range=-1;
    int effective_color_space=-1,effective_color_range=-1;
    bool active=false;
};
struct ViewerDecodeTelemetry {
    std::uint64_t sample_monotonic_ms=0;
    std::array<ViewerDecoderCounters,4> vr_decoders{};
    std::array<ViewerDecoderCounters,4> desktop_decoders{};
};
// XR only reads this copied snapshot, never the decoder's mutable statistics.
// No decoder/network/GPU work or file IO happens while this mutex is held.
class ViewerDecodeTelemetrySlot {
public:
    void publish(const ViewerDecodeTelemetry& value){std::lock_guard<std::mutex> lock(mutex_);value_=value;}
    ViewerDecodeTelemetry snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return value_;}
private:
    mutable std::mutex mutex_;
    ViewerDecodeTelemetry value_{};
};
// Only fixed lifecycle fields cross this private file. Rendering never writes
// to disk; it updates one bounded state and wakes the background writer.
struct ViewerTelemetry {
    bool desktop_capture_supported=false;
    unsigned desktop_capture_requested_mask=15,desktop_capture_accepted_mask=15;
    std::uint64_t keyboard_images_received=0,keyboard_candidate_frames=0;
    bool keyboard_active=false,keyboard_supported=false;
    transport::ReceiveSnapshot surfxr_receive;
    HapticsStats haptics;
    std::array<std::uint64_t,2> haptic_apply_requests{},haptic_apply_accepted{},haptic_stop_requests{},haptic_stop_accepted{};
    std::uint64_t haptic_suppressed=0,haptic_errors=0,focused_frames=0;
    int last_haptic_result=0;
    TestHeadMotionTelemetry test_head_motion;
    std::uint64_t sample_monotonic_ms=0;
    ViewerDecodeTelemetry decode;
    std::uint64_t transition_frames=0,transition_cached_frames=0,transition_placeholder_frames=0;
    std::uint32_t last_layer_count=0,last_enabled_desktop_mask=0,last_ready_desktop_mask=0,last_cached_desktop_mask=0;
    bool last_should_render=false,last_ui_anchored=false,last_vr_requested=false;
    bool last_vr_pair_valid=false,last_stage_locatable=false,last_transition_pending=false;
    std::uint64_t vr_keyframe_recoveries=0,vr_restart_recoveries=0;
    std::uint64_t mode_source_mismatches=0,desktop_source_retries=0;
    std::uint64_t desktop_keyframe_recoveries=0,desktop_restart_recoveries=0,desktop_timeouts=0;
    bool desktop_pending=false,desktop_failed=false;
    double requested_desktop_pointer_priority=0;
    double requested_vr_fps=default_remote_setting(SettingId::VrFramerate),
        requested_vr_quality=default_remote_setting(SettingId::VrGraphicsQuality),
        requested_vr_bitrate_mbps=default_remote_setting(SettingId::VrBitrateMbps),max_vr_bitrate_mbps=500;
    unsigned requested_foveated_streaming=1,eye_gaze_available=0,vr_foveated_streaming=0;
    std::uint64_t eye_gaze_samples=0,eye_gaze_invalid=0;
    unsigned system_click_available=0,system_touch_available=0,system_capture_active=0,controller_skeleton_active_mask=0;
    std::uint64_t system_capture_exit_serial=0,controller_skeleton_left_frames=0,controller_skeleton_right_frames=0;
    unsigned vr_focus_width=0,vr_focus_height=0;
    std::array<float,2> vr_focus_center_x{},vr_focus_center_y{};
    int active_codec=0,max_vr_quality=7,last_setting_request_id=-1,last_setting_request_value=0;
    bool last_setting_request_accepted=false;
    std::uint64_t last_setting_request_serial=0;
    int desktop_filter=0,vr_sharpening=25,last_quality_request_id=-1,last_quality_request_value=0;
    bool vr_timewarp=true;
    bool vr_upscaling=false,vr_sharpening_enabled=false,last_quality_request_accepted=false;
    std::uint64_t last_quality_request_serial=0;
    bool vr_controllers_enabled=true,performance_overlay=false,gaze_debug_overlay=false,boundary_sync_enabled=true,sync_raw_battery=false,last_input_request_accepted=false;
    bool mini_hud_enabled=false,mini_hud_fps=true,mini_hud_latency=true,mini_hud_frame_time=false,mini_hud_decode=false;
    bool mini_hud_gaze_only=false,mini_hud_gaze_marker=true,transition_hud_enabled=true;
    int mini_hud_x=80,mini_hud_y=20,mini_hud_scale=100;
    std::uint64_t gaze_debug_frames=0,gaze_debug_eye_layers=0,gaze_debug_budget_omissions=0;
    std::uint64_t performance_overlay_frames=0,performance_overlay_updates=0,performance_overlay_budget_omissions=0;
    int last_input_request_id=-1,last_input_request_value=0;
    std::uint64_t last_input_request_serial=0;
    int hmd_raw_battery_percent=-1,hmd_battery_percent=-1,left_battery_percent=-1,right_battery_percent=-1;
    bool hmd_battery_charging=false,left_battery_charging=false,right_battery_charging=false;
    std::uint64_t desktop_quality_frames=0,desktop_mip_updates=0,desktop_mip_reuses=0;
    std::uint64_t vr_enhancement_frames=0,vr_upscaled_frames=0;
    double vr_enhancement_gpu_wait_ms_sum=0;
    std::uint64_t vr_enhancement_gpu_samples=0,vr_raw_enhancement_frames=0;
    std::uint64_t vr_fused_postprocess_frames=0;
    double vr_enhancement_gpu_ms_sum=0;
    std::uint32_t vr_enhanced_width=0,vr_enhanced_height=0;
    std::uint64_t desktop_frames=0,desktop_intervals=0,stage_calls=0,projection_calls=0;
    std::uint64_t desktop_new_frames=0,desktop_new_intervals=0;
    double desktop_interval_ms=0,stage_ms=0,projection_ms=0,desktop_new_interval_ms=0;
    std::uint32_t projection_width=0,projection_height=0,desktop_visible_mask=0;
    std::array<std::uint32_t,4> desktop_decoded_width{},desktop_decoded_height{};
    std::array<std::uint64_t,4> desktop_panel_new_frames{};
    std::uint64_t desktop_layout_fingerprint=0,desktop_stage_copy_bytes=0,desktop_swapchain_wait_calls=0;
    std::uint64_t desktop_direct_upload_frames=0,desktop_direct_upload_bytes=0,desktop_edge_patch_bytes=0;
    std::uint64_t desktop_uploads=0,desktop_cached_upload_skips=0;
    double desktop_stage_copy_ms_sum=0,desktop_swapchain_wait_ms_sum=0;
    double desktop_end_frame_ms_sum=0,desktop_frame_work_ms_sum=0;
    bool desktop_gpu_timestamps=false;
    std::uint64_t desktop_mip_gpu_samples=0,desktop_projection_gpu_samples=0;
    double desktop_mip_gpu_ms_sum=0,desktop_mip_gpu_ms_max=0;
    double desktop_projection_gpu_ms_sum=0,desktop_projection_gpu_ms_max=0;
    std::uint64_t desktop_nv12_encoded_mip_updates=0,desktop_nv12_mip_updates=0,desktop_nv12_intermediate_bytes_avoided=0;
    std::uint64_t desktop_nv12_base_updates=0,desktop_nv12_base_reuses=0,desktop_nv12_encoded_base_updates=0;
    std::uint64_t desktop_nv12_gpu_samples=0;
    double desktop_nv12_gpu_ms_sum=0;
    // XR-owned cumulative counters only. Taking differences between samples
    // distinguishes newly decoded stereo captures from repeated compositor
    // submissions without reading the network/decoder thread's mutable state.
    std::uint64_t vr_timewarp_frames=0,vr_timewarp_fallback_frames=0;
    std::uint64_t vr_submitted_frames=0,vr_new_stereo_frames=0,vr_reused_stereo_frames=0,vr_submit_intervals=0;
    double vr_submit_interval_ms_sum=0;
    std::uint64_t vr_decoded_age_samples=0,vr_pose_sample_age_samples=0,vr_foveation_frames=0;
    double vr_decoded_age_ms_sum=0,vr_pose_sample_age_ms_sum=0,vr_foveation_gpu_wait_ms_sum=0;
    std::uint64_t vr_submitted_pose_age_samples=0;
    double vr_submitted_pose_age_ms_sum=0,vr_submitted_pose_age_ms_max=0;
    std::uint64_t vr_deferred_gpu_submissions=0,vr_deferred_gpu_completions=0,vr_foveation_gpu_samples=0;
    std::uint64_t vr_upload_wait_calls=0;
    double vr_upload_wait_ms_sum=0,vr_upload_wait_ms_max=0;
    std::uint64_t vr_stage_calls=0,vr_swapchain_wait_calls=0;
    std::uint64_t vr_direct_upload_frames=0,vr_direct_upload_bytes=0;
    std::uint64_t vr_direct_sample_frames=0,vr_upload_avoided_bytes=0;
    std::uint64_t vr_paired_frames=0,vr_queued_presentations=0;
    std::uint64_t vr_pair_queue_overflows=0,vr_pair_queue_stale=0,vr_pair_queue_skipped=0;
    std::uint64_t vr_pair_queue_depth=0,vr_playout_buffer_holds=0;
    double vr_playout_reserve_ms=0;
    double vr_foveation_gpu_ms_sum=0,vr_foveation_gpu_ms_max=0;
    double vr_stage_ms_sum=0,vr_stage_ms_max=0,vr_stage_copy_ms_sum=0,vr_stage_copy_ms_max=0;
    double vr_swapchain_wait_ms_sum=0,vr_swapchain_wait_ms_max=0;
    double vr_end_frame_ms_sum=0,vr_end_frame_ms_max=0,vr_frame_work_ms_sum=0,vr_frame_work_ms_max=0;
    std::uint64_t motion_sync_updates=0,prediction_time_valid_samples=0;
    std::uint64_t ipd_samples=0,ipd_events=0,ipd_fallback_samples=0;
    double motion_sync_latency_ms=0,motion_sync_previous_draw_ms=0,prediction_lead_ms_mean=0;
    bool surfxr_timing_available=false;
    double surfxr_total_ms=0,surfxr_upstream_ms=0,surfxr_decode_ms=0,surfxr_after_decode_ms=0;
    std::uint64_t motion_sync_gpu_estimated_frames=0,motion_sync_gpu_unavailable_frames=0;
    double motion_sync_gpu_tail_ms_sum=0,motion_sync_gpu_tail_ms_max=0;
    double current_refresh_hz=0,render_latency_ms=0;
    double requested_refresh_hz=0;
    bool refresh_rate_pending=false;
    std::uint64_t refresh_rate_requests=0,refresh_rate_request_failures=0;
    int last_refresh_request_result=0;
    bool render_latency_valid=false;
    std::uint32_t vr_eye_width=0,vr_eye_height=0,vr_decoded_eye_width=0,vr_decoded_eye_height=0;
};
// Copied by the receiver owner, independently of XR visibility or presentation.
// AudioStats contains only numeric values and does not require libpulse here.
struct ViewerModeTiming {
    std::uint64_t serial=0,requested_ms=0,presented_ms=0;
};
struct ViewerAudioTelemetry {
    bool requested=false,supported=false;
    std::uint64_t sample_monotonic_ms=0,received_pcm_bytes=0;
    AudioStats stats;
};
} // namespace surfxr
