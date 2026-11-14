use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum HeadsetEmulationMode {
    #[schema(strings(display_name = "Steam Frame"))]
    SteamFrame,
    #[schema(strings(display_name = "Rift S"))]
    RiftS,
    #[schema(strings(display_name = "Quest 1"))]
    Quest1,
    #[schema(strings(display_name = "Quest 2"))]
    Quest2,
    #[schema(strings(display_name = "Quest Pro"))]
    QuestPro,
    #[schema(strings(display_name = "Pico 4"))]
    Pico4,
    Vive,
    Custom {
        serial_number: String,
    },
}

#[derive(SettingsSchema, Serialize, Deserialize, PartialEq, Clone)]
pub enum PerformanceLevel {
    #[schema(strings(display_name = "Power Saving"))]
    PowerSavings,
    #[schema(strings(display_name = "Sustained Low"))]
    SustainedLow,
    #[schema(strings(display_name = "Sustained High"))]
    SustainedHigh,
    #[schema(flag = "hidden")]
    Boost,
}

#[derive(SettingsSchema, Serialize, Deserialize, PartialEq, Clone)]
pub struct PerformanceLevelConfig {
    #[schema(flag = "real-time")]
    #[schema(strings(
        display_name = "CPU",
        help = "When disabling this, the client needs to be restarted for the change to be applied."
    ))]
    pub cpu: Switch<PerformanceLevel>,
    #[schema(flag = "real-time")]
    #[schema(strings(
        display_name = "GPU",
        help = "When disabling this, the client needs to be restarted for the change to be applied."
    ))]
    pub gpu: Switch<PerformanceLevel>,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
pub enum FaceTrackingSourcesConfig {
    PreferEyeTrackingOnly,
    PreferFullFaceTracking,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum FaceTrackingSocialPresenceSinkConfig {
    #[schema(strings(display_name = "VRChat Eye OSC"))]
    VrchatEyeOsc { port: u16 },
    #[schema(strings(display_name = "VRCFaceTracking"))]
    VrcFaceTracking,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct FaceTrackingSinkConfig {
    #[schema(strings(help = "Forward eye and face tracking to another application"))]
    pub social_presence: Switch<FaceTrackingSocialPresenceSinkConfig>,
    #[schema(strings(
        display_name = "Eye-tracked foveated encoding",
        help = "Use the selected eye tracking input to move the foveation center. Requires Video > Foveated encoding to be enabled. Turn this off to keep fixed foveated encoding."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub eye_tracked_foveated_encoding: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct OscPort {
    pub port: u16,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct FaceTrackingConfig {
    pub sources: FaceTrackingSourcesConfig,
    #[schema(strings(
        display_name = "Eye tracking OSC override",
        help = "Use external combined gaze received over OSC/UDP on localhost instead of headset eye directions. This input is shared by foveated encoding and social output. Turn this off to use headset eye tracking."
    ))]
    pub eye_tracking_osc_override: Switch<OscPort>,
    pub sink: FaceTrackingSinkConfig,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Default)]
pub struct BodyTrackingMetaConfig {
    pub prefer_full_body: bool,
    #[schema(strings(help = "Prefer active upper body tracking, Quest 3 only"))]
    pub prefer_high_fidelity: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(gui = "button_group")]
pub enum BodyTrackingBDConfig {
    #[schema(strings(display_name = "Body Tracking"))]
    BodyTracking {
        #[schema(strings(
            help = "Improves accuracy of the tracking at the cost of higher latency."
        ))]
        high_accuracy: bool,
        #[schema(strings(
            help = "If trackers have not been calibrated before, the calibration process will start after you connect to the streamer."
        ))]
        prompt_calibration_on_start: bool,
    },

    #[schema(strings(display_name = "Object Tracking"))]
    ObjectTracking,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
pub struct BodyTrackingSourcesConfig {
    pub meta: BodyTrackingMetaConfig,
    pub bd: BodyTrackingBDConfig,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum BodyTrackingSinkConfig {
    #[schema(strings(display_name = "Fake Vive Trackers"))]
    FakeViveTracker,
    #[schema(strings(display_name = "VRChat Body OSC"))]
    VrchatBodyOsc { port: u16 },
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct BodyTrackingConfig {
    pub sources: BodyTrackingSourcesConfig,
    pub sink: BodyTrackingSinkConfig,
    #[schema(strings(help = "Turn this off to temporarily pause tracking."))]
    #[schema(flag = "real-time")]
    pub tracked: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct VMCConfig {
    pub host: String,
    pub port: u16,
    #[schema(strings(help = "Turn this off to temporarily pause sending data."))]
    #[schema(flag = "real-time")]
    pub publish: bool,
    #[schema(flag = "real-time")]
    pub orientation_correction: bool,
}
