#include "../surfxr_server/HmdIpd.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

struct Properties : vr::IVRProperties {
    unsigned writes = 0;
    float value = .063f;
    bool fail = false;
    vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t, vr::PropertyRead_t*, uint32_t) override { return vr::TrackedProp_Success; }
    vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t container, vr::PropertyWrite_t* batch, uint32_t count) override {
        assert(container == 42 && count == 1);
        assert(batch->prop == vr::Prop_UserIpdMeters_Float && batch->unTag == vr::k_unFloatPropertyTag);
        assert(batch->unBufferSize == sizeof(float));
        ++writes;
        batch->eError = fail ? vr::TrackedProp_InvalidOperation : vr::TrackedProp_Success;
        if (!fail) std::memcpy(&value, batch->pvBuffer, sizeof(value));
        return batch->eError;
    }
    const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override { return "test"; }
    vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t) override { return 42; }
};

struct Settings : vr::IVRSettings {
    unsigned writes = 0;
    float value = .063f;
    bool fail = false;
    const char* GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override { return "test"; }
    void SetFloat(const char* section, const char* key, float v, vr::EVRSettingsError* error) override {
        assert(std::strcmp(section, vr::k_pch_SteamVR_Section) == 0);
        assert(std::strcmp(key, vr::k_pch_SteamVR_IPD_Float) == 0);
        ++writes;
        *error = fail ? vr::VRSettingsError_WriteFailed : vr::VRSettingsError_None;
        if (!fail) value = v;
    }
    void SetBool(const char*, const char*, bool, vr::EVRSettingsError*) override {}
    void SetInt32(const char*, const char*, int32_t, vr::EVRSettingsError*) override {}
    void SetString(const char*, const char*, const char*, vr::EVRSettingsError*) override {}
    bool GetBool(const char*, const char*, vr::EVRSettingsError*) override { return false; }
    int32_t GetInt32(const char*, const char*, vr::EVRSettingsError*) override { return 0; }
    float GetFloat(const char*, const char*, vr::EVRSettingsError*) override { return value; }
    void GetString(const char*, const char*, char*, uint32_t, vr::EVRSettingsError*) override {}
    void RemoveSection(const char*, vr::EVRSettingsError*) override {}
    void RemoveKeyInSection(const char*, const char*, vr::EVRSettingsError*) override {}
};

struct Host : vr::IVRServerDriverHost {
    Properties& properties;
    Settings& settings;
    unsigned events = 0;
    float value = 0;
    Host(Properties& p, Settings& s) : properties(p), settings(s) {}
    void VendorSpecificEvent(uint32_t device, vr::EVREventType type, const vr::VREvent_Data_t& event, double offset) override {
        assert(device == 0 && type == vr::VREvent_IpdChanged && offset == 0);
        value = event.ipd.ipdMeters;
        // Readers awakened by the event must see the new value in both APIs.
        assert(value == properties.value && value == settings.value);
        ++events;
    }
    bool TrackedDeviceAdded(const char*, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver*) override { return true; }
    void TrackedDevicePoseUpdated(uint32_t, const vr::DriverPose_t&, uint32_t) override {}
    void VsyncEvent(double) override {}
    bool IsExiting() override { return false; }
    bool PollNextEvent(vr::VREvent_t*, uint32_t) override { return false; }
    void GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t*, uint32_t) override {}
    void RequestRestart(const char*, const char*, const char*, const char*) override {}
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming*, uint32_t) override { return 0; }
    void SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t&, const vr::HmdMatrix34_t&) override { assert(false); }
    void SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t&, const vr::HmdRect2_t&) override { assert(false); }
    void SetRecommendedRenderTargetSize(uint32_t, uint32_t, uint32_t) override { assert(false); }
};

int main() {
    Properties properties;
    vr::CVRPropertyHelpers helpers(&properties);
    Settings settings;
    Host host(properties, settings);
    HmdIpd ipd;
    vr::HmdMatrix34_t left{}, right{};
    const auto update = [&] { ipd.update(left, right, 0, 42, helpers, settings, host); };
    update(); assert(host.events == 0 && properties.writes == 0); // dummy views
    left.m[0][3] = -.0346f; right.m[0][3] = .0346f;
    update(); assert(host.events == 1 && std::abs(host.value - .0692f) < 1e-6f);
    for (unsigned i = 0; i < 10000; ++i) {
        right.m[0][3] = .0346f + (i % 2 ? .0000001f : 0);
        update();
    }
    assert(host.events == 1 && properties.writes == 1 && settings.writes == 1);
    // Do not double-filter an accepted FOV/IPD packet at the driver boundary.
    right.m[0][3] = .0351f; update(); assert(host.events == 2);
    const auto published = host.value;
    for (float bad : {0.f, .2f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        left = {}; right = {}; right.m[0][3] = bad; update();
    }
    assert(host.events == 2 && host.value == published && properties.writes == 2);
    // All three translation axes contribute, not just the X-axis difference.
    left = {}; right = {}; right.m[1][3] = .04f; right.m[2][3] = .03f;
    update(); assert(host.events == 3 && std::abs(host.value - .05f) < 1e-6f);
    ipd.reset(); update(); assert(host.events == 4); // fresh activation
    right.m[1][3] = .05f;
    properties.fail = true; update(); assert(host.events == 4);
    properties.fail = false; settings.fail = true; update(); assert(host.events == 4);
    settings.fail = false; update(); assert(host.events == 5); // failed writes remain retryable
    ipd.reset();
    ipd.update(left, right, vr::k_unTrackedDeviceIndexInvalid, 42, helpers, settings, host);
    ipd.update(left, right, 0, 0, helpers, settings, host);
    assert(host.events == 5);
    std::cout << "IPD property/settings/event consistency, jitter, invalid views and retry checks passed\n";
}
