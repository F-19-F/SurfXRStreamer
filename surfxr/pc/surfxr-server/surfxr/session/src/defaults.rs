//! SurfXR defaults specialize the byte-pinned upstream schema without changing its format.
pub fn session_settings_default() -> crate::SessionSettings {
    let mut settings = crate::settings::session_settings_default();
    // The atlas and upstream foveated encoder cannot both own the video geometry.
    settings.video.foveated_encoding.enabled = false;
    settings
}

#[cfg(test)]
mod tests {
    #[test]
    fn frame_compiled_defaults_match_pc_session_defaults() {
        // Cross-language contract: exercise the actual PC defaults and the
        // single initializer consumed by the Frame launcher/viewer/preferences.
        let header = include_str!("../../../../../frame/preferences/include/surfxr/remote_settings.hpp");
        let initializer = header.split_once("kDefaultRemoteSettings{").unwrap().1
            .split_once('}').unwrap().0;
        let client: Vec<f64> = initializer.split(',').map(|v| v.trim().parse().unwrap()).collect();
        assert_eq!(client.len(), 10);
        let session = crate::SessionConfig::default();
        let settings = session.to_settings();
        assert_eq!(client[0], settings.video.preferred_fps as f64);
        let (width, height) = crate::frame_settings::TIERS[(client[1] as i32 + 1) as usize];
        for size in [&settings.video.transcoding_view_resolution, &settings.video.emulated_headset_view_resolution] {
            assert!(matches!(size, crate::FrameSize::Absolute { width: w, height: Some(h) } if *w == width && *h == height));
        }
        assert!(matches!(settings.video.bitrate.mode, crate::BitrateMode::ConstantMbps(value) if client[2] == value as f64));
        assert_eq!(client[3], settings.extra.frame_desktop.framerate as f64);
        assert_eq!(client[4], settings.extra.frame_desktop.bitrate_mbps as f64);
        assert_eq!(client[7] != 0., settings.video.frame_gaze.enabled);
        assert_eq!(client[9] != 0., session.frame_settings_sync.prefer_pc_mouse);
    }

    #[test]
    fn new_sessions_select_only_gaze_and_existing_sessions_keep_saved_values() {
        let defaults = crate::SessionConfig::default();
        assert!(defaults.session_settings.video.frame_gaze.enabled);
        assert!(!defaults.session_settings.video.foveated_encoding.enabled);
        let mut saved = serde_json::to_value(&defaults).unwrap();
        saved["session_settings"]["video"]["foveated_encoding"]["enabled"] = true.into();
        let mut loaded = crate::SessionConfig::default();
        loaded.merge_from_json(&saved).unwrap();
        assert!(loaded.session_settings.video.foveated_encoding.enabled);
        assert_eq!(serde_json::to_value(loaded).unwrap(), saved);
    }
}
