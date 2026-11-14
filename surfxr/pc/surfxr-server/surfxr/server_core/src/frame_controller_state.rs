//! Negotiated, reliable controller forwarding state; timestamps use the client clock.
use surfxr_session::ControllersEmulationMode;
use std::time::Duration;

pub fn supported(capabilities: &str, mode: Option<&ControllersEmulationMode>) -> bool {
    matches!(mode, Some(ControllersEmulationMode::SteamFrame))
        && serde_json::from_str::<serde_json::Value>(capabilities)
            .ok()
            .is_some_and(|v| {
                v["frame_controller_inputs"].as_u64() == Some(1)
                    && v["frame_controller_state"].as_u64() == Some(1)
            })
}

#[derive(Clone, Copy, Debug)]
pub struct ControllerState {
    pub enabled: bool,
    pub since_ns: u64,
}
impl ControllerState {
    pub fn session(negotiated: bool) -> Self {
        Self {
            enabled: !negotiated,
            since_ns: 0,
        }
    }
    pub fn apply(&mut self, next: Self) -> bool {
        // Zero is a trusted, internal session reset, never accepted from the wire.
        if next.since_ns != 0 && next.since_ns <= self.since_ns {
            return false;
        }
        *self = next;
        true
    }
    pub fn allows(&self, poll_timestamp: Duration) -> bool {
        self.enabled && poll_timestamp.as_nanos() >= self.since_ns as u128
    }
}
pub fn decode(bytes: &[u8]) -> Option<ControllerState> {
    if bytes.len() != 20 {
        return None;
    }
    let word = |i| u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap());
    let since_ns = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
    if word(0) != 0x56444353 || word(4) != 1 || word(16) > 1 || since_ns == 0 {
        return None;
    }
    Some(ControllerState {
        enabled: word(16) == 1,
        since_ns,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn packet(enabled: u32, time: u64) -> Vec<u8> {
        [
            0x56444353u32.to_le_bytes().as_slice(),
            1u32.to_le_bytes().as_slice(),
            time.to_le_bytes().as_slice(),
            enabled.to_le_bytes().as_slice(),
        ]
        .concat()
    }
    #[test]
    fn bounded_wire_and_session_gate() {
        let bytes = packet(0, 100);
        for n in 0..20 {
            assert!(decode(&bytes[..n]).is_none());
        }
        assert!(decode(&[bytes.clone(), vec![0]].concat()).is_none());
        assert!(decode(&packet(2, 100)).is_none());
        assert!(decode(&packet(1, 0)).is_none());
        for offset in [0, 4] {
            let mut bad = bytes.clone();
            bad[offset] ^= 1;
            assert!(decode(&bad).is_none());
        }
        let mut gate = ControllerState::session(true);
        assert!(!gate.allows(Duration::from_nanos(999)));
        assert!(gate.apply(decode(&packet(1, 50)).unwrap()));
        assert!(gate.allows(Duration::from_nanos(50)));
        assert!(gate.apply(decode(&bytes).unwrap()));
        assert!(!gate.allows(Duration::from_nanos(999)));
        assert!(!gate.apply(decode(&packet(1, 50)).unwrap()));
        assert!(gate.apply(decode(&packet(1, 200)).unwrap()));
        assert!(!gate.allows(Duration::from_nanos(199)));
        assert!(gate.allows(Duration::from_nanos(200)));
        assert!(gate.apply(ControllerState::session(true)));
        assert!(gate.apply(decode(&packet(1, 1)).unwrap()));
        assert!(ControllerState::session(false).allows(Duration::ZERO));
    }
    #[test]
    fn negotiation_requires_native_frame_and_both_capabilities() {
        let caps = r#"{"frame_controller_inputs":1,"frame_controller_state":1}"#;
        assert!(supported(caps, Some(&ControllersEmulationMode::SteamFrame)));
        assert!(!supported(
            caps,
            Some(&ControllersEmulationMode::ValveIndex)
        ));
        for caps in [
            "{}",
            r#"{"frame_controller_state":1}"#,
            r#"{"frame_controller_inputs":1}"#,
        ] {
            assert!(!supported(
                caps,
                Some(&ControllersEmulationMode::SteamFrame)
            ));
        }
    }
}
