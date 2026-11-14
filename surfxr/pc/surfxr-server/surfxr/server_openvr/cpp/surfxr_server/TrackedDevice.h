#pragma once

#include "bindings.h"
#include "DeviceActivation.h"
#include "BooleanInput.h"
#include "openvr_driver_wrap.h"
#include <map>
#include <optional>

class TrackedDevice : vr::ITrackedDeviceServerDriver {
public:
    vr::TrackedDeviceIndex_t object_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t prop_container = vr::k_ulInvalidPropertyContainer;
    vr::DriverPose_t last_pose;

    bool register_device(bool await_activation);
    void set_prop(FfiOpenvrProperty prop);

protected:
    surfxr::BooleanInput boolean_input_;
    uint64_t device_id;
    vr::ETrackedDeviceClass device_class;

    TrackedDevice(uint64_t device_id, vr::ETrackedDeviceClass device_class);
    std::string get_serial_number();
    void submit_pose(vr::DriverPose_t pose);
    bool is_active() const { return activation_.ready(); }
    virtual bool activate() = 0;
    virtual void* get_component(const char*) = 0;

private:
    DeviceActivation activation_;

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(vr::TrackedDeviceIndex_t object_id) final;
    void Deactivate() final {
        activation_.deactivate();
        this->object_id = vr::k_unTrackedDeviceIndexInvalid;
        this->prop_container = vr::k_ulInvalidPropertyContainer;
    }
    void EnterStandby() final { }
    void* GetComponent(const char* component_name_and_version) final {
        return get_component(component_name_and_version);
    }
    void DebugRequest(const char*, char* buffer, uint32_t buffer_size) final {
        if (buffer_size >= 1)
            buffer[0] = 0;
    }
    vr::DriverPose_t GetPose() final { return this->last_pose; }
};
