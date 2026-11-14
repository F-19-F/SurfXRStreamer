use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Eq)]
pub enum ControllersEmulationMode {
    #[schema(strings(display_name = "Steam Frame Controllers"))]
    SteamFrame,
    #[schema(strings(display_name = "Rift S Touch"))]
    RiftSTouch,
    #[schema(strings(display_name = "Quest 1 Touch"))]
    Quest1Touch,
    #[schema(strings(display_name = "Quest 2 Touch"))]
    Quest2Touch,
    #[schema(strings(display_name = "Quest 3 Touch Plus"))]
    Quest3Plus,
    #[schema(strings(display_name = "Quest Pro"))]
    QuestPro,
    #[schema(strings(display_name = "Pico 4"))]
    Pico4,
    #[schema(strings(display_name = "PSVR2 Sense Controller"))]
    PSVR2Sense,
    #[schema(strings(display_name = "Valve Index"))]
    ValveIndex,
    #[schema(strings(display_name = "Vive Wand"))]
    ViveWand,
    #[schema(strings(display_name = "Vive Tracker"))]
    ViveTracker,
    Custom {
        serial_number: String,
        button_set: Vec<String>,
    },
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
pub struct HysteresisThreshold {
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub value: f32,
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub deviation: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
pub struct BinaryToScalarStates {
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub off: f32,
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub on: f32,
}

// Remaps 0..1 to custom range
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
pub struct Range {
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub min: f32,
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub max: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum ButtonMappingType {
    Passthrough,
    HysteresisThreshold(HysteresisThreshold),
    BinaryToScalar(BinaryToScalarStates),
    Remap(Range),
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct ButtonBindingTarget {
    pub destination: String,
    pub mapping_type: ButtonMappingType,
    pub binary_conditions: Vec<String>,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct AutomaticButtonMappingConfig {
    pub click_threshold: HysteresisThreshold,
    pub touch_threshold: HysteresisThreshold,
    pub force_threshold: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct HandTrackingInteractionConfig {
    #[schema(flag = "real-time")]
    pub only_touch: bool,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How close the tips of your fingers need to be to register a pinch click."
    ))]
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)), suffix = "cm")]
    pub pinch_touch_distance: f32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How close together the tips of your fingers need to be to start registering a pinch trigger pull."
    ))]
    #[schema(gui(slider(min = 0.0, max = 2.5, step = 0.025)), suffix = "cm")]
    pub pinch_trigger_distance: f32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How close to your palm the tips of your fingers need to be to register a curl click."
    ))]
    #[schema(gui(slider(min = 0.0, max = 5.0)), suffix = "cm")]
    pub curl_touch_distance: f32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How close to your palm the tips of your fingers need to be to start registering a trigger pull."
    ))]
    #[schema(gui(slider(min = 0.0, max = 10.0)), suffix = "cm")]
    pub curl_trigger_distance: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.0, max = 100.0)), suffix = "%")]
    pub joystick_deadzone: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -5.0, max = 5.0)), suffix = "cm")]
    pub joystick_offset_horizontal: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -5.0, max = 5.0)), suffix = "cm")]
    pub joystick_offset_vertical: f32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "The radius of motion of the joystick. The joystick can be controlled if the thumb is within 2x this range."
    ))]
    #[schema(gui(slider(min = 0.0, max = 5.0)), suffix = "cm")]
    pub joystick_range: f32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How long the gesture must be continuously held before it is activated."
    ))]
    #[schema(gui(slider(min = 0, max = 1000)), suffix = "ms")]
    pub activation_delay: u32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How long the gesture must be continuously released before it is deactivated."
    ))]
    #[schema(gui(slider(min = 0, max = 1000)), suffix = "ms")]
    pub deactivation_delay: u32,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "How long the after the gesture has been deactivated before it can be activated again."
    ))]
    #[schema(gui(slider(min = 0, max = 1000)), suffix = "ms")]
    pub repeat_delay: u32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct HapticsConfig {
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.0, max = 5.0, step = 0.1)))]
    pub intensity_multiplier: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    pub amplitude_curve: f32,

    #[schema(strings(display_name = "Minimum duration"))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.0, max = 0.1, step = 0.001)), suffix = "s")]
    pub min_duration_s: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct HandSkeletonConfig {
    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        display_name = "SteamVR input 2.0",
        help = r"Enabling this will use separate tracker objects with the full skeletal tracking level when hand tracking is detected. This is required for VRChat hand tracking."
    ))]
    pub steamvr_input_2_0: bool,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = r"Predict hand skeleton to make it less floaty. It may make hands too jittery."
    ))]
    pub predict: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct ControllersConfig {
    #[schema(strings(help = "Turning this off will make the controllers appear powered off."))]
    #[schema(flag = "real-time")]
    pub tracked: bool,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        help = "Enabling this passes skeletal hand data (finger tracking) to SteamVR."
    ))]
    pub hand_skeleton: Switch<HandSkeletonConfig>,

    #[schema(flag = "real-time")]
    #[schema(strings(
        help = "Enabling this allows using hand gestures to emulate controller inputs."
    ))]
    pub hand_tracking_interaction: Switch<HandTrackingInteractionConfig>,

    #[schema(strings(
        display_name = "Prediction",
        help = r"Higher values make the controllers track smoother.
