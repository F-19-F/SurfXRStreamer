#pragma once
#include "../../viewer_telemetry.hpp"
#include "surfxr/steamvr_startup.hpp"

namespace surfxr {
class ViewerStatusWriter {
public:
    explicit ViewerStatusWriter(std::string path,bool vr):path_(std::move(path)),vr_(vr){
        if(path_.empty())return;
        if(path_[0]!='/'||path_.size()>4000)throw std::runtime_error("invalid_status_path");
        temporary_=path_+".tmp."+std::to_string(getpid());
        if(!write(0,vr_))throw std::runtime_error("status_file_unwritable");
        worker_=std::thread([this]{run();});
    }
    ~ViewerStatusWriter(){finish(true);}
    ViewerStatusWriter(const ViewerStatusWriter&)=delete;
    ViewerStatusWriter& operator=(const ViewerStatusWriter&)=delete;
    void mode_requested(bool vr){
        if(path_.empty())return;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return;
            ++mode_timing_.serial;mode_timing_.requested_ms=monotonic_ms();mode_timing_.presented_ms=0;requested_vr_=vr;dirty_=true;
            if(vr||steamvr_startup_.state!=SteamVrStartupState::Failed)steamvr_startup_={};}
        wake_.notify_one();
    }
    bool presented(bool vr){
        if(path_.empty())return false;
        bool first=false;
        bool changed=false;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return false;
            const bool completed=vr&&steamvr_startup_.state==SteamVrStartupState::RuntimeReady;
            if(completed)steamvr_startup_.state=SteamVrStartupState::Completed;
            changed=state_!=1||vr_!=vr||completed;
            first=mode_timing_.requested_ms&&!mode_timing_.presented_ms&&vr==requested_vr_;
            if(first)mode_timing_.presented_ms=monotonic_ms();
            if(!first&&!changed)return false;
            state_=1;vr_=vr;dirty_=true;}
        wake_.notify_one();
        return first||changed;
    }
    // Independent receiver slot; XR metrics cannot erase PC startup progress.
    // Only an actual VR presentation above promotes RuntimeReady to Completed.
    void steamvr_startup(const SteamVrStartupStatus& value){
        if(path_.empty()||static_cast<unsigned>(value.state)>=steamvr_startup_states.size()||
           static_cast<unsigned>(value.reason)>=steamvr_startup_reasons.size())return;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return;
            // A late capability reply may replace legacy ID0 with a PC ID.
            // Only a new explicit mode request resets presentation completion.
            if(steamvr_startup_.state==SteamVrStartupState::Completed&&value.state!=SteamVrStartupState::Failed)return;
            if(steamvr_startup_.state==SteamVrStartupState::Failed&&value.state==SteamVrStartupState::NotStarted)return;
            steamvr_startup_=value;dirty_=true;}
        wake_.notify_one();
    }
    void telemetry(const ViewerTelemetry& value){
        if(path_.empty())return;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return;telemetry_=value;dirty_=true;}
        wake_.notify_one();
    }
    // Separate slot: one-second upload reports must not be overwritten by the
    // independently sampled XR telemetry. Only a small numeric copy under lock;
    // JSON serialization and disk IO remain on the status worker.
    void tracking_upload(const TrackingUploadTelemetry& value){
        if(path_.empty())return;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return;tracking_upload_=value;dirty_=true;}
        wake_.notify_one();
    }
    // The receiver publishes this slot even when XR is hidden. The writer
    // never reaches back into AudioOutput or holds an audio callback's lock.
    void audio_output(const ViewerAudioTelemetry& value){
        if(path_.empty())return;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return;audio_output_=value;dirty_=true;}
        wake_.notify_one();
    }
    // Separate bounded diagnostic file; the supervisor's status stays small.
    // nullopt sends no update. A disabled snapshot preserves the final trace.
    void timing(const std::optional<FrameTimingSnapshot>& value){
        if(path_.empty()||!value)return;
        {std::lock_guard<std::mutex> lock(mutex_);if(done_)return;timing_=value;timing_dirty_=true;dirty_=true;}
        wake_.notify_one();
    }
    void finish(bool failed){
        if(!worker_.joinable())return;
        {std::lock_guard<std::mutex> lock(mutex_);state_=failed?3:2;done_=true;dirty_=true;
            if(failed&&(steamvr_startup_.state==SteamVrStartupState::Launching||steamvr_startup_.state==SteamVrStartupState::RuntimeReady)){
                steamvr_startup_.state=SteamVrStartupState::Failed;steamvr_startup_.reason=SteamVrStartupReason::SessionFailed;
            }}
        wake_.notify_one();worker_.join();
    }
