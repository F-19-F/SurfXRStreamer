#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace surfxr {
// PCVR controller forwarding is independent of desktop keyboard/mouse input
// and local headset UI interaction. The transport owns the actual wire gate.
// The timing HUD and gaze marker are viewer-local and default off in legacy files.
enum class InputPreferenceId : std::uint8_t {
    VrControllersEnabled=0,PerformanceOverlay=1,GazeDebugOverlay=2,BoundarySyncEnabled=3,SyncRawBattery=4,
    MiniHudEnabled=5,MiniHudFps=6,MiniHudLatency=7,MiniHudFrameTime=8,MiniHudDecode=9,
    MiniHudGazeOnly=10,MiniHudGazeMarker=11,MiniHudX=12,MiniHudY=13,MiniHudScale=14,TransitionHudEnabled=15
};
struct InputPreferenceChange {InputPreferenceId id=InputPreferenceId::VrControllersEnabled;int value=1;};
// Ordered, unique set-1 scan codes; zero terminates a chord (at most eight keys).
using KeyboardShortcut=std::array<std::uint16_t,8>;
inline const std::vector<KeyboardShortcut> kDefaultInputShortcuts{{0x1d,0x39},{0x1d,0x2f},{0x1d,0x2e},{0x1d,0x2d},{0x1d,0x2c},{0x1d,0x1e}};
inline constexpr unsigned kMaxKeyboardShortcuts=24;
bool valid_keyboard_shortcut(const KeyboardShortcut&) noexcept;
struct InputPreferences {
    bool vr_controllers_enabled=true,performance_overlay=false,gaze_debug_overlay=false,boundary_sync_enabled=true,sync_raw_battery=false;
    bool mini_hud_enabled=false,mini_hud_fps=true,mini_hud_latency=true,mini_hud_frame_time=false,mini_hud_decode=false;
    bool mini_hud_gaze_only=false,mini_hud_gaze_marker=true,transition_hud_enabled=true;
    // Percentages across the view: X left to right, Y top to bottom; scale is relative size.
    int mini_hud_x=80,mini_hud_y=20,mini_hud_scale=100;
    std::vector<KeyboardShortcut> input_shortcuts=kDefaultInputShortcuts;
};
struct InputPreferenceSpec {const char* name;int minimum,maximum;bool boolean;};
inline constexpr std::array<InputPreferenceSpec,16> kInputPreferenceSpecs{{
    {"vr_controllers_enabled",0,1,true},{"performance_overlay",0,1,true},{"gaze_debug_overlay",0,1,true},
    {"boundary_sync_enabled",0,1,true},{"sync_raw_battery",0,1,true},
    {"mini_hud_enabled",0,1,true},{"mini_hud_fps",0,1,true},{"mini_hud_latency",0,1,true},
    {"mini_hud_frame_time",0,1,true},{"mini_hud_decode",0,1,true},{"mini_hud_gaze_only",0,1,true},
    {"mini_hud_gaze_marker",0,1,true},{"mini_hud_x",0,100,false},{"mini_hud_y",0,100,false},{"mini_hud_scale",50,200,false},
    {"transition_hud_enabled",0,1,true}
}};
inline constexpr std::size_t kInputPreferenceCount=kInputPreferenceSpecs.size();
inline constexpr const InputPreferenceSpec* input_preference_spec(InputPreferenceId id) noexcept {
    const auto index=static_cast<std::size_t>(id);
    return index<kInputPreferenceCount?&kInputPreferenceSpecs[index]:nullptr;
}
bool operator==(const InputPreferences& a,const InputPreferences& b) noexcept;
bool operator!=(const InputPreferences& a,const InputPreferences& b) noexcept;
bool valid_input_preference_change(InputPreferenceChange change) noexcept;
int input_preference_value(const InputPreferences& preferences,InputPreferenceId id) noexcept;
bool apply_input_preference_change(InputPreferences& preferences,InputPreferenceChange change) noexcept;
enum class InputPreferencesStoreStatus {Ok,Missing,Invalid,IoError,Disabled};
const char* input_preferences_store_status_name(InputPreferencesStoreStatus status) noexcept;
std::string default_input_preferences_path();
bool parse_input_preferences(const std::string& text,InputPreferences& output) noexcept;
std::string encode_input_preferences(const InputPreferences& preferences);
InputPreferencesStoreStatus load_input_preferences(const std::string& path,InputPreferences& output) noexcept;
struct InputPreferencesStoreStats {
    std::uint64_t submitted=0,coalesced=0,writes=0,failures=0,rejected=0;
    InputPreferencesStoreStatus last_status=InputPreferencesStoreStatus::Ok;
    bool enabled=false,finished=false;
};
// Submit only an accepted preference; persistence never runs on the XR thread.
class InputPreferencesStore {
public:
    explicit InputPreferencesStore(std::string path);
    ~InputPreferencesStore();
    InputPreferencesStore(const InputPreferencesStore&)=delete;
    InputPreferencesStore& operator=(const InputPreferencesStore&)=delete;
    bool submit(const InputPreferences& preferences) noexcept;
    InputPreferencesStoreStats statistics() const;
    InputPreferencesStoreStats finish() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace surfxr
