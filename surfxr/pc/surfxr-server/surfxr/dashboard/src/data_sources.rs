#[path = "session_owner.rs"]
mod session_owner;
#[path = "frame_settings_source.rs"]
mod frame_settings_source;
use crate::dashboard::ServerRequest;
#[path = "frame_desktop_source.rs"]
mod frame_desktop_source;
use surfxr_common::{
    semver::Version, SURFXR_VERSION, RelaxedAtomic, debug, error, info,
    parking_lot::Mutex,
    warn,
};
use surfxr_events::{Event, EventType};
use surfxr_packets::FirewallRulesAction;
use surfxr_server_io::ServerSessionManager;
use eframe::egui;
use serde::Serialize;
use std::{
    io::ErrorKind,
    net::{SocketAddr, TcpStream},
    str::FromStr,
    sync::{Arc, mpsc},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use tungstenite::{
    client::IntoClientRequest,
    http::{HeaderValue, Uri},
};

const LOCAL_REQUEST_TIMEOUT: Duration = Duration::from_millis(200);

enum SessionSource {
    Local(Box<ServerSessionManager>),
    Remote, // Note: the remote (server) is probably living as a separate process in the same PC
}

fn get_local_session_source() -> ServerSessionManager {
    let session_file_path = crate::get_filesystem_layout().session();
    ServerSessionManager::new(Some(session_file_path))
}

pub fn clean_session() {
    let mut session_manager = get_local_session_source();

    session_manager.clean_client_list();

    if session_manager.session().server_version != *SURFXR_VERSION {
        let mut session_ref = session_manager.session_mut();
        session_ref.server_version = SURFXR_VERSION.clone();
        session_ref.client_connections.clear();
        session_ref.session_settings.extra.open_setup_wizard = true;
        session_ref
            .session_settings
            .extra
            .new_version_popup
            .content
            .hide_while_version = SURFXR_VERSION.to_string();
    }
}

// Disallows all methods for mutating (and overwriting to disk) the session
pub fn get_read_only_local_session() -> Arc<ServerSessionManager> {
    Arc::new(get_local_session_source())
}

fn report_event_local(
    context: &egui::Context,
    sender: &mpsc::Sender<PolledEvent>,
    event_type: EventType,
) {
    sender
        .send(PolledEvent {
            inner: Event {
                timestamp: "".into(),
                event_type,
            },
            from_dashboard: false,
        })
        .ok();
    context.request_repaint();
}

fn report_session_local(
    context: &egui::Context,
    sender: &mpsc::Sender<PolledEvent>,
    session_manager: &ServerSessionManager,
) {
    report_event_local(
        context,
        sender,
        EventType::Session(Box::new(session_manager.session().clone())),
    )
}

pub struct PolledEvent {
    pub inner: Event,
    pub from_dashboard: bool,
}

pub struct DataSources {
    running: Arc<RelaxedAtomic>,
    requests_sender: mpsc::Sender<ServerRequest>,
    events_receiver: mpsc::Receiver<PolledEvent>,
    server_connected: Arc<RelaxedAtomic>,
    requests_thread: Option<JoinHandle<()>>,
    events_thread: Option<JoinHandle<()>>,
    ping_thread: Option<JoinHandle<()>>,
    frame_start_thread: Option<JoinHandle<()>>,
    frame_desktop_thread: Option<JoinHandle<()>>,
}

impl DataSources {
    pub fn new(
        context: egui::Context,
        events_sender: mpsc::Sender<PolledEvent>,
        events_receiver: mpsc::Receiver<PolledEvent>,
    ) -> Self {
        let filesystem_layout = crate::get_filesystem_layout();

        let running = Arc::new(RelaxedAtomic::new(true));
        let (requests_sender, requests_receiver) = mpsc::channel();
        let server_connected = Arc::new(RelaxedAtomic::new(false));

        let session_manager = get_local_session_source();
        let port = session_manager.settings().connection.web_server_port;
        let session_source = Arc::new(Mutex::new(SessionSource::Local(Box::new(session_manager))));
        let frame_desktop_thread = frame_desktop_source::spawn(
            Arc::clone(&running),
            Arc::clone(&session_source),
            context.clone(),
            events_sender.clone(),
            port,
        );
        let frame_start_thread = frame_desktop_source::spawn_start(
            Arc::clone(&running),
            Arc::clone(&session_source),
            context.clone(),
            events_sender.clone(),
        );

        let requests_thread = thread::spawn({
            let running = Arc::clone(&running);
            let context = context.clone();
            let session_source = Arc::clone(&session_source);
            let events_sender = events_sender.clone();
            move || {
                let base_uri = format!("http://127.0.0.1:{port}");
                let rq: ureq::Agent = ureq::Agent::config_builder()
                    .timeout_global(Some(LOCAL_REQUEST_TIMEOUT))
                    .build()
                    .into();

                while running.value() {
                    while let Ok(request) = requests_receiver.try_recv() {
                        debug!(
                            "Dashboard request: {}",
                            serde_json::to_string(&request).unwrap()
                        );

                        if let SessionSource::Local(session_manager) = &mut *session_source.lock() {
                            match request {
                                ServerRequest::Log(_) => (),
                                ServerRequest::GetSession => {
                                    report_session_local(&context, &events_sender, session_manager);
                                }
                                ServerRequest::UpdateSession(session) => {
                                    *session_manager.session_mut() = *session;

                                    report_session_local(&context, &events_sender, session_manager);
                                }
                                ServerRequest::SetSessionValues(values) => {
                                    if let Err(e) = session_manager.set_session_values(values) {
                                        error!("Failed to set session value: {e}")
                                    }

                                    report_session_local(&context, &events_sender, session_manager);
                                }
                                ServerRequest::UpdateClientList { hostname, action } => {
                                    session_manager.update_client_connections(hostname, action);

                                    report_session_local(&context, &events_sender, session_manager);
                                }
                                ServerRequest::AddFirewallRules => {
                                    if let Err(e) = surfxr_server_io::firewall_rules(
                                        FirewallRulesAction::Add,
                                        &filesystem_layout,
                                    ) {
                                        error!("Failed to add firewall rules! code: {e}");
                                    } else {
                                        info!("Successfully added firewall rules!");
                                    }
                                }
                                ServerRequest::RemoveFirewallRules => {
                                    if let Err(e) = surfxr_server_io::firewall_rules(
                                        FirewallRulesAction::Remove,
                                        &filesystem_layout,
                                    ) {
                                        error!("Failed to remove firewall rules! code: {e}");
                                    } else {
                                        info!("Successfully removed firewall rules!");
                                    }
                                }
                                ServerRequest::RegisterSurfxrDriver => {
                                    let surfxr_driver_dir =
                                        filesystem_layout.openvr_driver_root_dir.clone();

                                    surfxr_server_io::driver_registration(&[surfxr_driver_dir], true)
                                        .ok();

                                    if let Ok(list) = surfxr_server_io::get_registered_drivers() {
                                        report_event_local(
                                            &context,
                                            &events_sender,
                                            EventType::DriversList(list),
                                        )
                                    }
                                }
                                ServerRequest::UnregisterDriver(path) => {
                                    surfxr_server_io::driver_registration(&[path], false).ok();

                                    if let Ok(list) = surfxr_server_io::get_registered_drivers() {
                                        report_event_local(
                                            &context,
                                            &events_sender,
                                            EventType::DriversList(list),
                                        )
                                    }
                                }
                                ServerRequest::GetDriverList => {
                                    if let Ok(list) = surfxr_server_io::get_registered_drivers() {
                                        report_event_local(
                                            &context,
                                            &events_sender,
                                            EventType::DriversList(list),
                                        )
                                    }
                                }
                                ServerRequest::InsertIdr
                                | ServerRequest::StartRecording
                                | ServerRequest::StopRecording => {
                                    warn!(
                                        "Cannot perform action, streamer (SteamVR) is not connected."
                                    )
                                }
                                ServerRequest::RestartSteamvr | ServerRequest::ShutdownSteamvr => {
                                    warn!("Streamer not launched, can't signal SteamVR shutdown")
                                }
                            }
                        } else {
                            let get = |path: &str| {
                                rq.get(format!("{base_uri}/api/{path}"))
                                    .header("X-SURFXR", "true")
                                    .call()
                                    .ok();
                            };

                            fn post_body(
                                rq: &ureq::Agent,
                                base_uri: &str,
                                path: &str,
                                body: Option<impl Serialize>,
                            ) {
                                let builder = rq
                                    .post(format!("{base_uri}/api/{path}"))
                                    .header("X-SURFXR", "true");
                                if let Some(body) = body {
                                    builder.send_json(body).ok();
                                } else {
                                    builder.send_empty().ok();
                                }
                            }
                            let post = |path: &str| post_body(&rq, &base_uri, path, None::<()>);

                            match request {
                                ServerRequest::Log(entry) => {
                                    post_body(&rq, &base_uri, "log", Some(entry))
                                }
                                ServerRequest::GetSession => get("session"),
                                ServerRequest::UpdateSession(session) => {
                                    post_body(&rq, &base_uri, "session", Some(&*session))
                                }
                                ServerRequest::SetSessionValues(values) => {
                                    post_body(&rq, &base_uri, "session/values", Some(values))
                                }
                                ServerRequest::UpdateClientList { hostname, action } => post_body(
                                    &rq,
                                    &base_uri,
                                    "session/client-connections",
                                    Some((hostname, action)),
                                ),
                                ServerRequest::AddFirewallRules => post("firewall-rules/add"),
                                ServerRequest::RemoveFirewallRules => post("firewall-rules/remove"),
                                ServerRequest::GetDriverList => get("drivers"),
                                ServerRequest::RegisterSurfxrDriver => post("drivers/register-surfxr"),
                                ServerRequest::UnregisterDriver(path) => {
                                    post_body(&rq, &base_uri, "drivers/unregister", Some(path))
                                }
                                ServerRequest::InsertIdr => post("insert-idr"),
                                ServerRequest::StartRecording => post("recording/start"),
                                ServerRequest::StopRecording => post("recording/stop"),
                                ServerRequest::RestartSteamvr => post("steamvr/restart"),
                                ServerRequest::ShutdownSteamvr => post("steamvr/shutdown"),
                            }
                        }
                    }

                    thread::sleep(Duration::from_millis(100));
                }
            }
        });

        let events_thread = thread::spawn({
            let context = context.clone();
            let events_sender = events_sender.clone();
            let running = Arc::clone(&running);
            let session_source = Arc::clone(&session_source);
            move || {
                while running.value() {
                    if matches!(*session_source.lock(), SessionSource::Local(_)) {
                        thread::sleep(Duration::from_millis(100));

                        continue;
                    }

                    let uri = Uri::from_str(&format!("ws://127.0.0.1:{port}/api/events")).unwrap();

                    let maybe_socket = TcpStream::connect_timeout(
                        &SocketAddr::from_str(&format!("127.0.0.1:{port}")).unwrap(),
                        Duration::from_millis(500),
                    );
                    let Ok(socket) = maybe_socket else {
                        thread::sleep(Duration::from_millis(500));

                        continue;
                    };

                    let mut req = uri.into_client_request().unwrap();
                    req.headers_mut()
                        .insert("X-SURFXR", HeaderValue::from_str("true").unwrap());

                    let Ok((mut ws, _)) = tungstenite::client(req, socket) else {
                        thread::sleep(Duration::from_millis(500));

                        continue;
                    };

                    ws.get_mut().set_nonblocking(true).ok();

                    while running.value() {
                        match ws.read() {
                            Ok(tungstenite::Message::Text(json_string)) => {
                                debug!("Server event: {json_string}");
                                if let Ok(event) = serde_json::from_str::<Event>(&json_string) {
                                    if let EventType::Log(entry) = &event.event_type {
                                        if !crate::logging_backend::accepts(entry) { continue; }
                                    }
                                    events_sender
                                        .send(PolledEvent {
                                            inner: event,
                                            from_dashboard: false,
                                        })
                                        .ok();
                                    context.request_repaint();
                                }
                            }
                            Err(e) => {
                                if let tungstenite::Error::Io(e) = e
                                    && e.kind() == ErrorKind::WouldBlock
                                {
                                    thread::sleep(Duration::from_millis(50));

                                    continue;
                                }

                                break;
                            }
                            _ => (),
                        }
                    }
                }
            }
        });

        let ping_thread = session_owner::spawn(Arc::clone(&running), Arc::clone(&session_source),
            Arc::clone(&server_connected), context, events_sender, port);

        Self {
            requests_sender,
            events_receiver,
            server_connected,
            running,
            requests_thread: Some(requests_thread),
            events_thread: Some(events_thread),
            ping_thread: Some(ping_thread),
            frame_start_thread: Some(frame_start_thread),
            frame_desktop_thread: Some(frame_desktop_thread),
        }
    }

    pub fn request(&self, request: ServerRequest) {
        self.requests_sender.send(request).ok();
    }

    pub fn poll_event(&self) -> Option<PolledEvent> {
        self.events_receiver.try_recv().ok()
    }

    pub fn server_connected(&self) -> bool {
        self.server_connected.value()
    }
}

impl Drop for DataSources {
    fn drop(&mut self) {
        crate::frame_desktop::sessions::shutdown();
        self.running.set(false);

        self.requests_thread.take().unwrap().join().ok();
        self.events_thread.take().unwrap().join().ok();
        self.ping_thread.take().unwrap().join().ok();
        self.frame_start_thread.take().unwrap().join().ok();
        self.frame_desktop_thread.take().unwrap().join().ok();
    }
}
