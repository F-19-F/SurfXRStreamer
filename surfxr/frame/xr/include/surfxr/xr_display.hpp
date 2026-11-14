#pragma once

#include "surfxr/latest_frame.hpp"
#include "surfxr/vr_ui.hpp"
#include "surfxr/tracking.hpp"
#include "surfxr/vr_frame.hpp"
#include "surfxr/haptics.hpp"
#include "surfxr/image_quality.hpp"
#include "surfxr/input_preferences.hpp"
#include "surfxr/frame_timing.hpp"
#include "surfxr/build_features.hpp"

#include "surfxr/keyboard_image.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace surfxr {

struct XrDisplayStats {
    std::uint64_t gaze_debug_frames=0,gaze_debug_eye_layers=0,gaze_debug_budget_omissions=0;
    std::uint64_t waited_frames = 0;
    std::uint64_t renderable_frames = 0;
    std::uint64_t submitted_frames = 0;
    std::uint64_t new_generations = 0;
    // Decoded generations replaced in the latest slot before staging; not loss
    // of compressed packets and not a network-loss measurement.
    std::uint64_t skipped_generations = 0;
    std::uint64_t upload_count = 0;
    std::uint64_t repeated_submissions = 0;
    std::uint64_t visible_frames = 0;
    std::uint64_t focused_frames = 0;
    std::uint64_t skipped_not_renderable = 0;
    std::uint64_t zero_layer_frames = 0;
    // Startup HUD never counts as video or acknowledges a requested mode.
    // Legacy cached-background counters remain zero for status compatibility.
    std::uint64_t transition_frames=0,transition_cached_frames=0,transition_placeholder_frames=0;
    // Enabled is receiver availability; ready is a decoded snapshot selected
    // for live desktop presentation, including while SteamVR is starting.
    std::uint32_t last_layer_count=0,last_enabled_desktop_mask=0,last_ready_desktop_mask=0,last_cached_desktop_mask=0;
    bool last_should_render=false,last_ui_anchored=false,last_vr_requested=false;
    bool last_vr_pair_valid=false,last_stage_locatable=false,last_transition_pending=false;
    std::uint64_t begin_frame_discarded = 0;
    std::uint64_t last_generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<std::uint64_t, 9> session_state_events{};
    int last_session_state = 0;
    int last_begin_result = 0;
    int last_end_result = 0;
    double elapsed_seconds = 0;
    double max_xr_wait_ms = 0;
    // Same-machine steady_clock age from completed RGBA conversion to a new
    // generation's first successful xrEndFrame; not end-to-end input latency.
    std::uint64_t new_generation_age_samples = 0;
    double new_generation_age_ms_mean = 0;
    double new_generation_age_ms_max = 0;
    std::uint64_t gpu_wait_calls = 0;
    double gpu_wait_ms_sum = 0;
    double gpu_wait_ms_max = 0;
    std::uint64_t action_syncs=0,active_hand_frames=0,pointer_events=0,keyboard_events=0,keyboard_candidate_frames=0;
    std::uint64_t panel_drag_updates=0,panel_scale_updates=0,panel_layer_submissions=0;
    std::uint32_t max_visible_panels=0;
    bool frame_controller_profile_enabled=false;
    bool battery_extension_enabled=false;
    bool eye_gaze_extension_enabled=false,eye_gaze_supported=false;
    std::uint64_t eye_gaze_samples=0,eye_gaze_invalid=0;
    unsigned vr_focus_width=0,vr_focus_height=0;
    std::array<float,2> vr_focus_center_x{},vr_focus_center_y{};
    std::uint64_t battery_query_samples=0,battery_query_failures=0;
    bool cylinder_layer_enabled=false;
    std::uint32_t runtime_layer_limit=0;
    std::uint32_t max_render_width=0,max_render_height=0;
    std::array<std::uint32_t,4> curve_segments{}; // last mesh segment count; 0 = native cylinder/flat
    std::uint64_t curved_layer_submissions=0,controls_layer_submissions=0;
    std::uint64_t pointer_laser_budget_omissions=0;
    bool projection_mesh_enabled=false;
    std::uint32_t projection_width=0,projection_height=0,projection_msaa_samples=0;
    std::uint64_t projection_frames=0;
    double projection_gpu_wait_ms_sum=0,projection_gpu_wait_ms_max=0;
    // Exclusive desktop submission cadence includes repeated images. Per-panel
    // new-frame cadence counts only newly presented decoded generations.
    // Both reset continuity across VR mode, non-rendered frames and hidden panels.
    std::uint64_t desktop_submitted_frames=0,desktop_submit_intervals=0;
    double desktop_submit_interval_ms_sum=0,desktop_submit_interval_ms_max=0;
    std::array<std::uint64_t,4> desktop_new_frames{},desktop_new_frame_intervals{};
    std::array<double,4> desktop_new_frame_interval_ms_sum{},desktop_new_frame_interval_ms_max{};
    // Current selected decoded dimensions (zero when unavailable). Visibility
    // is the panel setting, independent of receiver availability/readiness.
    std::array<std::uint32_t,4> desktop_decoded_width{},desktop_decoded_height{};
    std::uint32_t desktop_visible_mask=0;
    // World panel geometry/source/crop/visibility only: excludes head pose,
    // controller hover, video generations and filter/display-frame phase.
    std::uint64_t desktop_layout_fingerprint=0;
    // CPU wall time, not GPU timestamps. Stage includes new-frame CPU copying;
    // swapchain wait includes acquire + wait. Projection includes its target's
    // acquire/wait and fence, so these two timings overlap and must not be added.
    // Frame work excludes xrWaitFrame. No metric is physical display/scanout FPS.
    std::uint64_t desktop_stage_calls=0,desktop_swapchain_wait_calls=0,desktop_projection_calls=0;
    double desktop_stage_ms_sum=0,desktop_stage_ms_max=0;
    std::uint32_t upload_format=0; // numeric VkFormat for diagnostics
    std::uint64_t desktop_stage_cached_calls=0,desktop_stage_coherent_calls=0;
    // CPU-written upload bytes: full crop on fallback, only edge strips on
    // mapped direct uploads. The latter retains the arbitrary-alpha feather.
    std::uint64_t desktop_stage_copy_bytes=0;
    // Direct counts are newly staged frames/interior bytes, not every rotated
    // XR image transfer. uploads counts completed desktop source image copies;
    // cache skips count otherwise-needed uploads avoided by an owned mip cache.
    std::uint64_t desktop_direct_upload_frames=0,desktop_direct_upload_bytes=0,desktop_edge_patch_bytes=0;
    std::uint64_t desktop_uploads=0,desktop_cached_upload_skips=0;
    double desktop_stage_copy_ms_sum=0,desktop_stage_copy_ms_max=0;
    double desktop_stage_flush_ms_sum=0,desktop_stage_flush_ms_max=0;
    double desktop_swapchain_wait_ms_sum=0,desktop_swapchain_wait_ms_max=0;
    double desktop_projection_ms_sum=0,desktop_projection_ms_max=0;
    double desktop_end_frame_ms_sum=0,desktop_end_frame_ms_max=0;
    double desktop_frame_work_ms_sum=0,desktop_frame_work_ms_max=0;
    std::uint64_t desktop_deferred_copy_batches=0,desktop_deferred_copy_covered_completions=0;
    std::array<std::uint64_t,2> right_click_action_active_frames{},right_click_press_edges{};
    std::uint64_t right_click_events=0; // dispatched right-button press events, not releases
    bool layout_restored=false;
    int last_action_sync_result=0;
    std::uint64_t action_not_focused_frames=0;
    std::uint32_t binding_profiles_rejected=0;
    std::array<std::uint64_t,2> hand_active_frames{};
    std::array<std::string,2> interaction_profiles{{"none","none"}};
    std::array<bool,2> frame_haptic_binding_supported{};
    std::array<std::uint64_t,2> haptic_apply_requests{},haptic_apply_accepted{},haptic_stop_requests{},haptic_stop_accepted{};
    std::uint64_t haptic_suppressed=0,haptic_errors=0;
    int last_haptic_result=0;
    bool left_system_binding_supported=false;
    std::uint64_t left_system_active_frames=0,left_system_press_edges=0;
    unsigned system_click_available=0,system_touch_available=0,system_capture_active=0,controller_skeleton_active_mask=0;
    std::uint64_t system_capture_exit_serial=0,controller_skeleton_left_frames=0,controller_skeleton_right_frames=0;
    bool stage_supported=false;
    bool timespec_time_extension_enabled=false;
    bool adaptive_prediction_enabled=true;
    std::uint64_t prediction_time_valid_samples=0,prediction_time_invalid_samples=0;
    std::uint64_t prediction_time_conversion_failures=0,prediction_time_past_samples=0,prediction_time_over_limit_samples=0;
    double prediction_lead_ms_mean=0,prediction_lead_ms_max=0;
    std::uint64_t prediction_target_display_samples=0;
    double prediction_target_display_delta_ms_sum=0,prediction_target_display_delta_ms_min=0,prediction_target_display_delta_ms_max=0;
    std::uint64_t prediction_stage_boundary_rejections=0,prediction_locate_time_invalid=0;
    std::uint64_t prediction_head_invalid=0,prediction_optics_invalid=0,prediction_controller_invalid=0;
    std::uint64_t motion_sync_updates=0,motion_sync_rejected_feedback=0;
    double motion_sync_latency_ms=0,motion_sync_previous_draw_ms=0;
    bool surfxr_timing_available=false;
    double surfxr_total_ms=0,surfxr_upstream_ms=0,surfxr_decode_ms=0,surfxr_after_decode_ms=0;
    // Deferred feedback estimates draw duration from CPU submission + that
    // same frame's GPU span. Missing timestamp results skip feedback entirely.
    std::uint64_t motion_sync_gpu_estimated_frames=0,motion_sync_gpu_unavailable_frames=0;
    double motion_sync_gpu_tail_ms_sum=0,motion_sync_gpu_tail_ms_max=0;
    bool performance_metrics_extension_enabled=false;
    std::uint64_t performance_overlay_frames=0,performance_overlay_updates=0,performance_overlay_budget_omissions=0;
    bool render_latency_supported=false,render_latency_enabled=false,render_latency_valid=false;
    std::uint64_t render_latency_samples=0,render_latency_errors=0,render_latency_invalid_samples=0,render_latency_expirations=0;
    double render_latency_ms=0,render_latency_ms_mean=0,render_latency_ms_max=0;
    std::uint64_t tracking_samples=0,stage_locatable_samples=0,stage_origin_changes=0,boundary_samples=0;
    std::uint64_t ipd_samples=0,ipd_events=0,ipd_fallback_samples=0;
    std::uint64_t shortcut_single_clicks=0,shortcut_double_clicks=0;
    std::uint32_t tracking_binding_profiles_rejected=0;
    std::vector<float> refresh_rates_hz;
    float current_refresh_hz=0;
    float requested_refresh_hz=0;
    bool refresh_rate_pending=false;
    std::uint64_t refresh_rate_requests=0,refresh_rate_request_failures=0,refresh_rate_query_failures=0;
    int last_refresh_request_result=0;
    std::uint64_t vr_submitted_frames=0,vr_new_stereo_frames=0,vr_rejected_stereo_frames=0;
    std::uint64_t vr_late_snapshot_checks=0,vr_late_snapshot_replacements=0,vr_late_snapshot_invalidations=0;
    // Same immutable stereo snapshot submitted using the last released images,
    // without acquiring/uploading/reconstructing either VR eye again.
    std::uint64_t vr_timewarp_frames=0,vr_timewarp_fallback_frames=0;
    std::uint64_t vr_reused_stereo_frames=0,vr_reuse_cache_resets=0;
    // Uploads whose host wait was deferred; covered completions used a later
    // same-queue reconstruction/projection fence instead of a second wait.
    std::uint64_t deferred_copy_batches=0,deferred_copy_covered_completions=0;
    // Frame-local latency only: the older decoded eye until successful XR
    // submission. This excludes PC capture/encode and network transport time.
    std::uint64_t vr_decoded_age_samples=0;
    double vr_decoded_age_ms_sum=0,vr_decoded_age_ms_max=0,vr_eye_decode_skew_ms_max=0;
    // Local pose sample -> successful submission, correlated through the PC's
    // echoed tracking frame ID. Includes the round trip, but not panel scanout.
    std::uint64_t vr_pose_sample_age_samples=0;
    double vr_pose_sample_age_ms_sum=0,vr_pose_sample_age_ms_max=0;
    // Every successful VR submission, including repeated stale images. This
    // is source-pose age at submission, not physical motion-to-photon latency.
    std::uint64_t vr_submitted_pose_age_samples=0;
    double vr_submitted_pose_age_ms_sum=0,vr_submitted_pose_age_ms_max=0;
    // Successful VR submission cadence, including repeated images. Previous
    // sample resets across mode/origin/swapchain changes; not server FPS.
    std::uint64_t vr_submit_intervals=0;
    double vr_submit_interval_ms_sum=0,vr_submit_interval_ms_max=0;
    std::uint64_t vr_last_timestamp=0;
    std::array<std::uint32_t,2> vr_eye_width{},vr_eye_height{};
    std::array<std::uint32_t,2> vr_decoded_eye_width{},vr_decoded_eye_height{};
    std::uint64_t vr_foveation_frames=0;
    double vr_foveation_gpu_wait_ms_sum=0,vr_foveation_gpu_wait_ms_max=0;
    // Timestamped reconstruction/postprocessing may finish after xrEndFrame. Retire its
    // upload fence before staging reuse, and rendering fence before command reuse.
    std::uint64_t vr_deferred_gpu_submissions=0,vr_deferred_gpu_completions=0;
    std::uint64_t vr_upload_wait_calls=0;
    double vr_upload_wait_ms_sum=0,vr_upload_wait_ms_max=0;
    // Timestamp span includes source-barrier dependencies, but excludes host
    // fence scheduling. It is not the old reconstruction fence-wait metric.
    std::uint64_t vr_foveation_gpu_samples=0;
    double vr_foveation_gpu_ms_sum=0,vr_foveation_gpu_ms_max=0;
    // Stage is per newly copied eye and includes its allocation/copy/flush.
    // Swapchain waits cover all acquisitions in a new VR frame; work excludes
    // xrWaitFrame but includes retiring the previous deferred GPU submission.
    std::uint64_t vr_stage_calls=0,vr_swapchain_wait_calls=0;
    double vr_stage_ms_sum=0,vr_stage_ms_max=0,vr_stage_copy_ms_sum=0,vr_stage_copy_ms_max=0;
    std::uint64_t vr_direct_upload_frames=0,vr_direct_upload_bytes=0;
    std::uint64_t vr_direct_sample_frames=0,vr_upload_avoided_bytes=0;
    double vr_swapchain_wait_ms_sum=0,vr_swapchain_wait_ms_max=0;
    double vr_end_frame_ms_sum=0,vr_end_frame_ms_max=0,vr_frame_work_ms_sum=0,vr_frame_work_ms_max=0;
    std::uint64_t desktop_quality_frames=0,desktop_mip_updates=0,desktop_mip_reuses=0;
    bool desktop_gpu_timestamps=false;
    std::uint64_t desktop_mip_gpu_samples=0,desktop_projection_gpu_samples=0;
    double desktop_mip_gpu_ms_sum=0,desktop_mip_gpu_ms_max=0;
    double desktop_projection_gpu_ms_sum=0,desktop_projection_gpu_ms_max=0;
    std::uint64_t desktop_nv12_encoded_mip_updates=0,desktop_nv12_mip_updates=0,desktop_nv12_intermediate_bytes_avoided=0;
    std::uint64_t desktop_nv12_base_updates=0,desktop_nv12_base_reuses=0,desktop_nv12_encoded_base_updates=0;
    std::uint64_t desktop_nv12_gpu_samples=0;
    double desktop_nv12_gpu_ms_sum=0;
    std::uint64_t vr_enhancement_frames=0,vr_upscaled_frames=0;
    double vr_enhancement_gpu_wait_ms_sum=0,vr_enhancement_gpu_wait_ms_max=0;
    std::uint64_t vr_enhancement_gpu_samples=0,vr_raw_enhancement_frames=0;
    // Fused work is included in the reconstruction GPU span, never counted twice.
    std::uint64_t vr_fused_postprocess_frames=0;
    double vr_enhancement_gpu_ms_sum=0,vr_enhancement_gpu_ms_max=0;
    std::uint32_t vr_enhanced_width=0,vr_enhanced_height=0;
};

