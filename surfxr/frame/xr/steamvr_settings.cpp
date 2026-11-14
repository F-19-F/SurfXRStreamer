#include "steamvr_settings.hpp"
#include <array>
#include <cmath>
#include <unistd.h>
#ifdef SURFXR_HAVE_OPENVR_REFRESH
#include <openvr.h>
#endif

namespace surfxr::xr_detail {
void SteamVrSettings::request(RefreshRequest value){
    if(value.serial==request_.serial)return;
    request_=value;attempts_=10;next_={};
}
bool SteamVrSettings::poll(RefreshSnapshot& value){
    const auto now=std::chrono::steady_clock::now();
    if(now<next_)return false;
    next_=now+std::chrono::milliseconds(500);
#ifdef SURFXR_HAVE_OPENVR_REFRESH
    auto* system=vr::VRSystem();auto* apps=vr::VRApplications();auto* settings=vr::VRSettings();
    if(!system||!apps||!settings)return false;
    std::array<char,1024> app{};
    const auto parent=static_cast<std::uint32_t>(getppid());
    snapshot_.app_known=apps->GetApplicationKeyByProcessId(parent,app.data(),app.size())==vr::VRApplicationError_None;
    // Query the parent by PID, not the current scene: another app may take focus.
    if(attempts_){
        --attempts_;snapshot_.requested=request_.hz;snapshot_.result=-2;
        if(snapshot_.app_known||!attempts_){
            snapshot_.serial=request_.serial;
            if(snapshot_.app_known){
                snapshot_.result=0;
                if(request_.hz>0){
                    std::array<float,64> rates{};vr::ETrackedPropertyError error=vr::TrackedProp_Success;
                    const auto bytes=system->GetArrayTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                        vr::Prop_DisplayAvailableFrameRates_Float_Array,vr::k_unFloatPropertyTag,rates.data(),sizeof(rates),&error);
                    float matched=0;
                    if(error==vr::TrackedProp_Success&&bytes<=sizeof(rates)&&bytes%sizeof(float)==0)
                        for(unsigned i=0;i<bytes/sizeof(float);++i)if(std::isfinite(rates[i])&&std::abs(rates[i]-request_.hz)<.01f)matched=rates[i];
                    if(!matched)snapshot_.result=-1;
                    else{
                        // SteamVR's per-application video preference, also read
                        // by its video settings UI. Do not change global defaults.
                        vr::EVRSettingsError error=vr::VRSettingsError_None;
                        settings->SetFloat(app.data(),"preferredRefreshRate",matched,&error);
                        snapshot_.result=static_cast<int>(error);
                    }
                }
            }
            attempts_=0;
        }
    }
    vr::ETrackedPropertyError error=vr::TrackedProp_Success;
    const auto actual=system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_DisplayFrequency_Float,&error);
    snapshot_.actual=error==vr::TrackedProp_Success&&std::isfinite(actual)&&actual>0&&actual<=1000?actual:0;
    value=snapshot_;return true;
#else
    (void)value;return false;
#endif
}
}
