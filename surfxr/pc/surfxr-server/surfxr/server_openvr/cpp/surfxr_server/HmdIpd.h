#pragma once
#include <openvr_driver.h>
#include <cmath>

// Owned by Hmd's serialized optical update path, shared by VR and desktop.
// Client-side optical filtering owns the noise floor and fine-adjustment rate limit.
class HmdIpd {
    float published_ = 0;
public:
    void reset() { published_ = 0; }

    void update(const vr::HmdMatrix34_t& left, const vr::HmdMatrix34_t& right,
        vr::TrackedDeviceIndex_t device, vr::PropertyContainerHandle_t container,
        vr::CVRPropertyHelpers& properties, vr::IVRSettings& settings,
        vr::IVRServerDriverHost& host) {
        if (device == vr::k_unTrackedDeviceIndexInvalid || !container) return;
        float squared = 0;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float delta = right.m[axis][3] - left.m[axis][3];
            squared += delta * delta;
        }
        const float ipd = std::sqrt(squared);
        // Ignore uninitialized dummy views and invalid optics. Deduplicate
        // resends/FOV-only updates without applying a second 1 mm deadband.
        if (!std::isfinite(ipd) || ipd <= 0 || ipd >= .2f ||
            (published_ > 0 && std::abs(ipd - published_) <= .000001f)) return;
        if (properties.SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, ipd)
            != vr::TrackedProp_Success) return;
        vr::EVRSettingsError error = vr::VRSettingsError_None;
        settings.SetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float, ipd, &error);
        if (error != vr::VRSettingsError_None) return;
        published_ = ipd;
        vr::VREvent_Data_t event{};
        event.ipd.ipdMeters = ipd;
        host.VendorSpecificEvent(device, vr::VREvent_IpdChanged, event, 0);
    }
};