struct XrPanelSource {
    std::uint32_t source_id=0;
    LatestFrameSlot* frames=nullptr; // must outlive run; shared across crops allowed
    std::string title="Desktop";
    UiCrop crop{}; // fixed normalized crop; default entire source
    const std::atomic_bool* available=nullptr; // nullptr=available; must outlive run
    const std::atomic<std::uint64_t>* demand_epoch=nullptr; // nullptr=legacy output; must outlive run
};
struct XrUiCallbacks {
    std::shared_ptr<KeyboardOverlay> keyboard_overlay;
    std::shared_ptr<HapticsQueue> haptics; // bounded bridge; only this XR thread applies feedback
    // False queries the remote poses again at this local frame's display time;
    // it never relabels an adaptively predicted pose with a different time.
    bool adaptive_prediction=true;
    // SURFXR predicts controllers on the PC; query their current pose/velocity.
    // This is a backend contract, independent of VR/desktop and head prediction.
    UiEventCallback event;
    std::function<void(const TrackingFrame&)> tracking; // nonblocking; copy latest snapshot
    // Called on the XR thread when shouldRender changes, before selecting
    // video, and with false at start/exit. Producers may omit presentation-only
    // work while hidden, but must continue decoding compressed references.
    std::function<void(bool)> renderable;
    // Visible source IDs, independent of whether their first frame is ready.
    // Publication and selection share fresh per-source tokens through pause.
    std::function<void(unsigned)> desktop_demand;
    std::function<void(unsigned)> desktop_capture_request;
    std::function<DesktopCaptureStatus()> desktop_capture_status;
    std::function<void()> toggle_mode; // request only; caller owns protocol transition
    std::function<RemoteSettingsSnapshot()> settings; // bounded shared snapshot, no network I/O
    std::function<ImageQualityPreferences()> image_quality; // local GPU options, no PC command
    std::function<InputPreferences()> input_preferences; // remote controller gate, not local UI
    std::function<void(const std::array<DeviceBattery,2>&)> controller_batteries; // copied once per second
    std::function<void(DeviceBattery,std::uint64_t)> headset_battery; // runtime value and original sample time
    // Actual runtime Hz only: initial report and observed changes. The caller
    // may queue an SURFXR settings edit; true confirms local enqueue only, not a
    // server VR FPS acknowledgement. False retains the report for retry.
    std::function<bool(float)> refresh_rate_changed;
    std::function<std::shared_ptr<const VrStereoFrame>()> vr_frame; // nullptr selects desktop
    // Acknowledge the exact immutable pair only after successful XR submission.
    // Snapshot reads remain non-consuming; no failed/hidden frame may dequeue
    // a ready pair. Must be nonblocking and must not perform network/GPU work.
    std::function<void(std::shared_ptr<const VrStereoFrame>)> vr_frame_presented;
    std::function<void(bool vr)> frame_presented; // nonblocking; only after successful desktop/VR submission
    // Called after every completed XR frame, including no-video/shouldRender
    // false frames, after statistics update. Must not perform blocking I/O.
    std::function<void()> frame_observed;
    // Try-only local telemetry copy; never reads a file or waits on a socket.
};