private:
    static std::uint64_t monotonic_ms(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    void write_mode(const ViewerModeTiming& mode,bool requested_vr)noexcept{
        try{
            const auto data="{\"mode_request_serial\":"+std::to_string(mode.serial)+",\"mode_requested_ms\":"+std::to_string(mode.requested_ms)+
                ",\"mode_presented_ms\":"+std::to_string(mode.presented_ms)+",\"mode_requested_vr\":"+(requested_vr?"true":"false")+"}\n";
            if(data.size()<=256)write_atomic(path_+".mode.json",temporary_+".mode",data);
        }catch(...){}
    }
    static bool write_atomic(const std::string& path,const std::string& temporary,const std::string& data) noexcept {
        const int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0)return false;
        std::size_t offset=0;bool okay=true;
        while(offset<data.size()){
            const auto n=::write(fd,data.data()+offset,data.size()-offset);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){okay=false;break;}offset+=static_cast<std::size_t>(n);
        }
        if(close(fd)!=0)okay=false;
        if(okay&&rename(temporary.c_str(),path.c_str())==0)return true;
        unlink(temporary.c_str());return false;
    }
    bool write_timing(const FrameTimingSnapshot& snapshot) noexcept {
        try{
            const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            std::ostringstream record;write_frame_timing_json(record,snapshot,static_cast<std::uint64_t>(getpid()),static_cast<std::uint64_t>(now));
            return write_atomic(path_+".timing.json",temporary_+".timing",record.str());
        }catch(...){return false;}
    }
    bool write_haptics(const ViewerTelemetry& metrics) noexcept {
        try{
            std::ostringstream record;record<<"{\"sample_monotonic_ms\":"<<metrics.sample_monotonic_ms;
            const auto array=[&](const char* name,const auto& values){record<<",\""<<name<<"\":["<<values[0]<<','<<values[1]<<']';};
            array("apply_requests",metrics.haptic_apply_requests);array("apply_accepted",metrics.haptic_apply_accepted);
            array("stop_requests",metrics.haptic_stop_requests);array("stop_accepted",metrics.haptic_stop_accepted);
            record<<",\"suppressed\":"<<metrics.haptic_suppressed<<",\"errors\":"<<metrics.haptic_errors
                <<",\"last_result\":"<<metrics.last_haptic_result<<",\"focused_frames\":"<<metrics.focused_frames
                <<",\"submitted_frames\":"<<metrics.vr_submitted_frames<<",\"system_capture_active\":"<<metrics.system_capture_active
                <<",\"queue\":{\"metadata\":"<<metrics.haptics.metadata;
#define SURFXR_HAPTIC_FIELD(name) record<<",\"" #name "\":"<<metrics.haptics.name
            SURFXR_HAPTIC_FIELD(positive_samples);SURFXR_HAPTIC_FIELD(invalid);SURFXR_HAPTIC_FIELD(duplicates);SURFXR_HAPTIC_FIELD(out_of_order);
            SURFXR_HAPTIC_FIELD(coalesced);SURFXR_HAPTIC_FIELD(expired);SURFXR_HAPTIC_FIELD(discarded_disabled);SURFXR_HAPTIC_FIELD(pulses);
            SURFXR_HAPTIC_FIELD(stops);SURFXR_HAPTIC_FIELD(clears);SURFXR_HAPTIC_FIELD(stale_video);SURFXR_HAPTIC_FIELD(stale_positive_video);
            SURFXR_HAPTIC_FIELD(deferred_renewals);
#undef SURFXR_HAPTIC_FIELD
            record<<"}}\n";const auto data=record.str();
            return data.size()<=2048&&write_atomic(path_+".haptics.json",temporary_+".haptics",data);
        }catch(...){return false;}
    }
    bool write(unsigned state,bool vr,const ViewerTelemetry& metrics={},const TrackingUploadTelemetry& tracking={},
               const ViewerAudioTelemetry& audio={},const SteamVrStartupStatus& startup={}) noexcept {
        try{
            const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const char* names[]={"starting","streaming","stopped","error"};
            std::ostringstream record;
            record.precision(17); // Preserve small A/B deltas of long-running cumulative totals.
            record<<"{\"state\":\""<<names[state]<<"\",\"mode\":\""<<(vr?"vr":"desktop")
                <<"\",\"error\":"<<(state==3?"\"viewer_error\"":"null")<<",\"heartbeat_monotonic_ms\":"<<now
                <<",\"desktop_submitted_frames\":"<<metrics.desktop_frames<<",\"desktop_submit_intervals\":"<<metrics.desktop_intervals
                <<",\"desktop_submit_interval_ms_sum\":"<<metrics.desktop_interval_ms<<",\"desktop_stage_calls\":"<<metrics.stage_calls
                <<",\"desktop_stage_ms_sum\":"<<metrics.stage_ms<<",\"desktop_projection_calls\":"<<metrics.projection_calls
                <<",\"desktop_projection_ms_sum\":"<<metrics.projection_ms<<",\"desktop_new_frames\":"<<metrics.desktop_new_frames
                <<",\"desktop_new_intervals\":"<<metrics.desktop_new_intervals<<",\"desktop_new_interval_ms_sum\":"<<metrics.desktop_new_interval_ms
                <<",\"steamvr_startup\":{\"state\":\""<<steamvr_startup_name(startup.state)<<"\",\"reason\":\""<<steamvr_startup_reason_name(startup.reason)
                <<"\",\"request_id\":"<<startup.request_id<<'}';
#define SURFXR_TELEMETRY_FIELD(name) record<<",\"" #name "\":"<<metrics.name
            SURFXR_TELEMETRY_FIELD(keyboard_active);SURFXR_TELEMETRY_FIELD(keyboard_supported);
            SURFXR_TELEMETRY_FIELD(keyboard_images_received);SURFXR_TELEMETRY_FIELD(keyboard_candidate_frames);
            SURFXR_TELEMETRY_FIELD(sample_monotonic_ms);
            SURFXR_TELEMETRY_FIELD(projection_width);SURFXR_TELEMETRY_FIELD(projection_height);SURFXR_TELEMETRY_FIELD(desktop_visible_mask);
            SURFXR_TELEMETRY_FIELD(desktop_layout_fingerprint);SURFXR_TELEMETRY_FIELD(desktop_stage_copy_bytes);
            SURFXR_TELEMETRY_FIELD(desktop_direct_upload_frames);SURFXR_TELEMETRY_FIELD(desktop_direct_upload_bytes);SURFXR_TELEMETRY_FIELD(desktop_edge_patch_bytes);
            SURFXR_TELEMETRY_FIELD(desktop_uploads);SURFXR_TELEMETRY_FIELD(desktop_cached_upload_skips);
            SURFXR_TELEMETRY_FIELD(desktop_stage_copy_ms_sum);SURFXR_TELEMETRY_FIELD(desktop_swapchain_wait_calls);SURFXR_TELEMETRY_FIELD(desktop_swapchain_wait_ms_sum);
            SURFXR_TELEMETRY_FIELD(desktop_end_frame_ms_sum);SURFXR_TELEMETRY_FIELD(desktop_frame_work_ms_sum);
            SURFXR_TELEMETRY_FIELD(desktop_gpu_timestamps);SURFXR_TELEMETRY_FIELD(desktop_mip_gpu_samples);SURFXR_TELEMETRY_FIELD(desktop_projection_gpu_samples);
            SURFXR_TELEMETRY_FIELD(desktop_mip_gpu_ms_sum);SURFXR_TELEMETRY_FIELD(desktop_mip_gpu_ms_max);
            SURFXR_TELEMETRY_FIELD(desktop_projection_gpu_ms_sum);SURFXR_TELEMETRY_FIELD(desktop_projection_gpu_ms_max);
            SURFXR_TELEMETRY_FIELD(desktop_nv12_encoded_mip_updates);SURFXR_TELEMETRY_FIELD(desktop_nv12_mip_updates);SURFXR_TELEMETRY_FIELD(desktop_nv12_intermediate_bytes_avoided);
            SURFXR_TELEMETRY_FIELD(desktop_nv12_base_updates);SURFXR_TELEMETRY_FIELD(desktop_nv12_base_reuses);SURFXR_TELEMETRY_FIELD(desktop_nv12_encoded_base_updates);
            SURFXR_TELEMETRY_FIELD(desktop_nv12_gpu_samples);SURFXR_TELEMETRY_FIELD(desktop_nv12_gpu_ms_sum);
            const auto panel_array=[&](const char* name,const auto& values){record<<",\""<<name<<"\":[";
                for(unsigned i=0;i<values.size();++i){if(i)record<<',';record<<values[i];}record<<']';};
            panel_array("desktop_decoded_width",metrics.desktop_decoded_width);
            panel_array("desktop_decoded_height",metrics.desktop_decoded_height);
            panel_array("desktop_panel_new_frames",metrics.desktop_panel_new_frames);
            SURFXR_TELEMETRY_FIELD(vr_timewarp_frames);SURFXR_TELEMETRY_FIELD(vr_timewarp_fallback_frames);
            SURFXR_TELEMETRY_FIELD(vr_submitted_frames);SURFXR_TELEMETRY_FIELD(vr_new_stereo_frames);SURFXR_TELEMETRY_FIELD(vr_reused_stereo_frames);
            SURFXR_TELEMETRY_FIELD(vr_submit_intervals);SURFXR_TELEMETRY_FIELD(vr_submit_interval_ms_sum);
            SURFXR_TELEMETRY_FIELD(vr_decoded_age_samples);SURFXR_TELEMETRY_FIELD(vr_decoded_age_ms_sum);
            SURFXR_TELEMETRY_FIELD(vr_pose_sample_age_samples);SURFXR_TELEMETRY_FIELD(vr_pose_sample_age_ms_sum);
            SURFXR_TELEMETRY_FIELD(vr_submitted_pose_age_samples);SURFXR_TELEMETRY_FIELD(vr_submitted_pose_age_ms_sum);SURFXR_TELEMETRY_FIELD(vr_submitted_pose_age_ms_max);
            SURFXR_TELEMETRY_FIELD(vr_foveation_frames);SURFXR_TELEMETRY_FIELD(vr_foveation_gpu_wait_ms_sum);
            SURFXR_TELEMETRY_FIELD(vr_deferred_gpu_submissions);SURFXR_TELEMETRY_FIELD(vr_deferred_gpu_completions);SURFXR_TELEMETRY_FIELD(vr_foveation_gpu_samples);
            SURFXR_TELEMETRY_FIELD(vr_upload_wait_calls);SURFXR_TELEMETRY_FIELD(vr_upload_wait_ms_sum);SURFXR_TELEMETRY_FIELD(vr_upload_wait_ms_max);
            SURFXR_TELEMETRY_FIELD(vr_foveation_gpu_ms_sum);SURFXR_TELEMETRY_FIELD(vr_foveation_gpu_ms_max);
            SURFXR_TELEMETRY_FIELD(vr_stage_calls);SURFXR_TELEMETRY_FIELD(vr_stage_ms_sum);SURFXR_TELEMETRY_FIELD(vr_stage_ms_max);
            SURFXR_TELEMETRY_FIELD(vr_stage_copy_ms_sum);SURFXR_TELEMETRY_FIELD(vr_stage_copy_ms_max);
            SURFXR_TELEMETRY_FIELD(vr_direct_upload_frames);SURFXR_TELEMETRY_FIELD(vr_direct_upload_bytes);
            SURFXR_TELEMETRY_FIELD(vr_direct_sample_frames);SURFXR_TELEMETRY_FIELD(vr_upload_avoided_bytes);
            SURFXR_TELEMETRY_FIELD(vr_paired_frames);SURFXR_TELEMETRY_FIELD(vr_queued_presentations);
            SURFXR_TELEMETRY_FIELD(vr_pair_queue_overflows);SURFXR_TELEMETRY_FIELD(vr_pair_queue_stale);SURFXR_TELEMETRY_FIELD(vr_pair_queue_skipped);
            SURFXR_TELEMETRY_FIELD(vr_pair_queue_depth);SURFXR_TELEMETRY_FIELD(vr_playout_buffer_holds);SURFXR_TELEMETRY_FIELD(vr_playout_reserve_ms);
            SURFXR_TELEMETRY_FIELD(vr_swapchain_wait_calls);SURFXR_TELEMETRY_FIELD(vr_swapchain_wait_ms_sum);SURFXR_TELEMETRY_FIELD(vr_swapchain_wait_ms_max);
            SURFXR_TELEMETRY_FIELD(vr_end_frame_ms_sum);SURFXR_TELEMETRY_FIELD(vr_end_frame_ms_max);SURFXR_TELEMETRY_FIELD(vr_frame_work_ms_sum);SURFXR_TELEMETRY_FIELD(vr_frame_work_ms_max);
            SURFXR_TELEMETRY_FIELD(motion_sync_updates);SURFXR_TELEMETRY_FIELD(motion_sync_latency_ms);SURFXR_TELEMETRY_FIELD(motion_sync_previous_draw_ms);
            const auto surfxr_begin=record.tellp();
            if(metrics.surfxr_timing_available){SURFXR_TELEMETRY_FIELD(surfxr_timing_available);SURFXR_TELEMETRY_FIELD(surfxr_total_ms);SURFXR_TELEMETRY_FIELD(surfxr_upstream_ms);
                SURFXR_TELEMETRY_FIELD(surfxr_decode_ms);SURFXR_TELEMETRY_FIELD(surfxr_after_decode_ms);}
            const auto surfxr_end=record.tellp();
            SURFXR_TELEMETRY_FIELD(motion_sync_gpu_estimated_frames);SURFXR_TELEMETRY_FIELD(motion_sync_gpu_unavailable_frames);
            SURFXR_TELEMETRY_FIELD(motion_sync_gpu_tail_ms_sum);SURFXR_TELEMETRY_FIELD(motion_sync_gpu_tail_ms_max);
            SURFXR_TELEMETRY_FIELD(prediction_time_valid_samples);SURFXR_TELEMETRY_FIELD(prediction_lead_ms_mean);
            SURFXR_TELEMETRY_FIELD(ipd_samples);SURFXR_TELEMETRY_FIELD(ipd_events);SURFXR_TELEMETRY_FIELD(ipd_fallback_samples);
            SURFXR_TELEMETRY_FIELD(current_refresh_hz);SURFXR_TELEMETRY_FIELD(render_latency_ms);SURFXR_TELEMETRY_FIELD(render_latency_valid);
            SURFXR_TELEMETRY_FIELD(requested_refresh_hz);SURFXR_TELEMETRY_FIELD(refresh_rate_pending);
            SURFXR_TELEMETRY_FIELD(refresh_rate_requests);SURFXR_TELEMETRY_FIELD(refresh_rate_request_failures);SURFXR_TELEMETRY_FIELD(last_refresh_request_result);
            SURFXR_TELEMETRY_FIELD(vr_eye_width);SURFXR_TELEMETRY_FIELD(vr_eye_height);
            SURFXR_TELEMETRY_FIELD(vr_decoded_eye_width);SURFXR_TELEMETRY_FIELD(vr_decoded_eye_height);
            SURFXR_TELEMETRY_FIELD(vr_focus_width);SURFXR_TELEMETRY_FIELD(vr_focus_height);
            panel_array("vr_focus_center_x",metrics.vr_focus_center_x);panel_array("vr_focus_center_y",metrics.vr_focus_center_y);
            SURFXR_TELEMETRY_FIELD(requested_foveated_streaming);SURFXR_TELEMETRY_FIELD(eye_gaze_available);SURFXR_TELEMETRY_FIELD(vr_foveated_streaming);
            SURFXR_TELEMETRY_FIELD(eye_gaze_samples);SURFXR_TELEMETRY_FIELD(eye_gaze_invalid);
            SURFXR_TELEMETRY_FIELD(system_click_available);SURFXR_TELEMETRY_FIELD(system_touch_available);SURFXR_TELEMETRY_FIELD(system_capture_active);
            SURFXR_TELEMETRY_FIELD(system_capture_exit_serial);SURFXR_TELEMETRY_FIELD(controller_skeleton_active_mask);
            SURFXR_TELEMETRY_FIELD(controller_skeleton_left_frames);SURFXR_TELEMETRY_FIELD(controller_skeleton_right_frames);
            SURFXR_TELEMETRY_FIELD(transition_frames);
            SURFXR_TELEMETRY_FIELD(transition_cached_frames);
            SURFXR_TELEMETRY_FIELD(transition_placeholder_frames);
            SURFXR_TELEMETRY_FIELD(last_layer_count);
            SURFXR_TELEMETRY_FIELD(last_enabled_desktop_mask);
            SURFXR_TELEMETRY_FIELD(last_ready_desktop_mask);
            SURFXR_TELEMETRY_FIELD(last_cached_desktop_mask);
            SURFXR_TELEMETRY_FIELD(last_should_render);
            SURFXR_TELEMETRY_FIELD(last_ui_anchored);
            SURFXR_TELEMETRY_FIELD(last_vr_requested);
            SURFXR_TELEMETRY_FIELD(last_vr_pair_valid);
            SURFXR_TELEMETRY_FIELD(last_stage_locatable);
            SURFXR_TELEMETRY_FIELD(last_transition_pending);
            SURFXR_TELEMETRY_FIELD(vr_keyframe_recoveries);SURFXR_TELEMETRY_FIELD(vr_restart_recoveries);
            SURFXR_TELEMETRY_FIELD(mode_source_mismatches);SURFXR_TELEMETRY_FIELD(desktop_source_retries);
            SURFXR_TELEMETRY_FIELD(desktop_keyframe_recoveries);SURFXR_TELEMETRY_FIELD(desktop_restart_recoveries);SURFXR_TELEMETRY_FIELD(desktop_timeouts);
            SURFXR_TELEMETRY_FIELD(desktop_capture_supported);SURFXR_TELEMETRY_FIELD(desktop_capture_requested_mask);SURFXR_TELEMETRY_FIELD(desktop_capture_accepted_mask);
            SURFXR_TELEMETRY_FIELD(desktop_pending);SURFXR_TELEMETRY_FIELD(desktop_failed);
            SURFXR_TELEMETRY_FIELD(requested_desktop_pointer_priority);
            SURFXR_TELEMETRY_FIELD(requested_vr_fps);SURFXR_TELEMETRY_FIELD(requested_vr_quality);SURFXR_TELEMETRY_FIELD(requested_vr_bitrate_mbps);
            SURFXR_TELEMETRY_FIELD(active_codec);SURFXR_TELEMETRY_FIELD(max_vr_bitrate_mbps);SURFXR_TELEMETRY_FIELD(max_vr_quality);
            SURFXR_TELEMETRY_FIELD(last_setting_request_id);SURFXR_TELEMETRY_FIELD(last_setting_request_value);
            record<<",\"last_setting_request_accepted\":"<<(metrics.last_setting_request_accepted?"true":"false");
            SURFXR_TELEMETRY_FIELD(last_setting_request_serial);
            SURFXR_TELEMETRY_FIELD(desktop_filter);SURFXR_TELEMETRY_FIELD(vr_sharpening);
            record<<",\"vr_upscaling\":"<<(metrics.vr_upscaling?"true":"false")
                  <<",\"vr_timewarp\":"<<(metrics.vr_timewarp?"true":"false")<<",\"vr_sharpening_enabled\":"<<(metrics.vr_sharpening_enabled?"true":"false")
                  <<",\"last_quality_request_accepted\":"<<(metrics.last_quality_request_accepted?"true":"false");
            SURFXR_TELEMETRY_FIELD(last_quality_request_id);SURFXR_TELEMETRY_FIELD(last_quality_request_value);SURFXR_TELEMETRY_FIELD(last_quality_request_serial);
            SURFXR_TELEMETRY_FIELD(performance_overlay_frames);
            SURFXR_TELEMETRY_FIELD(performance_overlay_updates);SURFXR_TELEMETRY_FIELD(performance_overlay_budget_omissions);
            record<<",\"performance_overlay\":"<<(metrics.performance_overlay?"true":"false");
            record<<",\"gaze_debug_overlay\":"<<(metrics.gaze_debug_overlay?"true":"false");
            record<<",\"boundary_sync_enabled\":"<<(metrics.boundary_sync_enabled?"true":"false");
            record<<",\"sync_raw_battery\":"<<(metrics.sync_raw_battery?"true":"false");
            record<<",\"transition_hud_enabled\":"<<(metrics.transition_hud_enabled?"true":"false");
            record<<",\"mini_hud_enabled\":"<<(metrics.mini_hud_enabled?"true":"false")
                  <<",\"mini_hud_fps\":"<<(metrics.mini_hud_fps?"true":"false")
                  <<",\"mini_hud_latency\":"<<(metrics.mini_hud_latency?"true":"false")
                  <<",\"mini_hud_frame_time\":"<<(metrics.mini_hud_frame_time?"true":"false")
                  <<",\"mini_hud_decode\":"<<(metrics.mini_hud_decode?"true":"false")
                  <<",\"mini_hud_gaze_only\":"<<(metrics.mini_hud_gaze_only?"true":"false")
                  <<",\"mini_hud_gaze_marker\":"<<(metrics.mini_hud_gaze_marker?"true":"false");
            SURFXR_TELEMETRY_FIELD(mini_hud_x);SURFXR_TELEMETRY_FIELD(mini_hud_y);SURFXR_TELEMETRY_FIELD(mini_hud_scale);
            SURFXR_TELEMETRY_FIELD(hmd_raw_battery_percent);
            SURFXR_TELEMETRY_FIELD(gaze_debug_frames);SURFXR_TELEMETRY_FIELD(gaze_debug_eye_layers);SURFXR_TELEMETRY_FIELD(gaze_debug_budget_omissions);
            record<<",\"vr_controllers_enabled\":"<<(metrics.vr_controllers_enabled?"true":"false")
                  <<",\"last_input_request_accepted\":"<<(metrics.last_input_request_accepted?"true":"false");
            SURFXR_TELEMETRY_FIELD(last_input_request_id);SURFXR_TELEMETRY_FIELD(last_input_request_value);SURFXR_TELEMETRY_FIELD(last_input_request_serial);
            SURFXR_TELEMETRY_FIELD(hmd_battery_percent);SURFXR_TELEMETRY_FIELD(left_battery_percent);SURFXR_TELEMETRY_FIELD(right_battery_percent);
            record<<",\"hmd_battery_charging\":"<<(metrics.hmd_battery_charging?"true":"false")
                  <<",\"left_battery_charging\":"<<(metrics.left_battery_charging?"true":"false")
                  <<",\"right_battery_charging\":"<<(metrics.right_battery_charging?"true":"false");
            SURFXR_TELEMETRY_FIELD(desktop_quality_frames);SURFXR_TELEMETRY_FIELD(desktop_mip_updates);SURFXR_TELEMETRY_FIELD(desktop_mip_reuses);
            SURFXR_TELEMETRY_FIELD(vr_enhancement_frames);SURFXR_TELEMETRY_FIELD(vr_upscaled_frames);SURFXR_TELEMETRY_FIELD(vr_enhancement_gpu_wait_ms_sum);
            SURFXR_TELEMETRY_FIELD(vr_enhancement_gpu_samples);SURFXR_TELEMETRY_FIELD(vr_enhancement_gpu_ms_sum);SURFXR_TELEMETRY_FIELD(vr_raw_enhancement_frames);
            SURFXR_TELEMETRY_FIELD(vr_fused_postprocess_frames);
            SURFXR_TELEMETRY_FIELD(vr_enhanced_width);SURFXR_TELEMETRY_FIELD(vr_enhanced_height);
#undef SURFXR_TELEMETRY_FIELD
            record<<",\"decoder_sample_monotonic_ms\":"<<metrics.decode.sample_monotonic_ms;
            std::streampos desktop_begin{},desktop_end{};
            for(unsigned kind=0;kind<2;++kind){
            if(kind)desktop_begin=record.tellp();
            record<<(kind?",\"desktop_decoders\":[":",\"vr_decoders\":[");
            const auto& decoders=kind?metrics.decode.desktop_decoders:metrics.decode.vr_decoders;
            for(unsigned i=0;i<decoders.size();++i){
                if(i)record<<',';
                const auto& decoder=decoders[i];
                if(kind&&!decoder.active){record<<"{\"active\":false}";continue;}
                record<<"{\"active\":"<<(decoder.active?"true":"false");
#define SURFXR_DECODER_FIELD(name) record<<",\"" #name "\":"<<decoder.name
                if(kind){
                    // Keep the existing 16 KiB status-reader budget even
                    // with four active desktop and four inactive VR decoders.
                    SURFXR_DECODER_FIELD(frames);SURFXR_DECODER_FIELD(packets);SURFXR_DECODER_FIELD(configuration_changes);
                    SURFXR_DECODER_FIELD(decode_time_ns);SURFXR_DECODER_FIELD(rgba_conversion_time_ns);
                    SURFXR_DECODER_FIELD(rgba_converted_frames);SURFXR_DECODER_FIELD(rgba_skipped_frames);
                    SURFXR_DECODER_FIELD(nv12_published_frames);SURFXR_DECODER_FIELD(nv12_copy_time_ns);SURFXR_DECODER_FIELD(nv12_pool_bytes);
                    SURFXR_DECODER_FIELD(nv12_zero_copy_frames);SURFXR_DECODER_FIELD(nv12_zero_copy_time_ns);SURFXR_DECODER_FIELD(nv12_zero_copy_fallback_frames);
                    SURFXR_DECODER_FIELD(metadata_unmatched);SURFXR_DECODER_FIELD(sequence_restarts);
                    SURFXR_DECODER_FIELD(width);SURFXR_DECODER_FIELD(height);record<<'}';continue;
                }
                SURFXR_DECODER_FIELD(packets);SURFXR_DECODER_FIELD(frames);SURFXR_DECODER_FIELD(configuration_changes);SURFXR_DECODER_FIELD(decode_time_ns);
                SURFXR_DECODER_FIELD(avcodec_send_time_ns);SURFXR_DECODER_FIELD(avcodec_receive_time_ns);SURFXR_DECODER_FIELD(rgba_conversion_time_ns);
                SURFXR_DECODER_FIELD(rgba_converted_frames);SURFXR_DECODER_FIELD(rgba_skipped_frames);SURFXR_DECODER_FIELD(rgba_suppressed_publications);
                SURFXR_DECODER_FIELD(nv12_published_frames);SURFXR_DECODER_FIELD(nv12_copy_time_ns);SURFXR_DECODER_FIELD(nv12_pool_bytes);
                SURFXR_DECODER_FIELD(nv12_zero_copy_frames);SURFXR_DECODER_FIELD(nv12_zero_copy_time_ns);SURFXR_DECODER_FIELD(nv12_zero_copy_fallback_frames);
                SURFXR_DECODER_FIELD(rgba_buffer_allocations);SURFXR_DECODER_FIELD(rgba_buffer_reuses);SURFXR_DECODER_FIELD(rgba_temporary_allocations);SURFXR_DECODER_FIELD(rgba_pool_bytes);
                SURFXR_DECODER_FIELD(metadata_unmatched);SURFXR_DECODER_FIELD(sequence_restarts);
                SURFXR_DECODER_FIELD(async_queue_packets_max);SURFXR_DECODER_FIELD(async_queue_bytes_max);SURFXR_DECODER_FIELD(async_backpressure_wait_ns);SURFXR_DECODER_FIELD(async_discarded_packets);
                SURFXR_DECODER_FIELD(callback_packets);SURFXR_DECODER_FIELD(packet_intervals);SURFXR_DECODER_FIELD(packet_interval_ns_sum);
                SURFXR_DECODER_FIELD(width);SURFXR_DECODER_FIELD(height);SURFXR_DECODER_FIELD(color_space);SURFXR_DECODER_FIELD(color_range);
                SURFXR_DECODER_FIELD(effective_color_space);SURFXR_DECODER_FIELD(effective_color_range);
#undef SURFXR_DECODER_FIELD
                record<<'}';
            }
            record<<']';if(kind)desktop_end=record.tellp();}
            record<<",\"test_head_motion\":{\"enabled\":"<<(metrics.test_head_motion.enabled?"true":"false");
#define SURFXR_MOTION_FIELD(name) record<<",\"" #name "\":"<<metrics.test_head_motion.name
            SURFXR_MOTION_FIELD(samples);SURFXR_MOTION_FIELD(sent_packets);SURFXR_MOTION_FIELD(elapsed_seconds);
            SURFXR_MOTION_FIELD(yaw_degrees);SURFXR_MOTION_FIELD(pitch_degrees);SURFXR_MOTION_FIELD(feedback_frames);
            SURFXR_MOTION_FIELD(matched_feedback_frames);SURFXR_MOTION_FIELD(feedback_tracking_id);
            SURFXR_MOTION_FIELD(feedback_rotation_degrees);SURFXR_MOTION_FIELD(feedback_rotation_max_degrees);SURFXR_MOTION_FIELD(feedback_error_degrees);
#undef SURFXR_MOTION_FIELD
            record<<'}';
            record<<",\"tracking_upload\":{\"sample_monotonic_ns\":"<<tracking.sample_monotonic_ns;
#define SURFXR_TRACKING_FIELD(name) record<<",\"" #name "\":"<<tracking.name
            SURFXR_TRACKING_FIELD(qos_requested);SURFXR_TRACKING_FIELD(qos_applied);SURFXR_TRACKING_FIELD(qos_tos);SURFXR_TRACKING_FIELD(qos_priority);
            SURFXR_TRACKING_FIELD(qos_tos_errno);SURFXR_TRACKING_FIELD(qos_priority_errno);
            SURFXR_TRACKING_FIELD(requested_hz);SURFXR_TRACKING_FIELD(packets);SURFXR_TRACKING_FIELD(missed_deadlines);
            SURFXR_TRACKING_FIELD(fresh_pose_packets);SURFXR_TRACKING_FIELD(repeated_pose_packets);SURFXR_TRACKING_FIELD(invalid_pose_packets);
            SURFXR_TRACKING_FIELD(view_config_packets);SURFXR_TRACKING_FIELD(menu_press_edges);
            SURFXR_TRACKING_FIELD(focused);
            panel_array("hand_pose_active",tracking.hand_pose_active);panel_array("hand_input_active",tracking.hand_input_active);
            panel_array("hand_ui_captured",tracking.hand_ui_captured);
            panel_array("fresh_hand_packets",tracking.fresh_hand_packets);panel_array("cached_hand_packets",tracking.cached_hand_packets);
            SURFXR_TRACKING_FIELD(interval_samples);SURFXR_TRACKING_FIELD(interval_ns_sum);SURFXR_TRACKING_FIELD(interval_ns_max);
            SURFXR_TRACKING_FIELD(send_duration_ns_sum);SURFXR_TRACKING_FIELD(send_duration_ns_max);
            SURFXR_TRACKING_FIELD(encoded_bytes_sum);SURFXR_TRACKING_FIELD(encoded_bytes_max);
            SURFXR_TRACKING_FIELD(sample_age_samples);SURFXR_TRACKING_FIELD(sample_age_ns_sum);SURFXR_TRACKING_FIELD(sample_age_ns_max);
            SURFXR_TRACKING_FIELD(window_started_ns);SURFXR_TRACKING_FIELD(window_ended_ns);SURFXR_TRACKING_FIELD(window_packets);
            SURFXR_TRACKING_FIELD(window_interval_ns_max);SURFXR_TRACKING_FIELD(window_send_duration_ns_max);SURFXR_TRACKING_FIELD(window_sample_age_ns_max);
            SURFXR_TRACKING_FIELD(clock_ready);SURFXR_TRACKING_FIELD(clock_samples);SURFXR_TRACKING_FIELD(clock_revision);
            SURFXR_TRACKING_FIELD(clock_accepted);SURFXR_TRACKING_FIELD(clock_rejected);
            SURFXR_TRACKING_FIELD(clock_error_ns);SURFXR_TRACKING_FIELD(clock_reply_age_ns);SURFXR_TRACKING_FIELD(clock_min_rtt_ns);
            SURFXR_TRACKING_FIELD(clock_offset_ns);SURFXR_TRACKING_FIELD(clock_rate_ppb);
            SURFXR_TRACKING_FIELD(clock_rate_low_ppb);SURFXR_TRACKING_FIELD(clock_rate_high_ppb);
            SURFXR_TRACKING_FIELD(clock_t1);SURFXR_TRACKING_FIELD(clock_t2);SURFXR_TRACKING_FIELD(clock_t3);SURFXR_TRACKING_FIELD(clock_t4);
#undef SURFXR_TRACKING_FIELD
            record<<'}';
            record<<",\"audio_output\":{\"requested\":"<<(audio.requested?"true":"false")
                <<",\"supported\":"<<(audio.supported?"true":"false")
                <<",\"sample_monotonic_ms\":"<<audio.sample_monotonic_ms<<",\"received_pcm_bytes\":"<<audio.received_pcm_bytes;
#define SURFXR_AUDIO_BOOL(name) record<<",\"" #name "\":"<<(audio.stats.name?"true":"false")
            SURFXR_AUDIO_BOOL(initialized);SURFXR_AUDIO_BOOL(ready);SURFXR_AUDIO_BOOL(reconnecting);SURFXR_AUDIO_BOOL(failed);SURFXR_AUDIO_BOOL(stopped);
#undef SURFXR_AUDIO_BOOL
#define SURFXR_AUDIO_FIELD(name) record<<",\"" #name "\":"<<audio.stats.name
            SURFXR_AUDIO_FIELD(pulse_error);SURFXR_AUDIO_FIELD(context_state);SURFXR_AUDIO_FIELD(stream_state);
            SURFXR_AUDIO_FIELD(connection_attempts);SURFXR_AUDIO_FIELD(successful_connections);SURFXR_AUDIO_FIELD(reconnects);
            SURFXR_AUDIO_FIELD(connection_failures);SURFXR_AUDIO_FIELD(queue_resets);
            SURFXR_AUDIO_FIELD(submitted_bytes);SURFXR_AUDIO_FIELD(pcm_frames_consumed);SURFXR_AUDIO_FIELD(silence_frames);
            SURFXR_AUDIO_FIELD(dropped_old_frames);SURFXR_AUDIO_FIELD(dropped_contention_frames);
            SURFXR_AUDIO_FIELD(underrun_callbacks);SURFXR_AUDIO_FIELD(server_underflows);SURFXR_AUDIO_FIELD(invalid_submissions);
            SURFXR_AUDIO_FIELD(submissions_after_stop);SURFXR_AUDIO_FIELD(write_callbacks);SURFXR_AUDIO_FIELD(written_frames);
            SURFXR_AUDIO_FIELD(queued_frames);SURFXR_AUDIO_FIELD(max_queued_frames);SURFXR_AUDIO_FIELD(partial_bytes);
            SURFXR_AUDIO_FIELD(server_buffer_bytes);SURFXR_AUDIO_FIELD(server_target_bytes);
            SURFXR_AUDIO_FIELD(buffering_target_frames);SURFXR_AUDIO_FIELD(rebuffer_callbacks);SURFXR_AUDIO_FIELD(rebuffer_events);SURFXR_AUDIO_FIELD(transition_frames);
#undef SURFXR_AUDIO_FIELD
            record<<'}';
            record<<",\"surfxr_receive\":";transport::write_receive_json(record,metrics.surfxr_receive);
            record<<"}\n";
            auto data=record.str();
            // Optional diagnostics must never suppress lifecycle/settings
            // acknowledgments when retained long-running counters are large.
            if(data.size()>viewer_status_max_bytes)data.erase(static_cast<std::size_t>(desktop_begin),
                static_cast<std::size_t>(desktop_end-desktop_begin));
            if(data.size()>viewer_status_max_bytes)data.erase(static_cast<std::size_t>(surfxr_begin),
                static_cast<std::size_t>(surfxr_end-surfxr_begin));
            if(data.size()>viewer_status_max_bytes){
                const auto receive_begin=data.rfind(",\"surfxr_receive\":");
                if(receive_begin!=std::string::npos)data.erase(receive_begin,data.size()-receive_begin-2);
            }
            return data.size()<=viewer_status_max_bytes&&write_atomic(path_,temporary_,data);
        }catch(...){return false;}
    }
    void run(){
        for(;;){
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock,std::chrono::seconds(2),[&]{return dirty_||done_;});
            const auto state=state_;const bool vr=vr_,done=done_;const auto metrics=telemetry_;
            const auto tracking=tracking_upload_;
            const auto audio=audio_output_;
            const auto startup=steamvr_startup_;
            const auto mode=mode_timing_;const auto requested_vr=requested_vr_;
            const auto timing=timing_dirty_?timing_:std::nullopt;
            dirty_=timing_dirty_=false;lock.unlock();
            write(state,vr,metrics,tracking,audio,startup);write_mode(mode,requested_vr);write_haptics(metrics);if(timing)write_timing(*timing);if(done)return;
        }
    }
    std::string path_,temporary_;
    std::mutex mutex_;std::condition_variable wake_;std::thread worker_;
    unsigned state_=0;bool vr_=false,dirty_=false,done_=false;
    ViewerTelemetry telemetry_{};
    TrackingUploadTelemetry tracking_upload_{};
    ViewerAudioTelemetry audio_output_{};
    SteamVrStartupStatus steamvr_startup_{};
    ViewerModeTiming mode_timing_{};bool requested_vr_=false;
    std::optional<FrameTimingSnapshot> timing_;
    bool timing_dirty_=false;
};
} // namespace surfxr
