//! Optional lossless IME image suffix on the existing video access unit.
//! Only the authenticated daemon enables this after explicit Frame negotiation.
use surfxr_common::parking_lot::Mutex;
use std::time::{Duration, Instant};

struct State {
    enabled: bool,
    image: Vec<u8>,
    updated: Option<Instant>,
    sent: Option<Instant>,
}
static STATE: Mutex<State> = Mutex::new(State {
    enabled: false, image: Vec::new(), updated: None, sent: None,
});

#[unsafe(no_mangle)]
pub unsafe extern "C" fn FrameDaemonKeyboardImageV1(data: *const u8, size: usize, enabled: bool) {
    if size > 65536 || (size > 0 && data.is_null()) { return; }
    let image = if size == 0 { Vec::new() } else {
        unsafe { std::slice::from_raw_parts(data, size) }.to_vec()
    };
    let mut state = STATE.lock();
    state.enabled = enabled;
    if state.image != image || !enabled { state.sent = None; }
    state.image = image;
    state.updated = Some(Instant::now());
}

pub(super) fn payload(data: &[u8]) -> Vec<u8> {
    let mut result = data.to_vec();
    let Some(mut state) = STATE.try_lock() else { return result; };
    if !state.enabled { return result; }
    let now = Instant::now();
    if state.sent.is_some_and(|last| now.duration_since(last) < Duration::from_millis(100)) {
        return result;
    }
    // A stopped capture owner cannot leave a permanent stale candidate list.
    let fresh = state.updated.is_some_and(|last| now.duration_since(last) < Duration::from_millis(500));
    let image = if fresh { state.image.as_slice() } else { &[] };
    result.extend_from_slice(image);
    result.extend_from_slice(&(image.len() as u32).to_le_bytes());
    result.extend_from_slice(b"FRIME001");
    state.sent = Some(now);
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn negotiated_suffix_clear_and_disable() {
        unsafe { FrameDaemonKeyboardImageV1(std::ptr::null(), 0, false) };
        assert_eq!(payload(&[1, 2, 3]), [1, 2, 3]);
        let image = [1, 0, 1, 0, 1, 0, 5, 6, 7];
        unsafe { FrameDaemonKeyboardImageV1(image.as_ptr(), image.len(), true) };
        let packet = payload(&[1, 2, 3]);
        assert_eq!(&packet[3..12], &image);
        assert_eq!(&packet[12..16], &9u32.to_le_bytes());
        assert_eq!(&packet[16..], b"FRIME001");
        assert_eq!(payload(&[1, 2, 3]), [1, 2, 3]);
        unsafe { FrameDaemonKeyboardImageV1(std::ptr::null(), 0, true) };
        assert_eq!(payload(&[1]).len(), 13);
        unsafe { FrameDaemonKeyboardImageV1(std::ptr::null(), 0, false) };
        assert_eq!(payload(&[1]), [1]);
    }
}
