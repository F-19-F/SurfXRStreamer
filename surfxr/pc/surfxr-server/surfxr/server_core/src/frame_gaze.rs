//! Tracking/crop association in the original Frame clock.
use surfxr_sockets::{FrameGazeConfig, decode_frame_gaze};
use std::{
    collections::VecDeque,
    time::{Duration, Instant},
};
const CAPACITY: usize = 1024;
const FRESHNESS: Duration = Duration::from_millis(100);
pub struct FrameGazeState {
    pub config: FrameGazeConfig,
    resolution: [u32; 2],
    last: [Option<(u64, Instant, [f32; 2])>; 2],
    origin_epoch: u64,
    tracking: VecDeque<(Duration, [Instant; 2], [[u32; 2]; 2])>,
    encoded: VecDeque<(Duration, [[u32; 2]; 2])>,
}
impl FrameGazeState {
    pub fn new(config: FrameGazeConfig, resolution: [u32; 2]) -> Self {
        Self {
            config,
            resolution,
            last: [None; 2],
            origin_epoch: 0,
            tracking: VecDeque::new(),
            encoded: VecDeque::new(),
        }
    }
    fn crop(&self, p: [f32; 2]) -> [u32; 2] {
        let size = [self.config.focus_width, self.config.focus_height];
        std::array::from_fn(|axis| {
            let center = (p[axis] * 0.5 + 0.5) * self.resolution[axis] as f32;
            ((center - size[axis] as f32 / 2.0).round().max(0.0) as u32)
                .min(self.resolution[axis] - size[axis])
                & !1
        })
    }
    pub fn update(&mut self, timestamp: Duration, bytes: &[u8], now: Instant) -> bool {
        let Ok(sample) = decode_frame_gaze(bytes) else {
            return false;
        };
        if self
            .tracking
            .back()
            .is_some_and(|(previous, _, _)| timestamp <= *previous)
        {
            return true;
        }
        let stamp = timestamp.as_nanos() as u64;
        if let Some(sample) = sample {
            if sample.origin_epoch != self.origin_epoch {
                self.last = [None; 2];
                self.origin_epoch = sample.origin_epoch;
            }
            if sample.sampled_ns <= stamp && stamp - sample.sampled_ns < FRESHNESS.as_nanos() as u64
            {
                for (eye, value) in sample.eyes.into_iter().enumerate() {
                    if value.valid
                        && value
                            .position
                            .iter()
                            .all(|p| p.is_finite() && (-0.5..=0.5).contains(p))
                        && self.last[eye]
                            .is_none_or(|(previous, _, _)| sample.sampled_ns >= previous)
                    {
                        if self.last[eye]
                            .is_none_or(|(previous, _, _)| sample.sampled_ns != previous)
                        {
                            self.last[eye] = Some((sample.sampled_ns, now, value.position));
                        }
                    }
                }
            }
        } else {
            self.last = [None; 2];
        }
        let fresh: [_; 2] = std::array::from_fn(|eye| {
            self.last[eye]
                .filter(|(sample, received, _)| {
                    stamp >= *sample
                        && stamp - sample < FRESHNESS.as_nanos() as u64
                        && now.saturating_duration_since(*received) < FRESHNESS
                })
                .map(|(sample, received, p)| {
                    (
                        (received + FRESHNESS)
                            .min(now + FRESHNESS - Duration::from_nanos(stamp - sample)),
                        p,
                    )
                })
        });
        let crops = std::array::from_fn(|eye| self.crop(fresh[eye].map_or([0.0; 2], |(_, p)| p)));
        let expires = std::array::from_fn(|eye| fresh[eye].map_or(now, |(deadline, _)| deadline));
        self.tracking.push_back((timestamp, expires, crops));
        while self.tracking.len() > CAPACITY {
            self.tracking.pop_front();
        }
        true
    }
    pub fn compose(&mut self, timestamp: Duration, now: Instant) -> [[u32; 2]; 2] {
        if let Some((_, crop)) = self.encoded.iter().rev().find(|(ts, _)| *ts == timestamp) {
            return *crop;
        }
        let centered = self.crop([0.0; 2]);
        let crops = self
            .tracking
            .iter()
            .rev()
            .find(|(ts, _, _)| *ts == timestamp)
            .map_or([centered; 2], |(_, expires, crop)| {
                std::array::from_fn(|eye| {
                    if now < expires[eye] {
                        crop[eye]
                    } else {
                        centered
                    }
                })
            });
        self.encoded.push_back((timestamp, crops));
        while self.encoded.len() > CAPACITY {
            self.encoded.pop_front();
        }
        crops
    }
    pub fn encoded(&self, timestamp: Duration) -> Option<[[u32; 2]; 2]> {
        self.encoded
            .iter()
            .rev()
            .find_map(|(ts, crop)| (*ts == timestamp).then_some(*crop))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_sockets::{FrameGazeEye, FrameGazeSample};
    fn state() -> FrameGazeState {
        FrameGazeState::new(
            FrameGazeConfig {
                version: 1,
                base_width: 32,
                base_height: 32,
                focus_width: 32,
                focus_height: 32,
            },
            [128, 128],
        )
    }
    fn feedback(stamp: u64, epoch: u64, p: [f32; 2]) -> Vec<u8> {
        bincode::serde::encode_to_vec(
            Some(FrameGazeSample {
                sampled_ns: stamp,
                origin_epoch: epoch,
                eyes: std::array::from_fn(|_| FrameGazeEye {
                    valid: true,
                    position: p,
                }),
            }),
            bincode::config::standard(),
        )
        .unwrap()
    }
    #[test]
    fn resized_focus_uses_negotiated_crop_and_requires_stream_reinitialization() {
        let (_, config) = surfxr_sockets::negotiate_frame_gaze(
            r#"{"frame_gaze_streaming":1}"#,
            "{}",
            [3072, 3264],
            [20, 30],
            true,
        )
        .unwrap();
        let mut gaze = FrameGazeState::new(config.unwrap(), [3072, 3264]);
        let now = Instant::now();
        let timestamp = Duration::from_nanos(1000);
        assert_eq!(gaze.compose(timestamp, now), [[1216, 1136]; 2]);
        assert_eq!(gaze.encoded(timestamp), Some([[1216, 1136]; 2]));

        let init = surfxr_session::SteamvrHmdInitConfig::default();
        let settings = surfxr_session::SessionConfig::default().to_settings();
        let original = crate::connection::compute_restart_settings_hash(&init, &settings);
        for field in 0..3 {
            let mut modified = settings.clone();
            match field {
                0 => modified.video.frame_gaze.enabled = false,
                1 => modified.video.frame_gaze.focus_width_percent = 20,
                _ => modified.video.frame_gaze.focus_height_percent = 30,
            }
            assert_ne!(
                crate::connection::compute_restart_settings_hash(&init, &modified),
                original
            );
        }
    }

    #[test]
    fn exact_render_timestamp_and_repeated_encoded_frame_keep_their_crop() {
        let now = Instant::now();
        let mut s = state();
        let t = Duration::from_nanos(1000);
        assert!(s.update(t, &feedback(1000, 1, [0.5, -0.5]), now));
        assert_eq!(s.encoded(t), None);
        assert_eq!(s.compose(t, now), [[80, 16]; 2]);
        assert!(s.update(
            t + Duration::from_nanos(1),
            &feedback(1001, 1, [-0.5, 0.5]),
            now
        ));
        assert_eq!(s.compose(t, now + FRESHNESS), [[80, 16]; 2]);
        assert_eq!(s.compose(t + Duration::from_nanos(2), now), [[48, 48]; 2]);
        assert_eq!(s.encoded(t), Some([[80, 16]; 2]));
    }
    #[test]
    fn repeated_sample_does_not_extend_receipt_or_frame_clock_freshness() {
        let now = Instant::now();
        let mut s = state();
        let t = Duration::from_millis(1);
        let sample = feedback(1_000_000, 1, [0.5, 0.5]);
        assert!(s.update(t, &sample, now));
        let later = t + Duration::from_millis(90);
        assert!(s.update(later, &sample, now + Duration::from_millis(90)));
        assert_eq!(s.compose(later, now + FRESHNESS), [[48, 48]; 2]);
        // An already-old sample expires after its remaining 10 ms, even if it
        // has just arrived on the PC. The GPU and source frame use one crop.
        let mut s = state();
        assert!(s.update(later, &sample, now));
        assert_eq!(
            s.compose(later, now + Duration::from_millis(10)),
            [[48, 48]; 2]
        );
    }
    #[test]
    fn invalid_future_epoch_and_focus_loss_use_centered_fallback() {
        let now = Instant::now();
        let mut s = state();
        for (ts, sample, epoch, p) in [
            (10, 11, 1, [0.5, 0.5]),
            (12, 12, 1, [f32::NAN, 0.0]),
            (13, 13, 1, [0.6, 0.0]),
            (14, 14, 2, [f32::INFINITY, 0.0]),
        ] {
            let t = Duration::from_nanos(ts);
            assert!(s.update(t, &feedback(sample, epoch, p), now));
            assert_eq!(s.compose(t, now), [[48, 48]; 2]);
        }
        assert!(s.update(Duration::from_nanos(15), &feedback(15, 2, [0.5, 0.5]), now));
        assert!(s.update(Duration::from_nanos(16), &[0], now));
        assert_eq!(s.compose(Duration::from_nanos(16), now), [[48, 48]; 2]);
        assert!(!s.update(Duration::from_nanos(17), &[1], now));
    }
    #[test]
    fn tracking_and_encoded_histories_remain_bounded() {
        let now = Instant::now();
        let mut s = state();
        for i in 0..CAPACITY + 8 {
            let t = Duration::from_nanos(i as u64);
            assert!(s.update(t, &[0], now));
            s.compose(t, now);
        }
        assert_eq!(s.tracking.len(), CAPACITY);
        assert_eq!(s.encoded.len(), CAPACITY);
        assert_eq!(s.encoded(Duration::ZERO), None);
    }
}
