//! Preserve the trusted device's real address behind the local SYSTEM daemon.
use std::net::IpAddr;
pub(crate) fn ip(capabilities: &str, peer: IpAddr) -> IpAddr {
    if !peer.is_loopback() {
        return peer;
    }
    let Ok(value) = serde_json::from_str::<serde_json::Value>(capabilities) else {
        return peer;
    };
    if value["frame_daemon"].as_u64() != Some(1) {
        return peer;
    }
    value["frame_daemon_peer_ip"]
        .as_str()
        .and_then(|value| value.parse().ok())
        .unwrap_or(peer)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_local_daemon_can_report_the_original_peer() {
        let caps = r#"{"frame_daemon":1,"frame_daemon_peer_ip":"192.0.2.7"}"#;
        let local = "127.0.0.2".parse().unwrap();
        let real = "192.0.2.7".parse().unwrap();
        assert_eq!(ip(caps, local), real);
        assert_eq!(ip(caps, real), real);
        assert_eq!(ip(r#"{"frame_daemon_peer_ip":"192.0.2.7"}"#, local), local);
        assert_eq!(
            ip(r#"{"frame_daemon":1,"frame_daemon_peer_ip":"bad"}"#, local),
            local
        );
    }
}
