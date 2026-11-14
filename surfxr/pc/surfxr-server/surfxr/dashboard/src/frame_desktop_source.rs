use super::*;
use surfxr_packets::{ClientConnectionsAction, PathSegment, PathValuePair};
use surfxr_session::{SessionConfig, Settings};
use std::io::Read;
use std::net::IpAddr;

fn owner_session(path: &std::path::Path) -> Option<SessionConfig> {
    let mut bytes = Vec::new();
    std::fs::File::open(path)
        .ok()?
        .take(2 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .ok()?;
    if bytes.len() > 2 * 1024 * 1024 {
        return None;
    }
    serde_json::from_slice(&bytes).ok()
}

pub(super) fn spawn(
    running: Arc<RelaxedAtomic>,
    source: Arc<Mutex<SessionSource>>,
    context: egui::Context,
    events: mpsc::Sender<PolledEvent>,
    port: u16,
) -> JoinHandle<()> {
    let settings_source = source.clone();
    let settings_context = context.clone();
    let settings_events = events.clone();
    let save_source = source.clone();
    let save_context = context.clone();
    let save_events = events.clone();
    crate::frame_desktop::spawn(
        running,
        move |requested, ip, register| {
            let mut guard = source.lock();
            if let SessionSource::Local(manager) = &mut *guard {
                let hostname = resolve(manager.session(), requested, ip);
                if register {
                    let auto_trust = matches!(&manager.settings().connection.client_discovery,
                    surfxr_common::settings_schema::Switch::Enabled(c) if c.auto_trust_clients);
                    manager.update_client_connections(
                        hostname.clone(),
                        ClientConnectionsAction::AddIfMissing {
                            trusted: auto_trust,
                            manual_ips: vec![],
                        },
                    );
                    let trusted = manager
                        .client_list()
                        .get(&hostname)
                        .is_some_and(|c| c.trusted);
                    if crate::pairing::claim(&hostname, trusted) {
                        manager.update_client_connections(
                            hostname.clone(),
                            ClientConnectionsAction::Trust,
                        );
                    }
                    report_session_local(&context, &events, manager);
                }
                return Some(admission(manager.session(), &hostname));
            }
            drop(guard);
            // The driver owns the session while connected. Never use a stale local
            // trust/config cache or overwrite its file from a second owner.
            // Read-only admission follows the current owner's persisted state.
            // The owner saves every permission/trust change before reporting it.
            // Do not turn a busy driver's HTTP loop into lease expiration, and
            // never create a second manager or reuse cached authorization.
            if !register {
                let session = owner_session(&crate::get_filesystem_layout().session())?;
                return Some(admission(&session, &resolve(&session, requested, ip)));
            }
            register_remote(port, requested, ip)
        },
        move |count| save_monitor_count(&save_source, &save_context, &save_events, port, count),
        move |request| super::frame_settings_source::request(&settings_source,&settings_context,&settings_events,port,request),
    )
}

// Keep driver-owned registration on its existing HTTP API for both desktop and
// explicit VR startup. Never create another SessionManager for the same file.
fn register_remote(port: u16, requested: &str, ip: IpAddr) -> Option<(bool, Settings)> {
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_millis(500)))
        .build()
        .into();
    let base = format!("http://127.0.0.1:{port}/api/session");
    let mut session: SessionConfig = agent
        .get(&base)
        .header("X-SURFXR", "true")
        .call()
        .ok()?
        .into_body()
        .read_json()
        .ok()?;
    let hostname = resolve(&session, requested, ip);
    if !session.client_connections.contains_key(&hostname) {
        let auto_trust = matches!(&session.to_settings().connection.client_discovery,
    surfxr_common::settings_schema::Switch::Enabled(c) if c.auto_trust_clients);
        agent
            .post(format!("{base}/client-connections"))
            .header("X-SURFXR", "true")
            .send_json((
                hostname.clone(),
                ClientConnectionsAction::AddIfMissing {
                    trusted: auto_trust,
                    manual_ips: vec![],
                },
            ))
            .ok()?;
        session = agent
            .get(&base)
            .header("X-SURFXR", "true")
            .call()
            .ok()?
            .into_body()
            .read_json()
            .ok()?;
    }
    if crate::pairing::claim(&hostname, admission(&session, &hostname).0) {
        agent
            .post(format!("{base}/client-connections"))
            .header("X-SURFXR", "true")
            .send_json((hostname.clone(), ClientConnectionsAction::Trust))
            .ok()?;
        session = agent
            .get(&base)
            .header("X-SURFXR", "true")
            .call()
            .ok()?
            .into_body()
            .read_json()
            .ok()?;
    }
    Some(admission(&session, &hostname))
}

