use super::*;

#[unsafe(no_mangle)]
pub extern "C" fn set_device_openvr_props(instance_ptr: *mut c_void, device_id: u64) {
    #[expect(clippy::enum_glob_use)]
    use OpenvrPropKey::*;

    let settings = surfxr_server_core::settings();

    let set_prop = |key, value: &str| {
        set_openvr_prop(
            Some(instance_ptr),
            device_id,
            OpenvrProperty {
                key,
                value: value.into(),
            },
        );
    };

    let set_icons = |base_path: &str| super::device_icons::set_generic_icons(base_path, &set_prop);

    let device_serial = &serial_number(device_id);
    let headset_serial = &serial_number(*HEAD_ID);

    if device_id == *HEAD_ID {
        // Capabilities of the linux direct driver, declared at activation.
        // The win32 driver never sends vsync events and keeps SteamVR's
        // hardcoded vsync-offset model, so these stay linux-only.
        #[cfg(target_os = "linux")]
        {
            // SteamVR uses the timing of VsyncEvent calls instead of its
            // hardcoded vsync-offset model.
            set_prop(DriverDirectModeSendsVsyncEventsBool, "true");
            // Enables the per-app throttling UI and the Throttling_t hints
            // the pacing grid honors.
            set_prop(HmdSupportsAppThrottlingBool, "true");
            set_prop(SupportsXrTextureSetsBool, "true");
        }

        // Closure for all the common Quest headset properties
        let set_oculus_common_headset_props = || {
            set_prop(
                RegisteredDeviceTypeString,
                format!("oculus/{headset_serial}").as_str(),
            );
            set_icons("{oculus}/icons/quest_headset");
        };

        // Per-device props
        match &settings.headset.emulation_mode {
            HeadsetEmulationMode::SteamFrame => {
                set_prop(TrackingSystemNameString, "frame_hmd");
                set_prop(ModelNumberString, "Steam Frame");
                set_prop(ManufacturerNameString, "Valve");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(RegisteredDeviceTypeString, format!("surfxr/steam_frame/{headset_serial}").as_str());
                set_prop(ControllerTypeString, "frame_hmd");
                super::frame_headset::set_icons(&set_prop);
            }
            HeadsetEmulationMode::RiftS => {
                set_prop(TrackingSystemNameString, "oculus");
                set_prop(ModelNumberString, "Oculus Rift S");
                set_prop(ManufacturerNameString, "Oculus");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(DriverVersionString, "1.42.0");
                set_icons("{oculus}/icons/rifts_headset");
            }
            HeadsetEmulationMode::Quest1 => {
                set_prop(TrackingSystemNameString, "oculus");
                set_prop(ModelNumberString, "Oculus Quest");
                set_prop(ManufacturerNameString, "Oculus");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(DriverVersionString, "1.111.0");
                set_oculus_common_headset_props();
            }
            HeadsetEmulationMode::Quest2 => {
                set_prop(TrackingSystemNameString, "oculus");
                set_prop(ModelNumberString, "Miramar");
                set_prop(ManufacturerNameString, "Oculus");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(DriverVersionString, "1.55.0");
                set_oculus_common_headset_props();
            }
            HeadsetEmulationMode::QuestPro => {
                set_prop(TrackingSystemNameString, "oculus");
                set_prop(ModelNumberString, "Meta Quest Pro");
                set_prop(ManufacturerNameString, "Oculus");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(DriverVersionString, "1.55.0");
                set_oculus_common_headset_props();
            }
            HeadsetEmulationMode::Pico4 => {
                set_prop(TrackingSystemNameString, "vrlink");
                set_prop(ModelNumberString, "PICO 4");
                set_prop(ManufacturerNameString, "ByteDance");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(RegisteredDeviceTypeString, "pico");
                set_prop(DriverVersionString, "");
                set_icons("{vrlink}/icons/headset_pico4");
            }
            HeadsetEmulationMode::Vive => {
                set_prop(TrackingSystemNameString, "Vive Tracker");
                set_prop(ModelNumberString, "SURFXR driver server");
                set_prop(ManufacturerNameString, "HTC");
                set_prop(RenderModelNameString, "generic_hmd");
                set_prop(RegisteredDeviceTypeString, "vive");
                set_prop(DriverVersionString, "");
                set_icons("{htc}/icons/vive_headset");
            }
            HeadsetEmulationMode::Custom { .. } => (),
        }

        set_prop(UserIpdMetersFloat, "0.063");
        set_prop(UserHeadToEyeDepthMetersFloat, "0.0");
        set_prop(SecondsFromVsyncToPhotonsFloat, "0.0");

        // return a constant that's not 0 (invalid) or 1 (reserved for Oculus)
        set_prop(CurrentUniverseIdUint64, "2");

        if cfg!(windows) {
            // avoid "not fullscreen" warnings from vrmonitor
            set_prop(IsOnDesktopBool, "false");

            // We let SteamVR handle VSyncs. We just wait in PostPresent().
            set_prop(DriverDirectModeSendsVsyncEventsBool, "false");
        }
        set_prop(DeviceProvidesBatteryStatusBool, "true");
        set_prop(ContainsProximitySensorBool, "true");

        for prop in &settings.headset.extra_openvr_props {
            set_prop(prop.key, &prop.value);
        }
    } else if device_id == *HAND_LEFT_ID
        || device_id == *HAND_RIGHT_ID
        || device_id == *HAND_TRACKER_LEFT_ID
        || device_id == *HAND_TRACKER_RIGHT_ID
    {
        let left_hand = device_id == *HAND_LEFT_ID || device_id == *HAND_TRACKER_LEFT_ID;
        let right_hand = device_id == *HAND_RIGHT_ID || device_id == *HAND_TRACKER_RIGHT_ID;
        let full_skeletal_hand =
            device_id == *HAND_TRACKER_LEFT_ID || device_id == *HAND_TRACKER_RIGHT_ID;
        if let Switch::Enabled(config) = &settings.headset.controllers {
            // Closure for all the common Oculus/Meta controller properties
            let set_oculus_common_props = || {
                set_prop(TrackingSystemNameString, "oculus");

                set_prop(ControllerTypeString, "oculus_touch");
                set_prop(InputProfilePathString, "{oculus}/input/touch_profile.json");
                if left_hand {
                    set_prop(
                        RegisteredDeviceTypeString,
                        format!("oculus/{headset_serial}_Controller_Left").as_str(),
                    );
                    set_icons("{oculus}/icons/rifts_left_controller");
                } else if right_hand {
                    set_prop(
                        RegisteredDeviceTypeString,
                        format!("oculus/{headset_serial}_Controller_Right").as_str(),
                    );
                    set_icons("{oculus}/icons/rifts_right_controller");
                }
            };

            // Controller-specific properties, not shared
            match config.emulation_mode {
                ControllersEmulationMode::SteamFrame => {
                    frame_controller::set_properties(left_hand, &set_prop);
                    set_prop(TrackingSystemNameString, "frame_controller");
                    set_prop(ManufacturerNameString, "Valve");
                    set_prop(ModelNumberString, if left_hand { "Steam Frame (Left Controller)" } else { "Steam Frame (Right Controller)" });
                    set_prop(RenderModelNameString, if left_hand { "{frame_controller}frame_controller_left" } else { "{frame_controller}frame_controller_right" });
                    set_prop(ControllerTypeString, "frame_controller");
                    set_prop(InputProfilePathString, "{frame_controller}/input/frame_controller_profile.json");
                    set_prop(RegisteredDeviceTypeString, if left_hand { "surfxr/steam_frame/controller_left" } else { "surfxr/steam_frame/controller_right" });
                }
                ControllersEmulationMode::RiftSTouch => {
                    set_prop(ManufacturerNameString, "Oculus");
                    if left_hand {
                        set_prop(ModelNumberString, "Oculus Rift S (Left Controller)");
                        set_prop(RenderModelNameString, "oculus_rifts_controller_left");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Oculus Rift S (Right Controller)");
                        set_prop(RenderModelNameString, "oculus_rifts_controller_right");
                    }
                    set_prop(ControllerTypeString, "oculus_touch");
                    set_prop(InputProfilePathString, "{oculus}/input/touch_profile.json");
                    set_oculus_common_props();
                }
                ControllersEmulationMode::Quest1Touch => {
                    set_prop(ManufacturerNameString, "Oculus");
                    if left_hand {
                        set_prop(ModelNumberString, "Oculus Quest (Left Controller)");
                        set_prop(RenderModelNameString, "oculus_quest_controller_left");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Oculus Quest (Right Controller)");
                        set_prop(RenderModelNameString, "oculus_quest_controller_right");
                    }
                    set_prop(ControllerTypeString, "oculus_touch");
                    set_prop(InputProfilePathString, "{oculus}/input/touch_profile.json");
                    set_oculus_common_props();
                }
                ControllersEmulationMode::Quest2Touch => {
                    set_prop(ManufacturerNameString, "Oculus");
                    if left_hand {
                        set_prop(ModelNumberString, "Miramar (Left Controller)");
                        set_prop(RenderModelNameString, "oculus_quest2_controller_left");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Miramar (Right Controller)");
                        set_prop(RenderModelNameString, "oculus_quest2_controller_right");
                    }
                    set_prop(ControllerTypeString, "oculus_touch");
                    set_prop(InputProfilePathString, "{oculus}/input/touch_profile.json");
                    set_oculus_common_props();
                }
                ControllersEmulationMode::Quest3Plus => {
                    set_prop(ManufacturerNameString, "Meta");

                    if left_hand {
                        set_prop(ModelNumberString, "Meta Quest 3 (Left Controller)");
                        set_prop(RenderModelNameString, "oculus_quest_plus_controller_left");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Meta Quest 3 (Right Controller)");
                        set_prop(RenderModelNameString, "oculus_quest_plus_controller_right");
                    }
                    set_prop(ControllerTypeString, "oculus_touch");
                    set_prop(InputProfilePathString, "{oculus}/input/touch_profile.json");
                    set_oculus_common_props();
                }
                ControllersEmulationMode::QuestPro => {
                    set_prop(ManufacturerNameString, "Meta");

                    if left_hand {
                        set_prop(ModelNumberString, "Meta Quest Pro (Left Controller)");
                        set_prop(RenderModelNameString, "oculus_quest_pro_controller_left");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Meta Quest Pro (Right Controller)");
                        set_prop(RenderModelNameString, "oculus_quest_pro_controller_right");
                    }
                    set_oculus_common_props();
                }
                ControllersEmulationMode::Pico4 => {
                    set_prop(TrackingSystemNameString, "vrlink");
                    set_prop(ManufacturerNameString, "ByteDance");
                    if left_hand {
                        set_prop(ModelNumberString, "PICO 4 (Left Controller)");
                        set_prop(
                            RenderModelNameString,
                            "{vrlink}/rendermodels/pico_4_controller_left",
                        );
                        set_icons("{vrlink}/icons/left_pico4");
                    } else if right_hand {
                        set_prop(ModelNumberString, "PICO 4 (Right Controller)");
                        set_prop(
                            RenderModelNameString,
                            "{vrlink}/rendermodels/pico_4_controller_right",
                        );
                        set_icons("{vrlink}/icons/right_pico4");
                    }
                    set_prop(ControllerTypeString, "pico_controller");
                    set_prop(
                        InputProfilePathString,
                        "{vrlink}/input/pico_controller_profile.json",
                    );
                }
                ControllersEmulationMode::PSVR2Sense => {
                    set_prop(TrackingSystemNameString, "playstation_vr2");
                    if left_hand {
                        set_prop(ModelNumberString, "PlayStation VR2 Sense Left");
                        set_prop(SerialNumberString, "playstation_vr2_sense_controller_left");
                        set_prop(
                            RenderModelNameString,
                            "{surfxr_server}/rendermodels/playstation_vr2_sense_left",
                        );
                        set_icons("{surfxr_server}/icons/left_controller_status");
                    } else if right_hand {
                        set_prop(ModelNumberString, "PlayStation VR2 Sense Right");
                        set_prop(SerialNumberString, "playstation_vr2_sense_controller_right");
                        set_prop(
                            RenderModelNameString,
                            "{surfxr_server}/rendermodels/playstation_vr2_sense_right",
                        );
                        set_icons("{surfxr_server}/icons/right_controller_status");
                    }
                    set_prop(TrackingFirmwareVersionString, "0303");
                    set_prop(HardwareRevisionString, "MP");
                    set_prop(ControllerTypeString, "playstation_vr2_sense");
                    set_prop(
                        InputProfilePathString,
                        "{surfxr_server}/input/playstation_vr2_sense_controller_profile.json",
                    );
                }
                ControllersEmulationMode::ValveIndex => {
                    set_prop(TrackingSystemNameString, "indexcontroller");
                    set_prop(ManufacturerNameString, "Valve");
                    if left_hand {
                        set_prop(ModelNumberString, "Knuckles (Left Controller)");
                        set_prop(
                            RenderModelNameString,
                            "{indexcontroller}valve_controller_knu_1_0_left",
                        );
                        set_prop(
                            RegisteredDeviceTypeString,
                            "valve/index_controllerLHR-E217CD00_Left",
                        );
                        set_icons("{indexcontroller}/icons/left_controller_status");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Knuckles (Right Controller)");
                        set_prop(
                            RenderModelNameString,
                            "{indexcontroller}valve_controller_knu_1_0_right",
                        );
                        set_prop(
                            RegisteredDeviceTypeString,
                            "valve/index_controllerLHR-E217CD00_Right",
                        );
                        set_icons("{indexcontroller}/icons/right_controller_status");
                    }
                    set_prop(ControllerTypeString, "knuckles");
                    set_prop(
                        InputProfilePathString,
                        "{indexcontroller}/input/index_controller_profile.json",
                    );
                }
                ControllersEmulationMode::ViveWand => {
                    set_prop(TrackingSystemNameString, "htc");
                    set_prop(ManufacturerNameString, "HTC");
                    set_prop(RenderModelNameString, "vr_controller_vive_1_5");
                    if left_hand {
                        set_prop(
                            ModelNumberString,
                            "SURFXR Remote Controller (Left Controller)",
                        );
                        set_prop(RegisteredDeviceTypeString, "htc/vive_controller_Left");
                    } else if right_hand {
                        set_prop(
                            ModelNumberString,
                            "SURFXR Remote Controller (Right Controller)",
                        );
                        set_prop(RegisteredDeviceTypeString, "htc/vive_controller_Right");
                    }
                    set_prop(ControllerTypeString, "vive_controller");
                    set_prop(InputProfilePathString, "{oculus}/input/touch_profile.json");
                    set_icons("{htc}/icons/controller");
                }
                ControllersEmulationMode::ViveTracker => {
                    set_prop(TrackingSystemNameString, "lighthouse");
                    set_prop(RenderModelNameString, "{htc}vr_tracker_vive_1_0");
                    if left_hand {
                        set_prop(ModelNumberString, "Vive Tracker Pro MV (Left Controller)");
                        set_prop(RegisteredDeviceTypeString, "SURFXR/tracker/left_foot");
                        set_prop(ControllerTypeString, "vive_tracker_left_foot");
                    } else if right_hand {
                        set_prop(ModelNumberString, "Vive Tracker Pro MV (Right Controller)");
                        set_prop(RegisteredDeviceTypeString, "SURFXR/tracker/right_foot");
                        set_prop(ControllerTypeString, "vive_tracker_right_foot");
                    }
                    set_prop(
                        InputProfilePathString,
                        "{htc}/input/vive_tracker_profile.json",
                    );
                    set_icons("{htc}/icons/tracker");

                    // All of these property values were dumped from real a vive tracker via
                    // https://github.com/SDraw/openvr_dumper and were copied from
                    // https://github.com/SDraw/driver_kinectV2
                    set_prop(ResourceRootString, "htc");
                    set_prop(WillDriftInYawBool, "false");
                    set_prop(
                        TrackingFirmwareVersionString,
                        "1541800000 RUNNER-WATCHMAN$runner-watchman@runner-watchman 2018-01-01 FPGA 512(2.56/0/0) BL 0 VRC 1541800000 Radio 1518800000",
                    );
                    set_prop(
                        HardwareRevisionString,
                        "product 128 rev 2.5.6 lot 2000/0/0 0",
                    );
                    set_prop(ConnectedWirelessDongleString, "D0000BE000");
                    set_prop(DeviceIsWirelessBool, "true");
                    set_prop(DeviceIsChargingBool, "false");
                    set_prop(ControllerHandSelectionPriorityInt32, "-1");
                    // vr::HmdMatrix34_t l_transform = {
                    //     {{-1.f, 0.f, 0.f, 0.f}, {0.f, 0.f, -1.f, 0.f}, {0.f, -1.f, 0.f, 0.f}}};
                    // vr_properties->SetProperty(this->prop_container,
                    //                            vr::Prop_StatusDisplayTransform_Matrix34,
                    //                            &l_transform,
                    //                            sizeof(vr::HmdMatrix34_t),
                    //                            vr::k_unHmdMatrix34PropertyTag);
                    set_prop(FirmwareUpdateAvailableBool, "false");
                    set_prop(FirmwareManualUpdateBool, "false");
                    set_prop(
                        FirmwareManualUpdateURLString,
                        "https://developer.valvesoftware.com/wiki/SteamVR/HowTo_Update_Firmware",
                    );
                    set_prop(HardwareRevisionUint64, "2214720000");
                    set_prop(FirmwareVersionUint64, "1541800000");
                    set_prop(FPGAVersionUint64, "512");
                    set_prop(VRCVersionUint64, "1514800000");
                    set_prop(RadioVersionUint64, "1518800000");
                    set_prop(DongleVersionUint64, "8933539758");
                    set_prop(DeviceCanPowerOffBool, "true");
                    // vr_properties->SetStringProperty(this->prop_container,
                    //                                  vr::Prop_Firmware_ProgrammingTargetString,
                    //                                  GetSerialNumber().c_str());
                    set_prop(FirmwareForceUpdateRequiredBool, device_serial);
                    set_prop(FirmwareRemindUpdateBool, "false");
                    set_prop(HasDisplayComponentBool, "false");
                    set_prop(HasCameraComponentBool, "false");
                    set_prop(HasDriverDirectModeComponentBool, "false");
                    set_prop(HasVirtualDisplayComponentBool, "false");
                }
                ControllersEmulationMode::Custom { .. } => {}
            }

            set_prop(SerialNumberString, device_serial);
            set_prop(AttachedDeviceIdString, device_serial);

            if full_skeletal_hand {
                set_prop(TrackingSystemNameString, "vrlink");
                set_prop(ManufacturerNameString, "VRLink");

                set_prop(RenderModelNameString, "{vrlink}/rendermodels/shuttlecock");
                set_prop(ControllerTypeString, "svl_hand_interaction_augmented");
                set_prop(
                    InputProfilePathString,
                    "{vrlink}/input/svl_hand_interaction_augmented_input_profile.json",
                );

                if left_hand {
                    set_prop(ModelNumberString, "VRLink Hand Tracker (Left Hand)");
                    set_prop(
                        RegisteredDeviceTypeString,
                        "vrlink/VRLINKQ_HandTracker_Left",
                    );
                    set_prop(SerialNumberString, "VRLINKQ_Hand_Left");
                    set_prop(AttachedDeviceIdString, "VRLINKQ_Hand_Left");
                    set_icons("{vrlink}/icons/left_handtracking");
                } else if right_hand {
                    set_prop(ModelNumberString, "VRLink Hand Tracker (Right Hand)");
                    set_prop(
                        RegisteredDeviceTypeString,
                        "vrlink/VRLINKQ_HandTracker_Right",
                    );
                    set_prop(SerialNumberString, "VRLINKQ_Hand_Right");
                    set_prop(AttachedDeviceIdString, "VRLINKQ_Hand_Right");
                    set_icons("{vrlink}/icons/right_handtracking");
                }
            }

            set_prop(
                SupportedButtonsUint64,
                0xFFFFFFFFFFFFFFFF_u64.to_string().as_str(),
            );

            // OpenXR does not support controller battery
            set_prop(DeviceProvidesBatteryStatusBool, "false");

            // k_eControllerAxis_Joystick = 2
            set_prop(Axis0TypeInt32, "2");

            if matches!(config.emulation_mode, ControllersEmulationMode::ViveTracker) {
                // TrackedControllerRole_Invalid
                set_prop(ControllerRoleHintInt32, "0");
            } else if left_hand {
                // TrackedControllerRole_LeftHand
                set_prop(ControllerRoleHintInt32, "1");
            } else if right_hand {
                // TrackedControllerRole_RightHand
                set_prop(ControllerRoleHintInt32, "2");
            }

            for prop in &config.extra_openvr_props {
                set_prop(prop.key, &prop.value);
            }
        }
    }
}
