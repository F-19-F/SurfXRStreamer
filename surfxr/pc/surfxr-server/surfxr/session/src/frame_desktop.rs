use surfxr_common::settings_schema::SettingsSchema;
use serde::{Deserialize, Serialize};

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct FrameDesktopConfig {
    #[schema(strings(
        help = "Allow trusted SURFXR devices to stream the PC desktop. Changes apply to the next desktop session; disabling access ends an active session."
    ))]
    pub enabled: bool,
    pub allow_input: bool,
    pub allow_audio: bool,
    #[schema(strings(display_name = "Encrypt unified streaming media",
        help = "Use AES-GCM for unified desktop/VR video, audio and tracking packets. Disabled by default. Applies on the next unified connection; authentication and reliable control stay encrypted. Plain media has no confidentiality or cryptographic integrity."))]
    pub media_encryption: bool,
    #[schema(strings(display_name = "Keep SteamVR tracking on desktop",
        help = "Continue updating headset and controller poses while streaming the desktop, if SteamVR is already running. Takes effect immediately; VR video encoding and transmission remain stopped."))]
    pub keep_steamvr_tracking: bool,
    #[schema(strings(
        help = "Require the installed LocalSystem desktop service. This permits UAC/administrator desktop access. No elevation is performed automatically."
    ))]
    pub require_system_service: bool,
    #[schema(gui(slider(min = 30, max = 144, step = 1)))]
    pub framerate: u32,
    #[schema(gui(slider(min = 1, max = 200, step = 1)))]
    pub bitrate_mbps: u32,
    #[schema(
        gui(slider(min = 1, max = 4, step = 1)),
        strings(
            help = "Desktop monitor count saved on this PC. Missing displays reduce the active count without changing this preference. Frame add/remove monitor controls update it."
        )
    )]
    pub monitor_count: u32,
}

#[cfg(test)]
mod tests {
    #[test]
    fn old_pc_session_migrates_and_selected_monitor_count_roundtrips() {
        let mut saved = serde_json::to_value(crate::SessionConfig::default()).unwrap();
        let desktop = saved["session_settings"]["extra"]["frame_desktop"]
            .as_object_mut()
            .unwrap();
        desktop.remove("monitor_count");
        desktop.remove("keep_steamvr_tracking");
        desktop.remove("media_encryption");
        desktop.insert("framerate".into(), serde_json::json!(90));
        let mut session = crate::SessionConfig::default();
        session.merge_from_json(&saved).unwrap();
        assert_eq!(session.to_settings().extra.frame_desktop.monitor_count, 1);
        assert!(session.to_settings().extra.frame_desktop.keep_steamvr_tracking);
        assert!(!session.to_settings().extra.frame_desktop.media_encryption);
        assert_eq!(session.to_settings().extra.frame_desktop.framerate, 90);
        session.session_settings.extra.frame_desktop.monitor_count = 3;
        session.session_settings.extra.frame_desktop.keep_steamvr_tracking = false;
        session.session_settings.extra.frame_desktop.media_encryption = true;
        let reloaded: crate::SessionConfig =
            serde_json::from_slice(&serde_json::to_vec(&session).unwrap()).unwrap();
        assert_eq!(reloaded.to_settings().extra.frame_desktop.monitor_count, 3);
        assert!(!reloaded.to_settings().extra.frame_desktop.keep_steamvr_tracking);
        assert!(reloaded.to_settings().extra.frame_desktop.media_encryption);
    }
}