#[cfg(test)]
mod owner_tests {
    use super::*;
    #[test]
    fn admission_reads_each_owner_change_and_rejects_partial_files() {
        let path =
            std::env::temp_dir().join(format!("surfxr-owner-admission-{}", std::process::id()));
        let file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .unwrap();
        drop(file);
        struct Remove(std::path::PathBuf);
        impl Drop for Remove {
            fn drop(&mut self) {
                std::fs::remove_file(&self.0).ok();
            }
        }
        let _remove = Remove(path.clone());
        let mut s = SessionConfig::default();
        s.session_settings.extra.frame_desktop.allow_input = true;
        std::fs::write(&path, serde_json::to_vec(&s).unwrap()).unwrap();
        assert!(
            owner_session(&path)
                .unwrap()
                .to_settings()
                .extra
                .frame_desktop
                .allow_input
        );
        s.session_settings.extra.frame_desktop.allow_input = false;
        std::fs::write(&path, serde_json::to_vec(&s).unwrap()).unwrap();
        assert!(
            !owner_session(&path)
                .unwrap()
                .to_settings()
                .extra
                .frame_desktop
                .allow_input
        );
        std::fs::write(&path, b"{").unwrap();
        assert!(owner_session(&path).is_none());
        std::fs::write(&path, vec![b' '; 2 * 1024 * 1024 + 1]).unwrap();
        assert!(owner_session(&path).is_none());
    }
}
fn save_monitor_count(
    source: &Mutex<SessionSource>,
    context: &egui::Context,
    events: &mpsc::Sender<PolledEvent>,
    port: u16,
    count: u32,
) -> bool {
    let values = vec![PathValuePair {
        path: [
            "session_settings",
            "extra",
            "frame_desktop",
            "monitor_count",
        ]
        .into_iter()
        .map(|name| PathSegment::Name(name.into()))
        .collect(),
        value: serde_json::json!(count),
    }];
    let mut guard = source.lock();
    if let SessionSource::Local(manager) = &mut *guard {
        let saved = manager.set_session_values(values).is_ok();
        report_session_local(context, events, manager);
        return saved;
    }
    drop(guard);
    let agent: ureq::Agent = ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_millis(500)))
        .build()
        .into();
    agent
        .post(format!("http://127.0.0.1:{port}/api/session/values"))
        .header("X-SURFXR", "true")
        .send_json(values)
        .is_ok()
}
fn resolve(session: &SessionConfig, requested: &str, ip: IpAddr) -> String {
    session
        .client_connections
        .iter()
        .find(|(_, c)| c.manual_ips.contains(&ip))
        .map(|(name, _)| name.clone())
        .unwrap_or_else(|| requested.into())
}
fn admission(session: &SessionConfig, hostname: &str) -> (bool, Settings) {
    (
        session
            .client_connections
            .get(hostname)
            .is_some_and(|c| c.trusted),
        session.to_settings(),
    )
}

pub(super) fn spawn_start(
    running: Arc<RelaxedAtomic>,
    source: Arc<Mutex<SessionSource>>,
    context: egui::Context,
    events: mpsc::Sender<PolledEvent>,
) -> JoinHandle<()> {
    let probe_source = source.clone();
    let status_source = source.clone();
    crate::frame_start::spawn(
        running,
        move |requested, ip| {
            let mut source = source.lock();
            let SessionSource::Local(manager) = &mut *source else {
                drop(source);
                if crate::pairing::enabled() {
                    let trusted = owner_session(&crate::get_filesystem_layout().session())
                        .and_then(|session| {
                            register_remote(
                                session.to_settings().connection.web_server_port,
                                requested,
                                ip,
                            )
                        })
                        .is_some_and(|(trusted, _)| trusted);
                    if !trusted {
                        return crate::frame_start::StartDecision::Untrusted;
                    }
                }
                return crate::frame_start::StartDecision::Ready;
            };
            let hostname = manager
                .client_list()
                .iter()
                .find(|(_, c)| c.manual_ips.contains(&ip))
                .map(|(name, _)| name.clone())
                .unwrap_or_else(|| requested.into());
            let auto_trust = matches!(&manager.settings().connection.client_discovery,
                    surfxr_common::settings_schema::Switch::Enabled(c) if c.auto_trust_clients);
            manager.update_client_connections(
                hostname.clone(),
                surfxr_packets::ClientConnectionsAction::AddIfMissing {
                    trusted: auto_trust,
                    manual_ips: vec![],
                },
            );
            let trusted = manager
                .client_list()
                .get(&hostname)
                .is_some_and(|c| c.trusted);
            if crate::pairing::claim(&hostname, trusted) {
                manager.update_client_connections(hostname.clone(), ClientConnectionsAction::Trust);
            }
            let trusted = manager
                .client_list()
                .get(&hostname)
                .is_some_and(|c| c.trusted);
            report_session_local(&context, &events, manager);
            if trusted {
                crate::frame_start::StartDecision::Launch
            } else {
                crate::frame_start::StartDecision::Untrusted
            }
        },
        move |requested, ip| {
            let guard = status_source.lock();
            if let SessionSource::Local(manager) = &*guard {
                return manager
                    .client_list()
                    .get(&resolve(manager.session(), requested, ip))
                    .is_some_and(|client| client.trusted);
            }
            drop(guard);
            // Status authorization reads current persisted ownership without trust
            // registration, automatic discovery or stale Dashboard permission cache.
            owner_session(&crate::get_filesystem_layout().session()).is_some_and(|session| {
                session
                    .client_connections
                    .get(&resolve(&session, requested, ip))
                    .is_some_and(|client| client.trusted)
            })
        },
        move || {
            let guard = probe_source.lock();
            let port = if let SessionSource::Local(manager) = &*guard {
                Some(manager.settings().connection.web_server_port)
            } else {
                owner_session(&crate::get_filesystem_layout().session())
                    .map(|session| session.to_settings().connection.web_server_port)
            };
            drop(guard);
            port.map(crate::steamvr_launcher::frame_runtime_evidence)
                .unwrap_or_default()
        },
    )
}

#[cfg(test)]
#[path = "frame_pairing_tests.rs"]
mod pairing_tests;
