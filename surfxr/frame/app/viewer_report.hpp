#pragma once
#include "viewer_private.hpp"
#include "surfxr/build_features.hpp"

namespace surfxr::viewer {
struct ViewerReport {
    surfxr::SessionBackend backend=surfxr::SessionBackend::Surfxr;
    surfxr::SessionResult receiver;std::array<surfxr::DecoderStats,4> decoders{};std::array<surfxr::DecoderStats,4> vr_decoders{};
    surfxr::VrClientStats vr_stats{};surfxr::SessionControlStats control_stats{};
    surfxr::RemoteSettingsSnapshot final_settings{};
    surfxr::XrDisplayStats xr;
    surfxr::CpuAffinityStats cpu_affinity,render_cpu_affinity;
    std::atomic_bool connection_started{false};
    bool connection_closed=true,worker_completed=false,worker_failed=false,xr_failed=false;
    const char* stage="arguments";std::string worker_error,xr_error,startup_error;
    bool audio_requested=false,microphone_requested=false;
    std::string microphone_report="{}",microphone_error;
    std::string audio_report="{}",audio_error;
    std::string layout_load="disabled",layout_save="skipped";
    std::string settings_load="disabled";unsigned settings_adjusted=0;
    surfxr::SettingsStoreStats settings_store_stats{};surfxr::SettingsPreferences initial_preferences;
    surfxr::ImageQualityStoreStats image_quality_store_stats{};surfxr::ImageQualityPreferences final_image_quality;
    std::string image_quality_load="disabled";
    surfxr::InputPreferencesStoreStats input_preferences_store_stats{};surfxr::InputPreferences final_input_preferences;
    std::string input_preferences_load="disabled";surfxr::BatterySnapshot final_battery;
    surfxr::InputStats input_stats{};bool input_requested=false;
    std::shared_ptr<surfxr::HapticsQueue> haptics=std::make_shared<surfxr::HapticsQueue>();
    std::unique_ptr<surfxr::ViewerStatusWriter> runtime_status;
    std::unique_ptr<surfxr::ViewerInstanceLock> instance_lock;
    int print();
};
}
