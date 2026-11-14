use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct RawEventsConfig {
    #[schema(flag = "real-time")]
    pub hide_spammy_events: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct LoggingConfig {
    #[schema(flag = "real-time")]
    #[schema(strings(help = "Warnings by default. Debug enables detailed diagnostic logs. Changes apply immediately."))]
    pub log_level: LogSeverity,
    #[schema(strings(help = "Notification tips teach you how to use SURFXR"))]
    pub show_notification_tip: bool,

    #[schema(strings(help = "This applies only to certain error or warning messages."))]
    #[schema(flag = "steamvr-restart")]
    pub prefer_backtrace: bool,

    #[schema(flag = "real-time")]
    pub notification_level: LogSeverity,

    pub client_log_report_level: Switch<LogSeverity>,

    #[schema(flag = "real-time")]
    pub show_raw_events: Switch<RawEventsConfig>,

    #[schema(strings(help = "Write logs into the session_log.txt file."))]
    pub log_to_disk: bool,

    #[schema(flag = "real-time")]
    pub log_tracking: bool,

    #[schema(flag = "real-time")]
    pub log_button_presses: bool,

    #[schema(flag = "real-time")]
    pub log_haptics: bool,

    #[cfg_attr(not(debug_assertions), schema(flag = "hidden"))]
    #[schema(strings(help = "These settings enable extra spammy logs for debugging purposes."))]
    pub debug_groups: DebugGroupsConfig,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct SteamvrLauncher {
    #[schema(strings(
        display_name = "Enable SURFXR on manual SteamVR start",
        help = "Load the SURFXR virtual headset when SteamVR is started directly from Steam or a shortcut. Disabled by default. Starting or restarting from the SurfXR Dashboard or client always loads SURFXR. Changes apply to the next SteamVR start."
    ))]
    pub enable_surfxr_on_manual_steamvr_start: bool,
    #[schema(strings(
        display_name = "Open and close SteamVR automatically",
        help = "Launches SteamVR automatically when the SURFXR dashboard is opened, and closes it when the dashboard is closed."
    ))]
    pub open_close_steamvr_with_dashboard: bool,

    #[cfg_attr(
        windows,
        schema(strings(help = "Directly start the VR server, bypassing Steam. \
                Will run start_server.bat if it exists alongside session.json, and try to automatically find SteamVR otherwise."))
    )]
    #[cfg_attr(
        not(windows),
        schema(strings(help = "Directly start the VR server, bypassing Steam. \
                Will run start_server.sh if it exists alongside session.json, and try to automatically find SteamVR otherwise."))
    )]
    pub direct_launch: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct RollingVideoFilesConfig {
    #[schema(strings(display_name = "Duration"))]
    #[schema(suffix = "s")]
    pub duration_s: u64,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct CaptureConfig {
    #[schema(strings(display_name = "Start video recording at client connection"))]
    pub startup_video_recording: bool,

    pub rolling_video_files: Switch<RollingVideoFilesConfig>,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct NewVersionPopupConfig {
    pub hide_while_version: String,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct ExtraConfig {
    #[schema(strings(display_name = "Steam Frame Desktop"))]
    pub frame_desktop: FrameDesktopConfig,
    #[schema(strings(display_name = "SteamVR Launcher"))]
    pub steamvr_launcher: SteamvrLauncher,
    pub capture: CaptureConfig,
    pub logging: LoggingConfig,

    #[schema(
        strings(help = "Linear and angular velocity multiplier for debug purposes.
It does not update in real time.")
    )]
    pub velocities_multiplier: f32,

    pub open_setup_wizard: bool,
    pub new_version_popup: Switch<NewVersionPopupConfig>,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct Settings {
    pub video: VideoConfig,
    pub audio: AudioConfig,
    pub headset: HeadsetConfig,
    pub connection: ConnectionConfig,
    pub extra: ExtraConfig,
}
