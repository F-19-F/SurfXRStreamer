//! Driver ownership and periodic recovery from missed session events.
use super::*;
pub(super) fn spawn(running: Arc<RelaxedAtomic>, session_source: Arc<Mutex<SessionSource>>,
    server_connected: Arc<RelaxedAtomic>, context: egui::Context,
    events_sender: mpsc::Sender<PolledEvent>, port: u16) -> JoinHandle<()> {
    thread::spawn(move || {
        const PING_INTERVAL: Duration = Duration::from_secs(1);
        let mut deadline = Instant::now();
        let mut remote_owner=None;
        let uri = format!("http://127.0.0.1:{port}/api/version");

        let request_agent: ureq::Agent = ureq::Agent::config_builder()
            .timeout_global(Some(LOCAL_REQUEST_TIMEOUT))
            .build()
            .into();

        loop {
            let maybe_server_version = request_agent
                .get(&uri)
                .header("X-SURFXR", "true")
                .call()
                .ok()
                .and_then(|r| {
                    Version::from_str(&r.into_body().read_to_string().ok()?).ok()
                });

            let connected = if let Some(version) = maybe_server_version {
                // We need exact match because we don't do session extrapolation at the
                // dashboard level. In the future we may relax the contraint and consider
                // protocol compatibility check for dashboard.
                let matches = version == *surfxr_common::SURFXR_VERSION;

                if !matches {
                    error!(
                        "Server version mismatch: found {version}. Please remove all previous SURFXR installations"
                    );
                }

                matches
            } else {
                false
            };

            let owner_now=crate::steamvr_launcher::steamvr_identity();
            if connected {remote_owner=owner_now;}
            let owner_alive=remote_owner.is_some()&&owner_now==remote_owner;
            {
                let mut session_source_lock = session_source.lock();
                if connected && matches!(*session_source_lock, SessionSource::Local(_)) {
                    info!("Server connected");
                    *session_source_lock = SessionSource::Remote;
                } else if !connected
                    && matches!(*session_source_lock, SessionSource::Remote)
                    && !owner_alive
                {
                    info!("Server disconnected");
                    *session_source_lock =
                        SessionSource::Local(Box::new(get_local_session_source()));
                }
            }

            // Websocket delivery can miss the owner handoff or an edit.
            // Periodically refresh from the persisted authority, never write UI echoes.
            if connected {
                if let Some(session) = request_agent.get(format!("http://127.0.0.1:{port}/api/session"))
                    .header("X-SURFXR", "true").call().ok()
                    .and_then(|r| r.into_body().read_json::<surfxr_session::SessionConfig>().ok()) {
                    report_event_local(&context, &events_sender, EventType::Session(Box::new(session)));
                }
            }

            // A busy HTTP/event loop is not proof of driver exit. Do
            // not create a second session writer while its owner lives.
            server_connected.set(connected||owner_alive);

            deadline += PING_INTERVAL;

            while Instant::now() < deadline {
                if !running.value() {
                    return;
                }
                thread::sleep(Duration::from_millis(100));
            }
        }
    })
}
