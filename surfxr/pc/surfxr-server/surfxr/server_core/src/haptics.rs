use surfxr_packets::Haptics;
use surfxr_session::HapticsConfig;
use std::time::{Duration, Instant};

/// Fixed two-hand state sampled by video send, never a FIFO of old pulses.
#[derive(Default)]
pub struct FrameVideoHaptics {
    hands: [Option<(f32, Instant)>; 2],
    pending: [Option<(f32, Instant)>; 2],
}

impl FrameVideoHaptics {
    /// Import a two-hand process snapshot with the original expiration/age.
    /// The caller only supplies a pending pulse once per publication serial.
    pub fn import_process_hand(&mut self, hand: usize, amplitude: f32, remaining: Duration,
        pending: Option<(f32, Duration)>, now: Instant) {
        if hand >= 2 || !amplitude.is_finite() { return; }
        self.hands[hand] = Some((amplitude.clamp(0.0, 1.0), now + remaining.min(Duration::from_secs(5))));
        if let Some((pulse, age)) = pending
            && pulse.is_finite() && age < Duration::from_millis(100)
            && let Some(received) = now.checked_sub(age) {
            self.pending[hand] = Some((pulse.clamp(0.0, 1.0), received));
        }
    }
    pub fn update(&mut self, haptics: Haptics, now: Instant) {
        let hand = if haptics.device_id == *surfxr_common::HAND_LEFT_ID {
            0
        } else if haptics.device_id == *surfxr_common::HAND_RIGHT_ID {
            1
        } else {
            return;
        };
        if !haptics.amplitude.is_finite() {
            return;
        }
        let duration = haptics.duration.min(Duration::from_secs(5));
        let amplitude = haptics.amplitude.clamp(0.0, 1.0);
        self.hands[hand] = Some((amplitude, now + duration));
        if amplitude > 0.0 && !duration.is_zero() {
            self.pending[hand] = Some((amplitude, now));
        }
    }

    pub fn snapshot(&mut self, now: Instant) -> [f32; 2] {
        std::array::from_fn(|hand| {
            // mailbox does. Never replay unsent feedback after a long stall.
            let pending = self.pending[hand].take().filter(|(_, received)| {
                now.checked_duration_since(*received)
                    .is_some_and(|age| age < Duration::from_millis(100))
            });
            pending
                .or_else(|| self.hands[hand].filter(|(_, end)| now < *end))
                .map_or(0.0, |(amplitude, _)| amplitude)
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn process_import_keeps_expiry_and_does_not_replay_stale_pulses() {
        let now = Instant::now();
        let mut state = FrameVideoHaptics::default();
        state.import_process_hand(0, 0.4, Duration::from_millis(10), None, now);
        assert_eq!(state.snapshot(now), [0.4, 0.0]);
        assert_eq!(state.snapshot(now + Duration::from_millis(10)), [0.0, 0.0]);
        let now = now + Duration::from_millis(10);
        state.import_process_hand(1, 0.0, Duration::ZERO,
            Some((0.7, Duration::from_millis(99))), now);
        assert_eq!(state.snapshot(now), [0.0, 0.7]);
        assert_eq!(state.snapshot(now), [0.0, 0.0]);
        state.import_process_hand(1, 0.0, Duration::ZERO,
            Some((0.8, Duration::from_millis(100))), now);
        state.import_process_hand(2, 1.0, Duration::MAX, None, now);
        state.import_process_hand(0, f32::NAN, Duration::MAX, None, now);
        assert_eq!(state.snapshot(now), [0.0, 0.0]);
    }

    #[test]
    fn video_state_is_bounded_per_hand_and_stops_at_the_original_deadline() {
        let now = Instant::now();
        let mut state = FrameVideoHaptics::default();
        let pulse = |device_id, amplitude, duration| Haptics {
            device_id,
            amplitude,
            duration,
            frequency: 100.0,
        };
        state.update(
            pulse(*surfxr_common::HAND_LEFT_ID, 0.4, Duration::from_millis(50)),
            now,
        );
        state.update(
            pulse(*surfxr_common::HAND_RIGHT_ID, 0.7, Duration::from_millis(80)),
            now,
        );
        assert_eq!(state.snapshot(now), [0.4, 0.7]);
        assert_eq!(state.snapshot(now + Duration::from_millis(50)), [0.0, 0.7]);
        state.update(
            pulse(*surfxr_common::HAND_RIGHT_ID, 0.0, Duration::ZERO),
            now + Duration::from_millis(60),
        );
        assert_eq!(state.snapshot(now + Duration::from_millis(60)), [0.0, 0.0]);
        state.update(
            pulse(*surfxr_common::HAND_LEFT_ID, f32::NAN, Duration::MAX),
            now,
        );
        state.update(pulse(0, 1.0, Duration::MAX), now);
        assert_eq!(state.snapshot(now + Duration::from_millis(80)), [0.0, 0.0]);
        state.update(pulse(*surfxr_common::HAND_LEFT_ID, 2.0, Duration::MAX), now);
        assert_eq!(state.snapshot(now), [1.0, 0.0]);
        assert_eq!(state.snapshot(now + Duration::from_secs(5)), [0.0, 0.0]);
    }

    #[test]
    fn sub_frame_pulse_survives_zero_once_without_replaying_after_stall() {
        let now = Instant::now();
        let mut state = FrameVideoHaptics::default();
        let pulse = |amplitude| Haptics {
            device_id: *surfxr_common::HAND_LEFT_ID,
            amplitude,
            duration: Duration::from_millis(5),
            frequency: 100.0,
        };
        state.update(pulse(0.4), now);
        assert_eq!(state.snapshot(now + Duration::from_millis(8)), [0.4, 0.0]);
        assert_eq!(state.snapshot(now + Duration::from_millis(9)), [0.0, 0.0]);
        state.update(pulse(0.7), now + Duration::from_millis(10));
        state.update(pulse(0.0), now + Duration::from_millis(12));
        assert_eq!(state.snapshot(now + Duration::from_millis(18)), [0.7, 0.0]);
        assert_eq!(state.snapshot(now + Duration::from_millis(19)), [0.0, 0.0]);
        state.update(pulse(0.8), now + Duration::from_millis(20));
        assert_eq!(state.snapshot(now + Duration::from_millis(120)), [0.0, 0.0]);
    }
}

pub fn map_haptics(config: &HapticsConfig, haptics: Haptics) -> Haptics {
    Haptics {
        duration: Duration::max(
            haptics.duration,
            Duration::from_secs_f32(config.min_duration_s),
        ),
        amplitude: config.intensity_multiplier
            * f32::powf(haptics.amplitude, config.amplitude_curve),
        ..haptics
    }
}
