#pragma once
#include "surfxr/ports.hpp"
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "surfxr/build_features.hpp"
#include "surfxr/remote_settings.hpp"
#include "surfxr/surfxr_desktop.hpp"
#include "surfxr/keyboard_image.hpp"

namespace surfxr {
namespace transport {class FeedbackQueue;class Modes;class UnifiedTransport;class SettingsSync;}
enum class SessionBackend {Surfxr};
class SessionInputQueue;
class SessionControlQueue;
struct SessionControlCommand;
class HapticsQueue;
struct VrConfiguration;
struct VrVideoMetadata;
struct TrackingFrame;
// Data-upload-owner counters. Only completed tracking sends count. Durations
// use CLOCK_MONOTONIC nanoseconds; no pose values or packet contents are kept.
// Sample age is measured at send start, before neutralization can erase an old
// sample; unavailable/future/zero sample times do not contribute to age samples.
struct TrackingUploadTelemetry {
    // Best-effort tracking-socket policy and readback; -1 means unavailable.
    // Errors cover setting or reading that option, without stopping tracking.
    bool qos_requested=false,qos_applied=false;
    int qos_tos=-1,qos_priority=-1,qos_tos_errno=0,qos_priority_errno=0;
    std::uint64_t sample_monotonic_ns=0,requested_hz=0;
    std::uint64_t packets=0,missed_deadlines=0,fresh_pose_packets=0,repeated_pose_packets=0,invalid_pose_packets=0;
    // SURFXR optical configuration sends and completed remote menu press edges.
    std::uint64_t view_config_packets=0,menu_press_edges=0;
    // Per-hand activity/neutralization only; no button values or poses.
    bool focused=false;
    std::array<bool,2> hand_pose_active{},hand_input_active{},hand_ui_captured{};
    std::array<std::uint64_t,2> fresh_hand_packets{},cached_hand_packets{};
    std::uint64_t interval_samples=0,interval_ns_sum=0,interval_ns_max=0;
    std::uint64_t send_duration_ns_sum=0,send_duration_ns_max=0,encoded_bytes_sum=0,encoded_bytes_max=0;
    std::uint64_t sample_age_samples=0,sample_age_ns_sum=0,sample_age_ns_max=0;
    // Completed, non-overlapping report windows (normally >=1s; last may be
    // partial). A crossing send interval belongs to the window of its end.
    std::uint64_t window_started_ns=0,window_ended_ns=0,window_packets=0;
    std::uint64_t window_interval_ns_max=0,window_send_duration_ns_max=0,window_sample_age_ns_max=0;
    // Software clock calibration; rate is server ns per client ns in parts/billion.
    bool clock_ready=false;
    std::uint64_t clock_samples=0,clock_accepted=0,clock_rejected=0,clock_revision=0;
    std::uint64_t clock_error_ns=0,clock_reply_age_ns=0,clock_min_rtt_ns=0;
    std::int64_t clock_offset_ns=0;
    std::uint64_t clock_rate_ppb=0,clock_rate_low_ppb=0,clock_rate_high_ppb=0;
    std::uint64_t clock_t1=0,clock_t2=0,clock_t3=0,clock_t4=0;
};
struct DesktopStreamConfig {
    std::uint32_t stream_id=0,codec=0,width=0,height=0;
    std::int32_t desktop_x=0,desktop_y=0,desktop_width=0,desktop_height=0;
};
struct DesktopConfiguration {
    std::uint32_t display_frequency=0;
    std::vector<DesktopStreamConfig> streams;
    unsigned monitor_capacity=0;
};
struct SessionOptions {
    SessionBackend backend=SessionBackend::Surfxr;
    unsigned surfxr_control_port=ports::vr_control,surfxr_view_width=kDefaultVrViewWidth,surfxr_view_height=kDefaultVrViewHeight;
    std::string surfxr_version="21.0.0-dev14"; // Explicit wire selection; development protocols are not interchangeable.
    std::vector<float> surfxr_refresh_rates;
    std::shared_ptr<transport::FeedbackQueue> surfxr_feedback;
    std::shared_ptr<transport::Modes> surfxr_modes;
    std::shared_ptr<transport::SettingsSync> surfxr_settings;
    std::shared_ptr<transport::UnifiedTransport> surfxr_unified;
    std::shared_ptr<KeyboardOverlay> keyboard_overlay=std::make_shared<KeyboardOverlay>();
    bool surfxr_media_aes=false; // Explicit override for negotiated unified UDP media; control remains authenticated/encrypted.
    bool surfxr_dashboard_desktop=false;
    unsigned surfxr_dashboard_port=ports::dashboard; // Internal fixture override; no launcher/CLI surface.
    // Historical TCP cases remain testable; no viewer CLI exposes this opt-in.
    bool surfxr_allow_deprecated_tcp=false;
    bool surfxr_legacy_server_connect=false; // Existing server-initiated v21 fixtures; v20 remains legacy.
    bool surfxr_start_prepared=false; // Internal handoff: runtime launch already accepted while desktop stayed live.
    bool surfxr_desktop=false;
    unsigned surfxr_desktop_port=ports::desktop_control;
    std::string surfxr_desktop_key_file;
    std::string host;
    unsigned seconds=8; // 0 means until explicit stop/disconnect; probes keep a bounded default.
    std::shared_ptr<SessionInputQueue> input_queue{};
    std::shared_ptr<SessionControlQueue> control_queue{};
    std::shared_ptr<HapticsQueue> haptics_queue{};
    bool vr_enabled=false;
    bool pose_time_available=false; // native XR sampler present; no persisted setting
    bool eye_gaze_available=false,initial_foveated_streaming=default_remote_setting(SettingId::FoveatedStreaming)!=0;
    bool microphone_enabled=false;
    unsigned initial_vr_framerate=static_cast<unsigned>(default_remote_setting(SettingId::VrFramerate));
    double initial_microphone_volume=.85;
    // Capture availability is separate from the user's saved mute preference.
    bool initial_microphone_active=true;
    std::string local_address; // Empty uses system routing; otherwise all six sockets bind here.
};
enum class SteamVrStartupState:std::uint8_t {NotStarted=0,Launching=1,RuntimeReady=2,Completed=3,Failed=4};
enum class SteamVrStartupReason:std::uint8_t {None=0,Untrusted=1,LauncherFailed=2,DriverNotReady=3,RuntimeExited=4,SessionFailed=5,TransportFailed=6};
struct SteamVrStartupStatus {
    SteamVrStartupState state=SteamVrStartupState::NotStarted;
    SteamVrStartupReason reason=SteamVrStartupReason::None;
    std::uint64_t request_id=0;
};
struct SessionCallbacks {
    // Startup progress is independent of image delivery. Completed is owned
    // by the first real XR VR presentation, never just process/driver ready.
    std::function<void(const SteamVrStartupStatus&)> steamvr_startup;
    std::function<bool(transport::desktop::Event&)> desktop_input;
    std::function<void()> desktop_input_release;
    std::function<unsigned()> desktop_capture_mask;
    std::function<void(bool,unsigned)> desktop_capture_ack;
    std::function<void(std::uint32_t,std::uint32_t,std::uint32_t)> video_config;
    // Bytes are valid only for the callback. Timestamp is the raw server field.
    std::function<void(const std::uint8_t*,std::size_t,std::uint64_t)> primary_video;
    std::function<void()> connection_started;
    // SURFXR attempt boundary, after uploads join; retire old decoded images before retry.
    std::function<void()> connection_ended;
    // Session-owner notification after the complete control record is sent.
    // The command reference is valid only during this nonblocking callback.
    std::function<void(const SessionControlCommand&)> control_command_sent;
    // Optional desktop PCM16LE, 48000 Hz, stereo. One complete packet per call;
    // data is valid only until return. Empty packets have no playback callback.
    // Negotiated SURFXR split streams invoke this on an independent receiver;
    // join that receiver before releasing the callback or its audio owner.
    std::function<void(const std::uint8_t*,std::size_t)> audio_pcm;
    // A complete replacement layout, then independent encoded access units.
    // Existing primary callbacks remain available for the single-screen CLI.
    std::function<void(const DesktopConfiguration&)> desktop_config;
    std::function<void(std::uint32_t,const std::uint8_t*,std::size_t,std::uint64_t)> video_packet;
    std::function<void(const VrConfiguration&)> vr_config;
    std::function<void(std::uint32_t,const std::uint8_t*,std::size_t,std::uint64_t,
                       std::shared_ptr<const VrVideoMetadata>)> vr_video_packet;
    // Optional SURFXR demux path: false rejects a whole access unit without
    // waiting for decode. The session skips dependent packets until a new IDR.
    std::function<bool(std::uint32_t,const std::uint8_t*,std::size_t,std::uint64_t,
                       std::shared_ptr<const VrVideoMetadata>)> try_vr_video_packet;
    // Optional owner transfer: offset selects encoded bytes in a complete
    // retained packet. A false return retains the same IDR recovery semantics.
    std::function<bool(std::uint32_t,std::vector<std::uint8_t>&&,std::size_t,std::uint64_t,
                       std::shared_ptr<const VrVideoMetadata>)> try_vr_video_packet_owned;
    // Check asynchronous media-worker failures/control requests even when no
    // new video packet arrives. Socket/deadline driven, with bounded idle
    // checks; no decode work or waits on this receive-owner callback.
    std::function<void()> video_tick;
    // Source confirmed by a received media configuration: 0 desktop, 1 VR.
    std::function<void(std::uint8_t)> source_changed;
    std::function<void()> show_desktop_requested;
    // Latest local Stage sample, copied under the caller's short lock. These
    // two tracking callbacks run on the independent data-upload thread and may
    // overlap video/settings callbacks. No waits or OpenXR calls here. Old
    // samples expire after250ms.
    std::function<bool(TrackingFrame&)> tracking_frame;
    // Negotiated present-time path. Runs on upload owner and performs a fresh
    // XR query through the display's lifetime/epoch gate, outside socket locks.
    std::function<bool(TrackingFrame&)> sample_tracking;
    std::function<void(bool)> timestamped_tracking;
    // Nonblocking latest gaze copy, only sampled while four-stream VR is active.
    std::function<bool(struct GazeSample&)> gaze_sample;
    // Independent of desktop input and local XR interaction. Copied on the
    // data-upload thread; must not wait or access UI state without synchronization.
    std::function<bool()> vr_controllers_enabled;
    // Called only after the composed state was completely written. Its low31
    // sequence bits are the server's echoed tracking_frame_id (not XrTime).
    std::function<void(const TrackingFrame&)> tracking_sent;
    // Short copied snapshot once per second, plus the final partial window.
    // Runs on the data-upload owner; no IO, pose lookup or network waits here.
    std::function<void(const TrackingUploadTelemetry&)> tracking_telemetry;
    // Bounded local numeric copy at 4 Hz on the receive owner; no media/IO.
    // Optional mono PCM16LE, 48000 Hz capture. All three callbacks run on the
    // session worker. start is called only after the four sockets and bandwidth
    // exchange are ready; false or an exception disables microphone only.
    // read must not wait, returns 0 or exactly 1920 bytes (20 ms), and may throw
    // on capture failure. stop is always called after any start attempt, even
    // if it failed, and must release recording immediately without draining.
    std::function<bool()> microphone_start;
    std::function<std::size_t(std::uint8_t*,std::size_t)> microphone_pcm;
    std::function<void()> microphone_stop;
    std::function<float()> microphone_volume; // Local SURFXR gain; absent uses initial volume.
};
struct SessionResult {
    int exit_code=1;
    bool connection_started=false,connection_closed=true;
    std::uint64_t received_frames=0;
    std::string sanitized_json;
    bool server_stopped=false; // SURFXR explicitly ended the session; no rendered-frame claim.
    bool reconnect_exhausted=false; // A retry budget ended without video recovery, distinct from local cancellation.
};
} // namespace surfxr
