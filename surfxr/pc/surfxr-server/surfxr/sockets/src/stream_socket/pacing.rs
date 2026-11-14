//! Bounded Frame UDP shard bursts. No packet loss, wire change, or encoder cap.
use std::time::{Duration, Instant};

pub(super) struct VideoPacer {
    bytes_per_second: u64,
    next: Instant,
    #[cfg(windows)]
    timer: HighResolutionTimer,
}

impl VideoPacer {
    pub fn new(bytes_per_second: u64) -> Self {
        Self {
            bytes_per_second,
            next: Instant::now(),
            #[cfg(windows)]
            timer: HighResolutionTimer::new(),
        }
    }

    pub fn update_bitrate(&mut self, bitrate_bps: u64, packet_size: u64, overhead: u64) {
        let rate = (bitrate_bps / 4).saturating_mul(packet_size)
            / packet_size.saturating_add(overhead).max(1);
        if rate > 0 && rate != self.bytes_per_second {
            self.bytes_per_second = rate;
            self.next = Instant::now(); // Do not retain debt from the previous rate.
        }
    }

    pub fn wait_batch(&mut self) {
        let remaining = self.next.saturating_duration_since(Instant::now());
        if !remaining.is_zero() {
            // Wake shortly before the deadline. A sub-millisecond batch must
            // not accumulate timer dispatch latency on every shard batch.
            let sleep_for = remaining.saturating_sub(Duration::from_micros(150));
            #[cfg(windows)]
            let slept = sleep_for.is_zero() || self.timer.sleep(sleep_for);
            #[cfg(not(windows))]
            let slept = false;
            if !slept && remaining > Duration::from_millis(1) {
                std::thread::sleep(remaining - Duration::from_millis(1));
            }
            while Instant::now() < self.next {
                std::hint::spin_loop();
            }
        }
        // Late wakeups never accumulate a catch-up burst or pacing debt.
        self.next = Instant::now();
    }

    pub fn charge(&mut self, bytes: usize) {
        self.next += Duration::from_nanos(
            (bytes as u64).saturating_mul(1_000_000_000) / self.bytes_per_second,
        );
    }
}

// A private auto-reset timer is moved with its single writer, never shared.
// https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
#[cfg(windows)]
struct HighResolutionTimer {
    handle: usize,
}

#[cfg(windows)]
#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateWaitableTimerExW(
        attributes: *const std::ffi::c_void,
        name: *const u16,
        flags: u32,
        access: u32,
    ) -> *mut std::ffi::c_void;
    fn SetWaitableTimer(
        handle: *mut std::ffi::c_void,
        due: *const i64,
        period: i32,
        callback: *const std::ffi::c_void,
        argument: *const std::ffi::c_void,
        resume: i32,
    ) -> i32;
    fn WaitForSingleObject(handle: *mut std::ffi::c_void, timeout: u32) -> u32;
    fn CloseHandle(handle: *mut std::ffi::c_void) -> i32;
}

#[cfg(windows)]
impl HighResolutionTimer {
    fn new() -> Self {
        Self {
            handle: unsafe {
                CreateWaitableTimerExW(std::ptr::null(), std::ptr::null(), 2, 0x001f0003)
            } as usize,
        }
    }
    fn sleep(&self, duration: Duration) -> bool {
        if self.handle == 0 {
            return false;
        }
        let handle = self.handle as *mut std::ffi::c_void;
        let due = -((duration.as_nanos() / 100).clamp(1, i64::MAX as u128) as i64);
        unsafe {
            SetWaitableTimer(handle, &due, 0, std::ptr::null(), std::ptr::null(), 0) != 0
                && WaitForSingleObject(handle, 1000) == 0
        }
    }
}

#[cfg(windows)]
impl Drop for HighResolutionTimer {
    fn drop(&mut self) {
        if self.handle != 0 {
            unsafe {
                CloseHandle(self.handle as *mut std::ffi::c_void);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn live_bitrate_replaces_startup_budget_and_discards_old_debt() {
        let mut pacer = VideoPacer::new(25_000_000); // 100 Mbps session
        pacer.charge(1_000_000);
        pacer.update_bitrate(500_000_000, 1400, 28);
        assert_eq!(pacer.bytes_per_second, 125_000_000 * 1400 / 1428);
        assert!(pacer.next <= Instant::now());
        pacer.update_bitrate(20_000_000, 1400, 0);
        assert_eq!(pacer.bytes_per_second, 5_000_000);
        pacer.charge(1000);
        let deadline = pacer.next;
        pacer.update_bitrate(20_000_000, 1400, 0);
        assert_eq!(pacer.next, deadline); // unchanged frames retain burst pacing
        pacer.update_bitrate(0, 1400, 0);
        assert_eq!(pacer.bytes_per_second, 5_000_000);
    }
}
