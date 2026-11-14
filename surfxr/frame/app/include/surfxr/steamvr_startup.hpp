#pragma once
#include "surfxr/session.hpp"
#include <array>
#include <optional>
#include <string>
namespace surfxr {
inline constexpr std::array<const char*,5> steamvr_startup_states{{"not_started","launching","runtime_ready","completed","failed"}};
inline constexpr std::array<const char*,7> steamvr_startup_reasons{{"none","untrusted","launcher_failed","driver_not_ready","runtime_exited","session_failed","transport_failed"}};
inline const char* steamvr_startup_name(SteamVrStartupState value)noexcept{
    const auto index=static_cast<unsigned>(value);return index<steamvr_startup_states.size()?steamvr_startup_states[index]:"not_started";
}
inline const char* steamvr_startup_reason_name(SteamVrStartupReason value)noexcept{
    const auto index=static_cast<unsigned>(value);return index<steamvr_startup_reasons.size()?steamvr_startup_reasons[index]:"none";
}
inline std::optional<SteamVrStartupState> steamvr_startup_state(const std::string& name){
    for(unsigned i=0;i<steamvr_startup_states.size();++i)if(name==steamvr_startup_states[i])return static_cast<SteamVrStartupState>(i);
    return std::nullopt;
}
inline std::optional<SteamVrStartupReason> steamvr_startup_reason(const std::string& name){
    for(unsigned i=0;i<steamvr_startup_reasons.size();++i)if(name==steamvr_startup_reasons[i])return static_cast<SteamVrStartupReason>(i);
    return std::nullopt;
}
inline const char* steamvr_startup_title(SteamVrStartupState state)noexcept{
    switch(state){
    case SteamVrStartupState::NotStarted:return "Ready to start SteamVR";
    case SteamVrStartupState::Launching:return "Starting PC SteamVR";
    case SteamVrStartupState::RuntimeReady:return "PC SteamVR is ready";
    case SteamVrStartupState::Completed:return "Streaming SteamVR";
    case SteamVrStartupState::Failed:return "SteamVR startup failed";
    }
    return "Ready to start SteamVR";
}
inline const char* steamvr_startup_detail(SteamVrStartupState state,SteamVrStartupReason reason)noexcept{
    if(state==SteamVrStartupState::Failed)switch(reason){
    case SteamVrStartupReason::Untrusted:return "Trust this Frame in the PC SurfXR Dashboard, then select SteamVR to retry.";
    case SteamVrStartupReason::LauncherFailed:return "The PC could not launch SteamVR. Check the PC SurfXR Dashboard and retry.";
    case SteamVrStartupReason::DriverNotReady:return "The PC has not reported an SURFXR driver ready. Check the PC SurfXR Dashboard.";
    case SteamVrStartupReason::RuntimeExited:return "PC SteamVR exited before VR streaming completed. Select SteamVR to retry.";
    case SteamVrStartupReason::SessionFailed:return "The VR session could not complete. Desktop remains available; select SteamVR to retry.";
    case SteamVrStartupReason::TransportFailed:return "The PC startup status connection failed. Check the selected link and retry.";
    case SteamVrStartupReason::None:return "VR startup did not complete. Desktop remains available; select SteamVR to retry.";
    }
    switch(state){
    case SteamVrStartupState::NotStarted:return "Select SteamVR to launch it on your PC.";
    case SteamVrStartupState::Launching:return "Waiting for PC SteamVR and the SURFXR driver to become ready.";
    case SteamVrStartupState::RuntimeReady:return "Waiting for the first VR frame to be presented on Frame.";
    case SteamVrStartupState::Completed:return "The first VR frame has been presented on Frame.";
    case SteamVrStartupState::Failed:break;
    }
    return "VR startup did not complete.";
}
}
