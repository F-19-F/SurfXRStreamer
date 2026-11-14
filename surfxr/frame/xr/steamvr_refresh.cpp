#include "steamvr_refresh.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#ifdef SURFXR_HAVE_OPENVR_REFRESH
#include <openvr.h>
#endif

namespace surfxr::xr_detail {
std::vector<float> steamvr_refresh_rates() {
    std::vector<float> rates;
#ifdef SURFXR_HAVE_OPENVR_REFRESH
    vr::EVRInitError init = vr::VRInitError_None;
    auto* system = vr::VR_Init(&init, vr::VRApplication_Background);
    if (init != vr::VRInitError_None) return rates;
    if (system) {
        std::array<float,64> modes{};
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        const auto bytes = system->GetArrayTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
            vr::Prop_DisplayAvailableFrameRates_Float_Array, vr::k_unFloatPropertyTag,
            modes.data(), sizeof(modes), &error);
        if (error == vr::TrackedProp_Success && bytes <= sizeof(modes) && bytes % sizeof(float) == 0) {
            for (unsigned i=0; i<bytes/sizeof(float); ++i) {
                const auto rate=modes[i];
                if (std::isfinite(rate) && rate>=30 && rate<=240 &&
                    std::find(rates.begin(),rates.end(),rate)==rates.end()) rates.push_back(rate);
            }
        }
        if (rates.empty()) {
            const auto rate=system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                vr::Prop_DisplayFrequency_Float, &error);
            if (error==vr::TrackedProp_Success && std::isfinite(rate) && rate>=30 && rate<=240)
                rates.push_back(rate);
        }
    }
    vr::VR_Shutdown();
#endif
    return rates;
}
}
