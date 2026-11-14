#pragma once
#include "viewer_report.hpp"

namespace surfxr::viewer {
// Callback state outlives every callback returned to the XR loop.
struct ViewerUi {
    ViewerReport& result;
    ViewerOptions& opts;
    surfxr::XrDisplay& display;
    std::shared_ptr<surfxr::SessionInputQueue>& input;
    std::shared_ptr<surfxr::SessionControlQueue>& control;
    surfxr::VrClientState& vr;
    surfxr::RemoteSettings& settings;
    surfxr::SettingsStore& settings_store;
    surfxr::ImageQualityPreferences& image_quality;
    surfxr::ImageQualityStore& image_quality_store;
    surfxr::InputPreferences& input_preferences;
    surfxr::InputPreferencesStore& input_preferences_store;
    std::atomic_bool& controllers_enabled;
    std::atomic_bool& microphone_enabled;
    surfxr::BatteryMonitor& battery_monitor;
    surfxr::TestHeadMotion& test_head_motion;
    surfxr::ViewerDecodeTelemetrySlot& decode_telemetry;
    surfxr::ViewerModeCommands& mode_commands;
    std::optional<bool>& requested_mode;
    DesktopFrameDemand& desktop_demand;
    std::optional<unsigned> requested_monitors{};
    std::chrono::steady_clock::time_point next_status_sample{};
    std::chrono::steady_clock::time_point fast_status_until{};
    int last_setting_id=-1,last_setting_value=0;bool last_setting_accepted=false;
    std::uint64_t last_setting_serial=0;
    int last_quality_id=-1,last_quality_value=0;bool last_quality_accepted=false;
    std::uint64_t last_quality_serial=0;
    int last_input_id=-1,last_input_value=0;bool last_input_accepted=false;
    std::uint64_t last_input_serial=0;
    bool timing_trace_enabled=false,timing_trace_pending=false;
    bool apply_quality(surfxr::ImageQualityChange);
    bool apply_input_preference(surfxr::InputPreferenceChange);
    surfxr::XrUiCallbacks callbacks();
};
}