Technically, this is the time (counted in frames) between pose submitted to SteamVR and the corresponding virtual vsync happens.
Currently this cannot be reliably estimated automatically. The correct value should be 2 but 3 is default for smoother tracking at the cost of slight lag."
    ))]
    #[schema(gui(slider(min = 1.0, max = 10.0, logarithmic)), suffix = "frames")]
    pub steamvr_pipeline_frames: f32,

    #[schema(flag = "real-time")]
    pub haptics: Switch<HapticsConfig>,

    #[schema(flag = "steamvr-restart")]
    pub emulation_mode: ControllersEmulationMode,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(display_name = "Extra OpenVR properties"))]
    pub extra_openvr_props: Vec<OpenvrProperty>,

    #[schema(flag = "real-time")]
    // note: logarithmic scale seems to be glitchy for this control
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)), suffix = "m/s")]
    pub linear_velocity_cutoff: f32,

    #[schema(flag = "real-time")]
    // note: logarithmic scale seems to be glitchy for this control
    #[schema(gui(slider(min = 0.0, max = 100.0, step = 1.0)), suffix = "掳/s")]
    pub angular_velocity_cutoff: f32,

    #[schema(flag = "real-time")]
    #[schema(strings(help = "Right controller offset is mirrored horizontally"))]
    // note: logarithmic scale seems to be glitchy for this control
    #[schema(gui(slider(min = -0.5, max = 0.5, step = 0.001)), suffix = "m")]
    pub left_controller_position_offset: [f32; 3],

    #[schema(flag = "real-time")]
    #[schema(strings(help = "Right controller offset is mirrored horizontally"))]
    #[schema(gui(slider(min = -180.0, max = 180.0, step = 1.0)), suffix = "掳")]
    pub left_controller_rotation_offset: [f32; 3],

    #[schema(flag = "real-time")]
    #[schema(strings(help = "Right controller offset is mirrored horizontally"))]
    // note: logarithmic scale seems to be glitchy for this control
    #[schema(gui(slider(min = -0.5, max = 0.5, step = 0.001)), suffix = "m")]
    pub left_hand_tracking_position_offset: [f32; 3],

    #[schema(flag = "real-time")]
    #[schema(strings(help = "Right controller offset is mirrored horizontally"))]
    #[schema(gui(slider(min = -180.0, max = 180.0, step = 1.0)), suffix = "掳")]
    pub left_hand_tracking_rotation_offset: [f32; 3],

    #[schema(strings(help = "List of OpenXR-syle paths"))]
    pub button_mappings: Option<Vec<(String, Vec<ButtonBindingTarget>)>>,

    pub button_mapping_config: AutomaticButtonMappingConfig,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum RecenteringMode {
    Stage,
    LocalFloor,
    Local {
        #[schema(gui(slider(min = 0.0, max = 3.0)), suffix = "m")]
        view_height: f32,
    },
    Tilted {
        #[schema(gui(slider(min = 0.0, max = 3.0)), suffix = "m")]
        view_height: f32,
    },
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
pub struct MultimodalTracking {
    pub enabled: bool,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        display_name = "Map non-held controllers to SteamVR trackers",
        help = "Non-held controllers are mapped to left and right feet.
This will be configurable in the future."
    ))]
    pub detached_controllers_steamvr_sink: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct HeadsetConfig {
    #[schema(strings(
        help = r"Stage: the playspace origin is determined by the room-scale guardian setup.
Local floor: the origin is on the floor and resets when long pressing the recentering button.
Local: the origin resets when long pressing the recentering button, and is calculated as an offset from the current head position.
Tilted: the world gets tilted when long pressing the recentering button. This is useful for using VR while laying down."
    ))]
    #[schema(flag = "real-time")]
    pub recentering_mode: RecenteringMode,

    #[schema(flag = "steamvr-restart")]
    pub controllers: Switch<ControllersConfig>,

    #[schema(flag = "steamvr-restart")]
    pub emulation_mode: HeadsetEmulationMode,

    #[schema(strings(
        help = r#"Power Savings might increase latency or reduce framerate consistency but decreases temperatures and improves battery life.
Sustained Low provides consistent framerates but might increase latency if necessary.
Sustained High provides consistent framerates but increases temperature.
This is mainly for Quest headsets, mileage may vary on other devices."#
    ))]
    pub performance_level: PerformanceLevelConfig,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(display_name = "Extra OpenVR properties"))]
    pub extra_openvr_props: Vec<OpenvrProperty>,

    #[schema(flag = "steamvr-restart")]
    pub tracking_ref_only: bool,

    #[schema(flag = "steamvr-restart")]
    pub enable_vive_tracker_proxy: bool,

    pub face_tracking: Switch<FaceTrackingConfig>,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        help = r"Track hand skeleton while holding controllers. This will reduce hand tracking frequency to 30Hz.
Because of runtime limitations, this option is ignored when body tracking is active."
    ))]
    pub multimodal_tracking: Switch<MultimodalTracking>,

    #[schema(flag = "steamvr-restart")]
    pub body_tracking: Switch<BodyTrackingConfig>,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(display_name = "VMC"))]
    pub vmc: Switch<VMCConfig>,

    #[schema(strings(
        help = "Maximum prediction for head and controllers. Used to avoid too much jitter during loading."
    ))]
    #[schema(gui(slider(min = 0, max = 200, step = 5)), suffix = "ms")]
    pub max_prediction_ms: u64,
}
