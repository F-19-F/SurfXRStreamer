//! Desired settings are persisted immediately; the running driver keeps its launch snapshot.
use surfxr_session::{FrameSize, Settings, SteamvrHmdInitConfig};

pub fn initial_config(settings: &Settings, mut config: SteamvrHmdInitConfig) -> SteamvrHmdInitConfig {
    fn apply(size: &FrameSize, width: &mut u32, height: &mut u32) {
        if let FrameSize::Absolute { width: w, height: h } = size {
            let ratio = *height as f64 / (*width).max(1) as f64;
            *width = (*w / 32 * 32).max(32);
            *height = (h.unwrap_or((*w as f64 * ratio) as u32) / 32 * 32).max(32);
        }
    }
    apply(&settings.video.transcoding_view_resolution, &mut config.eye_resolution_width, &mut config.eye_resolution_height);
    apply(&settings.video.emulated_headset_view_resolution, &mut config.target_eye_resolution_width, &mut config.target_eye_resolution_height);
    config.refresh_rate = settings.video.preferred_fps as _;
    config
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn first_launch_uses_saved_resolution_and_rate_without_negotiation_restart() {
        let mut settings = surfxr_session::SessionConfig::default().to_settings();
        settings.video.preferred_fps = 90.;
        settings.video.transcoding_view_resolution = FrameSize::Absolute { width: 2112, height: Some(2304) };
        let config = initial_config(&settings, SteamvrHmdInitConfig::default());
        assert_eq!((config.eye_resolution_width, config.eye_resolution_height, config.refresh_rate), (2112,2304,90));
    }
}
