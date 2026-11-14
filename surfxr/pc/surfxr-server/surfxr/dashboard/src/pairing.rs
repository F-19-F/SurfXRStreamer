//! Process-local, single-candidate pairing window. Never persists auto-trust.
use surfxr_common::{ConnectionState, parking_lot::Mutex};
use surfxr_session::SessionConfig;
use std::{collections::HashSet, sync::LazyLock};

#[derive(Default)]
pub struct Pairing {
    enabled: bool,
    candidate: Option<String>,
    known: HashSet<String>,
}

impl Pairing {
    fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
        self.candidate = None;
    }

    fn claim(&mut self, hostname: &str, trusted: bool) -> bool {
        if !self.enabled || trusted {
            return false;
        }
        match &self.candidate {
            Some(candidate) => candidate == hostname,
            None => {
                self.candidate = Some(hostname.into());
                true
            }
        }
    }

    fn connected(&mut self, hostname: &str) {
        if self.candidate.as_deref() == Some(hostname) {
            self.set_enabled(false);
        }
    }

    fn observe(&mut self, session: &SessionConfig) -> Option<String> {
        if let Some(name) = self.candidate.clone() {
            if session.client_connections.get(&name).is_some_and(|client| {
                client.trusted
                    && matches!(
                        client.connection_state,
                        ConnectionState::Connected | ConnectionState::Streaming
                    )
            }) {
                self.connected(&name);
            }
        }
        let mut pending: Vec<_> = session
            .client_connections
            .iter()
            .filter(|(name, client)| {
                !client.trusted
                    && (!self.known.contains(*name)
                        || self.candidate.as_deref() == Some(name.as_str()))
            })
            .map(|(name, _)| name)
            .collect();
        self.known = session.client_connections.keys().cloned().collect();
        pending.sort();
        pending
            .into_iter()
            .find(|name| self.claim(name, false))
            .cloned()
    }
}

#[cfg(test)]
pub static TEST_SERIAL: Mutex<()> = Mutex::new(());

static WINDOW: LazyLock<Mutex<Pairing>> = LazyLock::new(Default::default);
pub fn enabled() -> bool {
    WINDOW.lock().enabled
}
pub fn set_enabled(value: bool) {
    WINDOW.lock().set_enabled(value);
}
pub fn claim(hostname: &str, trusted: bool) -> bool {
    WINDOW.lock().claim(hostname, trusted)
}
pub fn connected(hostname: &str) {
    WINDOW.lock().connected(hostname);
}
pub fn observe(session: &SessionConfig) -> Option<String> {
    WINDOW.lock().observe(session)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_candidate_only_and_known_devices_do_not_consume_window() {
        let mut window = Pairing::default();
        assert!(!window.claim("new", false));
        window.set_enabled(true);
        assert!(!window.claim("known", true));
        window.connected("known");
        assert!(window.enabled);
        assert!(window.claim("new", false));
        assert!(!window.claim("other", false));
        assert!(window.claim("new", false)); // retry after a failed admission
        window.connected("other");
        assert!(window.enabled);
        window.connected("new");
        assert!(!window.enabled);
        assert!(!window.claim("other", false));
        assert!(!Pairing::default().enabled); // new process starts disarmed
    }

    #[test]
    fn vr_connected_disarms_without_changing_saved_trust_or_settings() {
        let mut session = SessionConfig::default();
        session.client_connections.insert(
            "frame".into(),
            surfxr_session::ClientConnectionConfig {
                trusted: false,
                display_name: "Frame".into(),
                current_ip: None,
                manual_ips: Default::default(),
                connection_state: ConnectionState::Disconnected,
            },
        );
        let mut window = Pairing::default();
        window.set_enabled(true);
        let before = serde_json::to_value(&session).unwrap();
        assert_eq!(window.observe(&session).as_deref(), Some("frame"));
        assert_eq!(serde_json::to_value(&session).unwrap(), before);
        session.client_connections.get_mut("frame").unwrap().trusted = true;
        session
            .client_connections
            .get_mut("frame")
            .unwrap()
            .connection_state = ConnectionState::Connecting;
        window.observe(&session);
        assert!(window.enabled);
        session
            .client_connections
            .get_mut("frame")
            .unwrap()
            .connection_state = ConnectionState::Connected;
        window.observe(&session);
        assert!(!window.enabled);
        window.set_enabled(true);
        window.claim("second", false);
        window.set_enabled(false);
        assert!(!window.claim("second", false));
    }
    #[test]
    fn stale_untrusted_entries_do_not_reserve_a_new_window() {
        let mut session = SessionConfig::default();
        session.client_connections.insert(
            "old-offline".into(),
            surfxr_session::ClientConnectionConfig {
                trusted: false,
                display_name: "Frame".into(),
                current_ip: None,
                manual_ips: Default::default(),
                connection_state: ConnectionState::Disconnected,
            },
        );
        let mut window = Pairing::default();
        assert!(window.observe(&session).is_none());
        window.set_enabled(true);
        assert!(window.observe(&session).is_none());
        // An explicit new connection attempt from a previously listed device still works.
        assert!(window.claim("old-offline", false));
        assert_eq!(window.observe(&session).as_deref(), Some("old-offline"));
    }
}