// the XR/Vulkan session locally; the swapchain is allocated lazily for the first
// valid RGBA frame. This is an explicit display action, not a capability probe.
class XrDisplay {
public:
    explicit XrDisplay(bool sync_boundary=false);
    ~XrDisplay();
    XrDisplay(const XrDisplay&) = delete;
    XrDisplay& operator=(const XrDisplay&) = delete;

    // Runs on the constructing thread. Reads immutable snapshots without holding
    // the slot's lock while waiting on XR/GPU. Shared stop ends the display; this
    // function does not install signal handlers or print to stdout. seconds=0
    // runs until shared stop, a UI exit action, or the runtime ends the session.
    XrDisplayStats run(LatestFrameSlot& frames, std::atomic_bool& stop, unsigned seconds);
    XrDisplayStats run(const std::vector<XrPanelSource>& panels,std::atomic_bool& stop,unsigned seconds,
                       const XrUiCallbacks& callbacks={},const UiLayout* initial_layout=nullptr);
    // Available after run throws; errors contain fixed local API names/results.
    XrDisplayStats statistics() const noexcept;
    // Session/upload owner calls. The sampler serializes only pose queries and
    // lifetime transitions; it does not wait on rendering, GPU or networking.
    void set_timestamped_tracking(bool enabled);
    bool sample_tracking(TrackingFrame& frame);
    // Optional VR-only cached/coherent transfer storage for decoder output.
    // The callback may run on the decoder thread; all callbacks/frames must
    // finish and be released before this display is destroyed. Empty on BGRA
    // swapchains or when suitable memory is unavailable; normal RGBA remains.
    RgbaFrameAllocator rgba_frame_allocator() const;
    Nv12FrameAllocator nv12_frame_allocator() const;
    // Media-thread barrier after producers/slots/pairing have been retired.
    // XR completes GPU reads and detaches imported decoder mappings before
    // the next hardware session opens. Shared stop cancels shutdown waits.
    void retire_decoder_frames(const std::atomic_bool& stop);
    // XR-thread-only bounded numeric diagnostics, enabled through private IPC.
    void set_timing_trace_enabled(bool enabled);
    // Nonpersistent private diagnostic override for same-session A/B checks.
    void set_vr_presentation_pacing(bool enabled);
    void set_desktop_nv12_fused(bool enabled);
    FrameTimingSnapshot timing_snapshot() const;
    // Call on the XR thread after run. Empty if no head anchor was established.
    // The caller decides whether normal completion permits persisting it.
    std::optional<UiLayout> layout() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace surfxr
