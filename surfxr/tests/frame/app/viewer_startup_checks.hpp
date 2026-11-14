#pragma once
#include "viewer_affinity_checks.hpp"
#include "surfxr/viewer_status.hpp"
namespace surfxr::tests {
inline void viewer_startup_checks(const std::string& path){
    const auto observe=[&](const std::string& expected){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(read(path).find(expected)==std::string::npos&&std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(read(path).find(expected)!=std::string::npos,"steamvr_startup_status_not_published");
    };
    surfxr::ViewerStatusWriter status(path,false);status.mode_requested(true);
    status.steamvr_startup({surfxr::SteamVrStartupState::Launching,surfxr::SteamVrStartupReason::None,21});
    surfxr::ViewerTelemetry metrics;metrics.vr_submitted_frames=100;status.telemetry(metrics);status.presented(false);
    observe("\"steamvr_startup\":{\"state\":\"launching\",\"reason\":\"none\",\"request_id\":21}");
    status.steamvr_startup({surfxr::SteamVrStartupState::RuntimeReady,surfxr::SteamVrStartupReason::None,21});
    status.telemetry(metrics);observe("\"state\":\"runtime_ready\"");
    require(read(path).find("\"state\":\"completed\"")==std::string::npos,"pc_ready_or_metrics_falsely_completed_vr");
    require(status.presented(true),"first_real_vr_presentation_not_observed");
    observe("\"state\":\"completed\"");
    status.steamvr_startup({surfxr::SteamVrStartupState::RuntimeReady,surfxr::SteamVrStartupReason::None,21});
    status.steamvr_startup({surfxr::SteamVrStartupState::Launching,surfxr::SteamVrStartupReason::None,99});
    status.telemetry(metrics);status.finish(false);require(read(path).find("\"state\":\"completed\"")!=std::string::npos,"late_pc_ready_erased_presentation");
    surfxr::ViewerStatusWriter fallback(path,false);fallback.mode_requested(true);
    fallback.steamvr_startup({surfxr::SteamVrStartupState::Failed,surfxr::SteamVrStartupReason::RuntimeExited,22});
    fallback.mode_requested(false);fallback.presented(false);fallback.telemetry({});fallback.steamvr_startup({});
    observe("\"state\":\"failed\",\"reason\":\"runtime_exited\",\"request_id\":22");
    fallback.mode_requested(true);fallback.steamvr_startup({surfxr::SteamVrStartupState::Launching,surfxr::SteamVrStartupReason::None,23});
    observe("\"state\":\"launching\",\"reason\":\"none\",\"request_id\":23");
    fallback.steamvr_startup({static_cast<surfxr::SteamVrStartupState>(255),surfxr::SteamVrStartupReason::None,24});
    fallback.finish(true);
    const auto final=read(path);
    require(final.find("\"steamvr_startup\":{\"state\":\"failed\",\"reason\":\"session_failed\",\"request_id\":23}")!=std::string::npos&&
            final.size()<=surfxr::viewer_status_max_bytes,"unexpected_viewer_exit_lost_bounded_startup_failure");
}
}
