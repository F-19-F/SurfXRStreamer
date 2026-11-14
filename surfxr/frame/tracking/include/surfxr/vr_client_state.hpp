#pragma once
#include "surfxr/xr_display.hpp"
#include "surfxr/vr_wire.hpp"
#include "surfxr/remote_settings.hpp"
#include <deque>
#include <atomic>
#include <functional>
#include <mutex>

namespace surfxr {
struct VrClientStats {
    std::uint64_t enter_requests=0,desktop_requests=0,launch_requests=0,exit_requests=0;
    std::uint64_t paired_frames=0,invalid_frames=0,unmatched_epochs=0,old_epochs=0;
    std::uint64_t paired_queue_overflows=0,paired_queue_stale=0,paired_queue_skipped=0;
    // Unique successful ACKs, including a selected pair evicted while XR was
    // rendering it. Queue evictions therefore are not a count of unseen frames.
    std::uint64_t queued_presentations=0;
    std::uint64_t paired_queue_depth=0;
    std::uint64_t playout_reserve_ns=0,playout_buffer_holds=0;
    std::uint64_t origin_changes=0,start_timeouts=0,settings_rejected=0;
    std::uint64_t missing_metadata=0,invalid_pose=0,invalid_fov=0,mismatched_timestamp=0;
    std::uint64_t duplicate_enter_requests=0,duplicate_desktop_requests=0,source_mismatches=0;
    std::uint64_t vr_source_retries=0,vr_keyframe_recoveries=0,vr_restart_recoveries=0;
    std::uint64_t start_commands_sent=0;
    std::uint64_t desktop_source_retries=0,desktop_timeouts=0;
    std::uint64_t desktop_keyframe_recoveries=0,desktop_restart_recoveries=0;
    bool requested_vr=false,presenting_vr=false,desktop_pending=false;
    bool desktop_failed=false;
    bool start_command_pending=false;
    const char* error=""; // fixed local reason, never remote text
};
// Coordinates XR requests with received configuration and matched decoded eyes.
// No network or decoder calls here; all commands enter the bounded transport queue.
class VrClientState {
public:
    // Runtime display modes are retained as an API argument, but do not limit
    // remote video rates. The XR owner handles display refresh requests.
    VrClientState(SessionControlQueue& control,RemoteSettings& settings,
                  const std::vector<float>& refresh_rates,bool microphone_available,
                  unsigned max_render_width=8192,unsigned max_render_height=8192,
                  std::function<std::uint64_t()> clock={});
    void connected(bool value);
    void set_vr_only(bool value);
    void set_desktop_only(bool value);
    void set_surfxr_controls(std::function<bool(SettingChange)> setting,std::function<bool()> exit);
    void set_mode_request(std::function<bool(bool)> request);
    void tracking(const TrackingFrame& frame);
    bool tracking_snapshot(TrackingFrame& frame) const;
    void tracking_sent(const TrackingFrame& frame);
    // Called after a complete local control record is written, not when the
    // user request is queued behind the bandwidth exchange.
    void control_command_sent(const SessionControlCommand& command);
    bool toggle();
    bool launch();
    bool exit_vr();
    bool desktop();
    bool monitor(bool add);
    void desktop_monitors(unsigned count,unsigned capacity=0);
    bool setting(SettingChange change);
    void recenter();
    void source_config(std::uint8_t source,std::uint32_t codec);
    // Current desktop decoder's synchronous publication, including idle poll.
    // Only the primary monitor completes a desktop first-frame transition.
    void desktop_decoded(unsigned stream);
    // Receiver-owned recovery also runs when XR is not producing frames.
    void tick();
    void vr_configuration(const VrConfiguration& configuration);
    // Local XR output demand only: retain the live stream and tracking ledger.
    // Calling this first opts into epoch validation; legacy callers remain
    // compatible with frames whose output_epoch is zero.
    void set_renderable(bool renderable);
    // Pause desktop first-frame recovery while its primary image is hidden.
    void set_desktop_renderable(bool renderable);
    // Zero suppresses RGBA output. Each resumed interval gets a fresh token
    // carried from packet submission through decoder publication.
    std::uint64_t frame_demand_epoch() const noexcept {return frame_demand_epoch_.load(std::memory_order_acquire);}
    // Stream IDs: ordinary L/R=0/1; gaze L base/focus,R base/focus=0/1/2/3.
    void decoded(unsigned stream,std::shared_ptr<const VideoFrame> frame);
    std::shared_ptr<const VrStereoFrame> stereo() const;
    // Keep the matching
    // pixels/pose/FOV together, and acknowledge the exact submitted pair only
    // after xrEndFrame succeeds; repeated submissions need no new entry.
    std::shared_ptr<const VrStereoFrame> presentation_stereo();
    void stereo_presented(std::shared_ptr<const VrStereoFrame> frame);
    RemoteSettingsSnapshot settings_snapshot();
    VrClientStats statistics() const;
private:
    static bool stage_ready(const TrackingFrame&);
    bool enter_locked();
    bool desktop_locked();
    void capabilities_locked();
    void clear_video_locked();
    void tick_locked(std::uint64_t now_ns);
    std::uint64_t clock_ns() const;
    void begin_vr_wait_locked(std::uint64_t now_ns);
    void begin_desktop_wait_locked(std::uint64_t now_ns);
    void pair_locked();
    SessionControlQueue& control_;
    RemoteSettings& settings_;
    mutable std::mutex mutex_;
    TrackingFrame tracking_{};
    bool connected_=false,have_tracking_=false,microphone_available_=false;
    bool vr_only_=false;
    bool desktop_only_=false;
    std::function<bool(bool)> mode_request_;
    std::function<bool(SettingChange)> surfxr_setting_;
    std::function<bool()> surfxr_exit_;
    bool frame_demand_enabled_=false;
    std::atomic<std::uint64_t> frame_demand_epoch_{0};
    std::uint64_t next_frame_demand_epoch_=0,frame_demand_paused_ns_=0;
    std::uint8_t received_source_=0;
    bool vr_waiting_=false;
    std::uint64_t start_transition_id_=0;
    std::uint64_t last_source_request_ns_=0;
    std::uint64_t vr_wait_started_ns_=0,desktop_deadline_ns_=0,desktop_last_request_ns_=0;
    std::uint64_t desktop_wait_started_ns_=0;
    unsigned desktop_retries_=0;
    bool source_known_=false,desktop_failed_=false,vr_config_wait_=false,keyframe_recovery_attempted_=false,restart_recovery_attempted_=false;
    bool desktop_ready_=false,desktop_keyframe_attempted_=false,desktop_restart_attempted_=false;
    bool desktop_renderable_=true;std::uint64_t desktop_paused_ns_=0;
    std::uint64_t monitor_deadline_ns_=0;
    unsigned monitor_previous_count_=0;
    bool monitor_add_=false;
    bool monitor_sent_=false;
    std::function<std::uint64_t()> clock_;
    std::vector<int> stream_framerates_;
    unsigned max_render_width_=8192,max_render_height_=8192;
    int max_quality_=7;
    int vr_codec_=0; // Desktop configuration may use a different encoder.
    unsigned vr_frame_rate_=0;
    struct Sent {
        std::int32_t id;
        std::uint64_t epoch,sampled_monotonic_ns,query_monotonic_ns;
        std::int64_t query_xr_time,target_xr_time;
    };
    std::deque<Sent> sent_;
    std::array<std::deque<std::shared_ptr<const VideoFrame>>,4> eyes_;
    std::array<std::uint64_t,4> generations_{};
    unsigned stream_count_=2;
    std::uint32_t reconstruction_width_=0,reconstruction_height_=0;
    std::shared_ptr<const VrStereoFrame> stereo_;
    struct PendingStereo {
        std::shared_ptr<const VrStereoFrame> frame;
        std::uint64_t ready_monotonic_ns;
    };
    std::deque<PendingStereo> presentation_;
    // The latest early/late poll is the candidate held by the XR owner. Keep
    // its identity valid if newer decoded pairs evict it before xrEndFrame.
    std::shared_ptr<const VrStereoFrame> presentation_candidate_;
    std::shared_ptr<const VrStereoFrame> presented_stereo_;
    bool playout_anchored_=false;
    std::uint64_t playout_source_ns_=0,playout_local_ns_=0;
    std::uint64_t last_presented_timestamp_=0;
    bool have_presented_timestamp_=false;
    VrClientStats stats_;
};
int vr_client_state_self_test();
}
