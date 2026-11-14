void SetTracking(
    unsigned long long targetTimestampNs,
    float controllerPoseTimeOffsetS,
    FfiDeviceMotion headMotion,
    FfiHandData leftHandData,
    FfiHandData rightHandData,
    const FfiDeviceMotion* bodyTrackerMotions,
    int bodyTrackerMotionCount
) {
    if (g_driver_provider.hmd) {
        g_driver_provider.hmd->OnPoseUpdated(targetTimestampNs, headMotion);
    }

    if (g_driver_provider.left_hand_tracker) {
        g_driver_provider.left_hand_tracker->OnPoseUpdate(
            targetTimestampNs, controllerPoseTimeOffsetS, leftHandData
        );
    }

    if (g_driver_provider.left_controller) {
        g_driver_provider.left_controller->OnPoseUpdate(
            targetTimestampNs, controllerPoseTimeOffsetS, leftHandData
        );
    }

    if (g_driver_provider.right_hand_tracker) {
        g_driver_provider.right_hand_tracker->OnPoseUpdate(
            targetTimestampNs, controllerPoseTimeOffsetS, rightHandData
        );
    }

    if (g_driver_provider.right_controller) {
        g_driver_provider.right_controller->OnPoseUpdate(
            targetTimestampNs, controllerPoseTimeOffsetS, rightHandData
        );
    }

    if (Settings_Instance()->m_enableBodyTrackingFakeVive) {
        std::map<uint64_t, FfiDeviceMotion> motionsMap;
        for (int i = 0; i < bodyTrackerMotionCount; i++) {
            auto m = bodyTrackerMotions[i];
            motionsMap.insert({ m.deviceID, m });
        }

        for (auto id : BODY_IDS) {
            auto it = g_driver_provider.tracked_devices.find(id);
            if (it != g_driver_provider.tracked_devices.end()) {
                auto* maybeTracker = (FakeViveTracker*)it->second;
                auto res = motionsMap.find(id);
                auto* maybeMotion = res != motionsMap.end() ? &res->second : nullptr;

                maybeTracker->OnPoseUpdated(targetTimestampNs, maybeMotion);
            }
        }
    }
}

void RequestDriverResync() {
    if (g_driver_provider.hmd) {
        vr::VRServerDriverHost()->VendorSpecificEvent(
            g_driver_provider.hmd->object_id, VendorEvent_SURFXRDriverResync, {}, 0
        );
    }
}

void ShutdownSteamvr() {
    if (g_driver_provider.hmd) {
        vr::VRServerDriverHost()->VendorSpecificEvent(
            g_driver_provider.hmd->object_id, vr::VREvent_DriverRequestedQuit, {}, 0
        );
    }
}

void SetOpenvrProperty(void* instancePtr, FfiOpenvrProperty prop) {
    ((TrackedDevice*)instancePtr)->set_prop(prop);
}

void SetOpenvrPropByDeviceID(unsigned long long deviceID, FfiOpenvrProperty prop) {
    auto device_it = g_driver_provider.tracked_devices.find(deviceID);

    if (device_it != g_driver_provider.tracked_devices.end()) {
        device_it->second->set_prop(prop);
    }
}

void RegisterButton(void* instancePtr, unsigned long long buttonID) {
    // Todo: move RegisterButton to generic TrackedDevice interface
    ((Controller*)instancePtr)->RegisterButton(buttonID);
}

void SetLocalViewParams(const FfiViewParams params[2]) {
    if (g_driver_provider.hmd) {
        g_driver_provider.hmd->SetViewParams(params);
    }
}

void SetBattery(unsigned long long deviceID, float gauge_value, bool is_plugged) {
    if (!std::isfinite(gauge_value) || gauge_value < 0.f || gauge_value > 1.f) return;
    auto device_it = g_driver_provider.tracked_devices.find(deviceID);

    if (device_it != g_driver_provider.tracked_devices.end()) {
        // Enable the UI only after receiving a real sample. Other OpenXR
        // clients without controller batteries keep their existing defaults.
        vr::VRProperties()->SetBoolProperty(
            device_it->second->prop_container, vr::Prop_DeviceProvidesBatteryStatus_Bool, true
        );
        vr::VRProperties()->SetFloatProperty(
            device_it->second->prop_container, vr::Prop_DeviceBatteryPercentage_Float, gauge_value
        );
        vr::VRProperties()->SetBoolProperty(
            device_it->second->prop_container, vr::Prop_DeviceIsCharging_Bool, is_plugged
        );
    }
}

void SetButton(unsigned long long buttonID, FfiButtonValue value) {
    if (g_driver_provider.left_controller && g_driver_provider.left_controller->UsesFrameInputs() &&
        !FrameInput(buttonID,true).steamvr_paths.empty()) {
        g_driver_provider.left_controller->SetButton(buttonID,value);return;
    }
    if (g_driver_provider.right_controller && g_driver_provider.right_controller->UsesFrameInputs() &&
        !FrameInput(buttonID,false).steamvr_paths.empty()) {
        g_driver_provider.right_controller->SetButton(buttonID,value);return;
    }
    if (LEFT_CONTROLLER_BUTTON_MAPPING.find(buttonID) != LEFT_CONTROLLER_BUTTON_MAPPING.end()) {
        if (g_driver_provider.left_controller) {
            g_driver_provider.left_controller->SetButton(buttonID, value);
        }
        if (g_driver_provider.left_hand_tracker) {
            g_driver_provider.left_hand_tracker->SetButton(buttonID, value);
        }
    } else if (RIGHT_CONTROLLER_BUTTON_MAPPING.find(buttonID)
               != RIGHT_CONTROLLER_BUTTON_MAPPING.end()) {
        if (g_driver_provider.right_controller) {
            g_driver_provider.right_controller->SetButton(buttonID, value);
        }
        if (g_driver_provider.right_hand_tracker) {
            g_driver_provider.right_hand_tracker->SetButton(buttonID, value);
        }
    }
}

void SetFrameControllerForwarding(bool enabled, unsigned long long since) {
    if(g_driver_provider.left_controller)g_driver_provider.left_controller->SetFrameForwarding(enabled,since);
    if(g_driver_provider.right_controller)g_driver_provider.right_controller->SetFrameForwarding(enabled,since);
}

void SetFrameSkeleton(unsigned int hand, unsigned long long timestamp, const FfiHandSkeleton* skeleton) {
    if(hand>1)return;
    auto& controller=hand?g_driver_provider.right_controller:g_driver_provider.left_controller;
    if(controller)controller->SetFrameSkeleton(timestamp,skeleton);
}

void SetProximityState(bool headset_is_worn) {
    if (g_driver_provider.hmd) {
        g_driver_provider.hmd->SetProximityState(headset_is_worn);
    }
}

void SetChaperoneArea(float areaWidth, float areaHeight) {
    _SetChaperoneArea(areaWidth, areaHeight);
}
