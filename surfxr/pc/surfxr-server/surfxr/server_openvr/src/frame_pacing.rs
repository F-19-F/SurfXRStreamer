//! Preserve the existing compositor grid, with bounded wake headroom on Windows.
//! Numeric telemetry distinguishes an overslept deadline from game/GPU stalls.
use std::{cell::RefCell, thread, time::{Duration, Instant}};

const HEADROOM: Duration = Duration::from_micros(150);
struct Timing {
    started: Instant,
    waits: u64,
    wake_max_us: u64,
    late_wakes: u64,
}
thread_local! {
    static TIMING: RefCell<Timing> = RefCell::new(Timing {
        started: Instant::now(), waits: 0, wake_max_us: 0, late_wakes: 0,
    });
}
pub fn wait(duration: Duration) {
    let deadline = Instant::now() + duration;
    if duration > HEADROOM { thread::sleep(duration - HEADROOM); }
    while Instant::now() < deadline { std::hint::spin_loop(); }
    let now = Instant::now();
    let late = now.saturating_duration_since(deadline).as_micros() as u64;
    TIMING.with_borrow_mut(|timing| {
        timing.waits += 1;
        timing.wake_max_us = timing.wake_max_us.max(late);
        timing.late_wakes += u64::from(late > 500);
        let seconds = now.duration_since(timing.started).as_secs_f64();
        if seconds >= 5.0 {
            surfxr_common::info!("Frame pacing timing: {}", serde_json::json!({
                "seconds": seconds, "waits": timing.waits,
                "wake_max_us": timing.wake_max_us, "late_wakes": timing.late_wakes,
            }));
            *timing = Timing { started: now, waits: 0, wake_max_us: 0, late_wakes: 0 };
        }
    });
}
