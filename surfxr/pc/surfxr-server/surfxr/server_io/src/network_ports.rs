//! Migrate only old SurfXR defaults before any network owner starts.
use surfxr_common::ports;
use surfxr_session::SessionConfig;

pub(super) fn migrate(session: &mut SessionConfig) -> bool {
    let connection = &mut session.session_settings.connection;
    let mut changed = false;
    for (value, old, new) in [
        (&mut connection.web_server_port, 8082, ports::WEB),
        (&mut connection.stream_port, 9944, ports::VR_MEDIA),
        (&mut connection.osc_local_port, 9942, ports::OSC_LOCAL),
        (&mut session.session_settings.headset.face_tracking.content.eye_tracking_osc_override.content.port,
            9945, ports::GAZE_OSC),
    ] {
        if *value == old { *value = new; changed = true; }
    }
    changed
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ServerSessionManager;
    #[test]
    fn saved_defaults_move_without_resetting_settings_trust_or_receipts() {
        let mut session = SessionConfig::default();
        let c = &mut session.session_settings.connection;
        c.web_server_port = 8082; c.stream_port = 9944; c.osc_local_port = 9942;
        session.session_settings.headset.face_tracking.content.eye_tracking_osc_override.content.port = 9945;
        session.frame_settings_sync.revision = 42;
        session.frame_settings_sync.receipts = vec![(123, 7)];
        session.restart_settings_hash = 456;
        session.session_settings.extra.frame_desktop.bitrate_mbps = 73;
        session.client_connections.insert("frame-test".into(), surfxr_session::ClientConnectionConfig {
            display_name: "Paired Frame".into(), current_ip: None, manual_ips: Default::default(),
            trusted: true, connection_state: surfxr_common::ConnectionState::Disconnected,
        });
        let mut expected = session.clone();
        let dir = std::env::temp_dir().join(format!("surfxr-port-migration-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("session.json");
        std::fs::write(&path, serde_json::to_vec(&session).unwrap()).unwrap();
        assert!(migrate(&mut expected));
        let manager = ServerSessionManager::new(Some(path.clone()));
        let saved: SessionConfig = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(serde_json::to_value(manager.session()).unwrap(), serde_json::to_value(&expected).unwrap());
        assert_eq!(serde_json::to_value(&saved).unwrap(), serde_json::to_value(&expected).unwrap());
        assert!(!migrate(&mut expected));
        std::fs::remove_file(path).unwrap(); std::fs::remove_dir(dir).unwrap();
    }
    #[test]
    fn custom_ports_remain_unchanged() {
        let mut session = SessionConfig::default();
        session.session_settings.connection.stream_port = 23456;
        session.session_settings.connection.web_server_port = 23457;
        let before = serde_json::to_value(&session).unwrap();
        assert!(!migrate(&mut session));
        assert_eq!(serde_json::to_value(&session).unwrap(), before);
    }
}
