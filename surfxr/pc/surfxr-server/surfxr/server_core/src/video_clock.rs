//! Map actual PC capture intervals onto one client-clock anchor. Tracking
//! identity is separate: repeated/regressing poses do not identify new images.
use std::time::Duration;

#[derive(Default)]
pub struct CaptureClock {
    anchor: Option<(u64, Duration)>,
}
impl CaptureClock {
    pub fn timestamp(&mut self, capture_ns: u64, tracking: Duration) -> Option<Duration> {
        if capture_ns == 0 { return None; }
        let (capture, source) = *self.anchor.get_or_insert((capture_ns, tracking));
        source.checked_add(Duration::from_nanos(capture_ns.checked_sub(capture)?))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn distinct_captures_keep_real_intervals_despite_repeated_or_regressing_poses() {
        let mut clock = CaptureClock::default();
        let source = Duration::from_secs(10);
        assert_eq!(clock.timestamp(1_000_000, source), Some(source));
        assert_eq!(clock.timestamp(7_944_444, source), Some(source + Duration::from_nanos(6_944_444)));
        assert_eq!(clock.timestamp(14_888_888, source - Duration::from_millis(2)),
            Some(source + Duration::from_nanos(13_888_888)));
        // A real 40 ms game stall remains 40 ms, rather than manufactured ticks.
        assert_eq!(clock.timestamp(54_888_888, source), Some(source + Duration::from_nanos(53_888_888)));
    }
    #[test]
    fn held_source_reencodes_keep_identity_and_invalid_clocks_are_rejected() {
        let mut clock = CaptureClock::default();
        assert_eq!(clock.timestamp(0, Duration::ZERO), None);
        let source = Duration::from_secs(10);
        assert_eq!(clock.timestamp(100, source), Some(source));
        assert_eq!(clock.timestamp(100, source + Duration::from_secs(1)), Some(source));
        assert_eq!(clock.timestamp(99, source), None);
    }
}
