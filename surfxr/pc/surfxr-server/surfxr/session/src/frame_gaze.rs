//! PC preferences for the negotiated Frame gaze atlas. Wire geometry remains version 1.
use serde::{Deserialize, Serialize};
use settings_schema::SettingsSchema;

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct FrameGazeConfig {
    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        display_name = "Enable Dynamic Foveated Streaming",
        help = "Allow the Frame gaze atlas. The headset must also enable foveated streaming and support eye gaze. Enabling this in Dashboard disables upstream foveated encoding. Requires Windows, UDP, H.264/HEVC, SDR 8-bit and gamma 1."
    ))]
    pub enabled: bool,
    #[schema(flag = "steamvr-restart")]
    #[schema(gui(slider(min = 10, max = 33, step = 1)), suffix = "%")]
    #[schema(strings(
        display_name = "Focus width",
        help = "Full-resolution crop width as a percentage of one eye's image width, rounded up to 32 pixels. Larger regions cover more gaze movement but cost more encoding and decoding work. The version 1 atlas limits width to the base-image slot."
    ))]
    pub focus_width_percent: u32,
    #[schema(flag = "steamvr-restart")]
    #[schema(gui(slider(min = 10, max = 33, step = 1)), suffix = "%")]
    #[schema(strings(
        display_name = "Focus height",
        help = "Full-resolution crop height as a percentage of one eye's image height, rounded up to 32 pixels. 25% by 25% preserves the existing SurfXR gaze region. This changes streaming geometry, not the game's rendering resolution."
    ))]
    pub focus_height_percent: u32,
}

pub fn defaults() -> FrameGazeConfigDefault {
    FrameGazeConfigDefault {
        enabled: true,
        focus_width_percent: 25,
        focus_height_percent: 25,
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn default_gaze_behavior_and_custom_region_roundtrip() {
        let mut session = crate::SessionConfig::default();
        session.session_settings.video.preferred_fps = 144.0;
        let gaze = session.to_settings().video.frame_gaze;
        assert!(gaze.enabled);
        assert_eq!(
            (gaze.focus_width_percent, gaze.focus_height_percent),
            (25, 25)
        );
        session.session_settings.video.frame_gaze.enabled = false;
        session
            .session_settings
            .video
            .frame_gaze
            .focus_width_percent = 20;
        session
            .session_settings
            .video
            .frame_gaze
            .focus_height_percent = 33;
        let saved = serde_json::to_value(&session).unwrap();
        let mut reloaded = crate::SessionConfig::default();
        reloaded.merge_from_json(&saved).unwrap();
        let settings = reloaded.to_settings();
        assert!(!settings.video.frame_gaze.enabled);
        assert_eq!(
            (
                settings.video.frame_gaze.focus_width_percent,
                settings.video.frame_gaze.focus_height_percent
            ),
            (20, 33)
        );
        assert_eq!(settings.video.preferred_fps, 144.0);
    }
}
