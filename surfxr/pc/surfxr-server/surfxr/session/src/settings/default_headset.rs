use super::*;

pub(super) fn make() -> HeadsetConfigDefault {
    let default_custom_openvr_props = VectorDefault {
        gui_collapsed: true,
        element: OpenvrPropertyDefault {
            key: OpenvrPropKeyDefault {
                variant: OpenvrPropKeyDefaultVariant::TrackingSystemNameString,
            },
            value: "".into(),
        },
        content: vec![],
    };
HeadsetConfigDefault {
            emulation_mode: HeadsetEmulationModeDefault {
                Custom: HeadsetEmulationModeCustomDefault {
                    serial_number: "Unknown".into(),
                },
                variant: HeadsetEmulationModeDefaultVariant::SteamFrame,
            },
            performance_level: PerformanceLevelConfigDefault {
                cpu: SwitchDefault {
                    enabled: false,
                    content: PerformanceLevelDefault {
                        variant: PerformanceLevelDefaultVariant::PowerSavings,
                    },
                },
                gpu: SwitchDefault {
                    enabled: false,
                    content: PerformanceLevelDefault {
                        variant: PerformanceLevelDefaultVariant::PowerSavings,
                    },
                },
            },
            extra_openvr_props: default_custom_openvr_props.clone(),
            tracking_ref_only: false,
            enable_vive_tracker_proxy: false,
            face_tracking: SwitchDefault {
                enabled: false,
                content: FaceTrackingConfigDefault {
                    gui_collapsed: true,
                    sources: FaceTrackingSourcesConfigDefault {
                        variant: FaceTrackingSourcesConfigDefaultVariant::PreferFullFaceTracking,
                    },
                    eye_tracking_osc_override: SwitchDefault {
                        enabled: false,
                        content: OscPortDefault { port: surfxr_common::ports::GAZE_OSC },
                    },
                    sink: FaceTrackingSinkConfigDefault {
                        social_presence: SwitchDefault {
                            enabled: true,
                            content: FaceTrackingSocialPresenceSinkConfigDefault {
                                VrchatEyeOsc:
                                    FaceTrackingSocialPresenceSinkConfigVrchatEyeOscDefault {
                                        port: 9000,
                                    },
                                variant:
                                    FaceTrackingSocialPresenceSinkConfigDefaultVariant::VrchatEyeOsc,
                            },
                        },
                        eye_tracked_foveated_encoding: false,
                    },
                },
            },
            multimodal_tracking: SwitchDefault {
                enabled: false,
                content: MultimodalTrackingDefault {
                    enabled: true,
                    detached_controllers_steamvr_sink: false,
                },
            },
            body_tracking: SwitchDefault {
                enabled: false,
                content: BodyTrackingConfigDefault {
                    gui_collapsed: true,
                    sources: BodyTrackingSourcesConfigDefault {
                        meta: BodyTrackingMetaConfigDefault {
                            prefer_full_body: true,
                            prefer_high_fidelity: true,
                        },
                        bd: BodyTrackingBDConfigDefault {
                            BodyTracking: BodyTrackingBDConfigBodyTrackingDefault {
                                high_accuracy: true,
                                prompt_calibration_on_start: true,
                            },
                            variant: BodyTrackingBDConfigDefaultVariant::BodyTracking,
                        },
                    },
                    sink: BodyTrackingSinkConfigDefault {
                        VrchatBodyOsc: BodyTrackingSinkConfigVrchatBodyOscDefault { port: 9000 },
                        variant: BodyTrackingSinkConfigDefaultVariant::FakeViveTracker,
                    },
                    tracked: true,
                },
            },
            vmc: SwitchDefault {
                enabled: false,
                content: VMCConfigDefault {
                    gui_collapsed: true,
                    host: "127.0.0.1".into(),
                    port: 39539,
                    publish: true,
                    orientation_correction: true,
                },
            },
            controllers: SwitchDefault {
                enabled: true,
                content: ControllersConfigDefault {
                    gui_collapsed: false,
                    tracked: true,
                    hand_skeleton: SwitchDefault {
                        enabled: true,
                        content: HandSkeletonConfigDefault {
                            steamvr_input_2_0: true,
                            predict: false,
                        },
                    },
                    emulation_mode: ControllersEmulationModeDefault {
                        Custom: ControllersEmulationModeCustomDefault {
                            serial_number: "SURFXR Controller".into(),
                            button_set: VectorDefault {
                                gui_collapsed: false,
                                element: "/user/hand/left/input/a/click".into(),
                                content: vec![],
                            },
                        },
                        variant: ControllersEmulationModeDefaultVariant::SteamFrame,
                    },
                    extra_openvr_props: default_custom_openvr_props,
                    button_mappings: OptionalDefault {
                        set: false,
                        content: DictionaryDefault {
                            gui_collapsed: false,
                            key: "/user/hand/left/input/a/click".into(),
                            value: VectorDefault {
                                gui_collapsed: false,
                                element: ButtonBindingTargetDefault {
                                    destination: "/user/hand/left/input/a/click".into(),
                                    mapping_type: ButtonMappingTypeDefault {
                                        HysteresisThreshold: HysteresisThresholdDefault {
                                            value: 0.5,
                                            deviation: 0.05,
                                        },
                                        BinaryToScalar: BinaryToScalarStatesDefault {
                                            off: 0.0,
                                            on: 1.0,
                                        },
                                        Remap: RangeDefault { min: 0.0, max: 1.0 },
                                        variant: ButtonMappingTypeDefaultVariant::Passthrough,
                                    },
                                    binary_conditions: VectorDefault {
                                        gui_collapsed: true,
                                        element: "/user/hand/left/input/trigger/touch".into(),
                                        content: vec![],
                                    },
                                },
                                content: vec![],
                            },
                            content: vec![],
                        },
                    },
                    button_mapping_config: AutomaticButtonMappingConfigDefault {
                        gui_collapsed: true,
                        click_threshold: HysteresisThresholdDefault {
                            value: 0.5,
                            deviation: 0.05,
                        },
                        touch_threshold: HysteresisThresholdDefault {
                            value: 0.1,
                            deviation: 0.05,
                        },
                        force_threshold: 0.8,
                    },
                    hand_tracking_interaction: SwitchDefault {
                        enabled: false,
                        content: HandTrackingInteractionConfigDefault {
                            only_touch: false,
                            pinch_touch_distance: 0.0,
                            pinch_trigger_distance: 0.25,
                            curl_touch_distance: 2.0,
                            curl_trigger_distance: 2.5,
                            joystick_deadzone: 40.0,
                            joystick_offset_horizontal: 0.0,
                            joystick_offset_vertical: 0.0,
                            joystick_range: 1.0,
                            repeat_delay: 100,
                            activation_delay: 50,
                            deactivation_delay: 100,
                        },
                    },
                    steamvr_pipeline_frames: 2.1,
                    linear_velocity_cutoff: 0.05,
                    angular_velocity_cutoff: 10.0,
                    left_controller_position_offset: ArrayDefault {
                        gui_collapsed: true,
                        content: [0.0, 0.0, -0.11],
                    },
                    left_controller_rotation_offset: ArrayDefault {
                        gui_collapsed: true,
                        content: [0.0; 3],
                    },
                    left_hand_tracking_position_offset: ArrayDefault {
                        gui_collapsed: true,
                        content: [0.04, -0.02, -0.13],
                    },
                    left_hand_tracking_rotation_offset: ArrayDefault {
                        gui_collapsed: true,
                        content: [0.0, -45.0, -90.0],
                    },
                    haptics: SwitchDefault {
                        enabled: true,
                        content: HapticsConfigDefault {
                            gui_collapsed: true,
                            intensity_multiplier: 1.0,
                            amplitude_curve: 1.0,
                            min_duration_s: 0.01,
                        },
                    },
                },
            },
            recentering_mode: RecenteringModeDefault {
                Local: RecenteringModeLocalDefault { view_height: 1.5 },
                Tilted: RecenteringModeTiltedDefault { view_height: 1.5 },
                variant: RecenteringModeDefaultVariant::LocalFloor,
            },
            max_prediction_ms: 100,
        }
}
