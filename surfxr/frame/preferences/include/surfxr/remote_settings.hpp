#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace surfxr {
// Values use menu units. Wire normalization belongs to the session serializer.
enum class SettingId : std::uint8_t {
    VrFramerate, VrGraphicsQuality, VrBitrateMbps, DesktopFramerate,
    DesktopBitrateMbps, MicrophoneEnabled, MicrophoneVolume, FoveatedStreaming, Reserved, DesktopPointerPriority, Count
};
inline constexpr std::size_t kRemoteSettingCount=static_cast<std::size_t>(SettingId::Count);
// First-run values mirror PC session defaults. Local microphone preferences
// remain client-owned; none of these values are unsolicited server edits.
inline constexpr std::array<double,kRemoteSettingCount> kDefaultRemoteSettings{120,4,100,60,60,0,.85,1,0,0};
inline constexpr unsigned kDefaultVrViewWidth=3072,kDefaultVrViewHeight=3264;
constexpr double default_remote_setting(SettingId id){return kDefaultRemoteSettings[static_cast<std::size_t>(id)];}
struct SettingChange {SettingId id=SettingId::VrFramerate;double value=0;};
struct SettingState {
    double requested=0;
    std::optional<double> actual;
    bool pending=false,supported=false;
};
struct VrQualityProfile {
    int quality;
    const char* name;
    unsigned encoded_width,encoded_height,render_width,render_height;
};
const std::array<VrQualityProfile,9>& surfxr_vr_quality_profiles() noexcept;
const VrQualityProfile* surfxr_vr_quality_profile(int quality) noexcept;
// -2 means that even Potato does not fit. Throughput is not implied by size.
int surfxr_max_vr_quality(unsigned encoded_width,unsigned encoded_height,
                         std::uint64_t encoded_pixels,unsigned render_width,unsigned render_height,
                         double render_scale_x=1,double render_scale_y=1) noexcept;
struct VrObservedDimensions {unsigned encoded_width=0,encoded_height=0,render_width=0,render_height=0;};
// Local client state only; never populated from arbitrary server strings.
enum class VrMenuStatus : std::uint8_t {
    Idle,Starting,StageNotReady,NoSupportedRefreshRate,SteamVrUnavailable,
    ControlQueueFull,StartTimedOut
};
const char* vr_menu_status_text(VrMenuStatus status) noexcept;
struct RemoteSettingsCapabilities {
    bool surfxr_video_controls=false;
    bool surfxr_pointer_controls=false;
    bool server_video_controls=true,vr_only=false,frame_controller_inputs=false;
    bool session_mode_switch=false;
    bool connected=false,has_vr=false,can_launch_vr=false,vr_ready=false,vr_active=false;
    bool vr_requested=false; // Includes startup; drives Enter/Cancel/Back behavior.
    VrMenuStatus vr_status=VrMenuStatus::Idle;
    bool stage_active=false,microphone_available=false;
    bool eye_gaze_available=false;
    bool can_add_monitor=false,can_remove_monitor=false;
    unsigned desktop_monitor_count=0; // Last desktop media configuration; zero = unknown.
    bool monitor_pending=false;
    bool monitor_timed_out=false;
    int active_codec=0;
    // Legal remote video rates, independent of local display refresh modes.
    // A runtime that exposes only 144 Hz can still present a 120 FPS stream.
    std::vector<int> vr_framerates;
    unsigned vr_stream_framerate=0; // Current VR media config; zero until negotiated.
    int max_vr_quality=7; // Caller narrows this with actual decoder/XR size limits.
};
struct RemoteSettingsSnapshot {
    std::array<SettingState,kRemoteSettingCount> values{};
    RemoteSettingsCapabilities capabilities;
    std::optional<VrObservedDimensions> vr_dimensions;
    std::uint64_t revision=0;
    const SettingState& at(SettingId id) const {return values.at(static_cast<std::size_t>(id));}
};
const char* setting_name(SettingId id) noexcept;
bool valid_setting_change(SettingChange change) noexcept;
// SurfXR bitrate limits are absolute Mbps.
double vr_bitrate_max_mbps(int codec) noexcept;
// Remote video rates are independent of local display refresh.
std::vector<int> protocol_vr_framerates();
// Discrete, bounded choices for the menu; an empty vector means unavailable.
std::vector<double> setting_choices(SettingId id,const RemoteSettingsSnapshot& snapshot);
bool setting_request_allowed(SettingChange change,const RemoteSettingsSnapshot& snapshot) noexcept;

// Network, viewer and XR threads share this small snapshot. A successful send is
// NOT an acknowledgement: only observe() with received server evidence updates
// actual. Defaults seed requested values, but never claim they were observed.
class RemoteSettings {
public:
    RemoteSettings();
    void set_capabilities(RemoteSettingsCapabilities capabilities);
    RemoteSettingsSnapshot snapshot() const;
    bool request(SettingChange change);
    void synchronize_surfxr(const std::array<double,3>& values,const std::array<bool,3>& pending);
    void synchronize_pointer(double value,bool pending,bool supported);
    bool observe(SettingChange change);
    // Received dimensions are telemetry; accepted PC settings determine the quality tier.
    bool observe_vr_dimensions(unsigned encoded_width,unsigned encoded_height,
                               unsigned render_width,unsigned render_height,bool foveated=false);
    void seed_requested(SettingChange change); // Initial client profile, before requests.
private:
    mutable std::mutex mutex_;
    RemoteSettingsSnapshot state_;
    std::array<bool,kRemoteSettingCount> requested_{};
    std::array<bool,kRemoteSettingCount> seeded_{};
    void update_support();
};
} // namespace surfxr
