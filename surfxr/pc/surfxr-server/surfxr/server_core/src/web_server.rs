use crate::{
    ConnectionContext, FILESYSTEM_LAYOUT, SESSION_MANAGER, ServerCoreEvent,
    logging_backend::EVENTS_SENDER,
};
use surfxr_common::{ConnectionState, LogEntry, anyhow::Result, error, info, log};
use surfxr_events::{ButtonEvent, EventType};
use surfxr_packets::{ButtonEntry, ClientConnectionsAction, FirewallRulesAction, PathValuePair};
use surfxr_session::SessionConfig;
use axum::{
    Json, Router,
    extract::{Request, State, WebSocketUpgrade, ws::Message},
    http::{
        HeaderValue, Method, StatusCode,
        header::{CACHE_CONTROL, CONTENT_TYPE},
    },
    middleware,
    response::Response,
    routing,
};
use serde_json as json;
use std::{
    net::{Ipv4Addr, SocketAddr},
    path::PathBuf,
    sync::Arc,
};
use tokio::net::TcpListener;
use tower_http::{
    cors::{self, CorsLayer},
    set_header::SetResponseHeaderLayer,
};

const X_SURFXR: &str = "X-SURFXR";

// This is the actual core part of cors
// We require the X-SURFXR header, but the browser forces a cors preflight
// if the site tries to send a request with it set since it's not-whitelisted
//
// The dashboard can just set the header and be allowed through without the preflight
// thus not getting blocked by allow_untrusted_http being disabled
async fn ensure_preflight(request: Request, next: middleware::Next) -> Response {
    if request.headers().contains_key(X_SURFXR) || request.method() == Method::OPTIONS {
        next.run(request).await
    } else {
        Response::builder()
            .status(StatusCode::BAD_REQUEST)
            .body(format!("missing {X_SURFXR} header").into())
            .unwrap()
    }
}

pub async fn web_server(connection_context: Arc<ConnectionContext>) -> Result<()> {
    let allow_untrusted_http;
    let allow_remote_dashboard;
    let web_server_port;

    {
        let session_manager = SESSION_MANAGER.read();
        allow_untrusted_http = session_manager.settings().connection.allow_untrusted_http;
        allow_remote_dashboard = session_manager.settings().connection.allow_remote_dashboard;
        web_server_port = session_manager.settings().connection.web_server_port;
    }

    let bind_address = if allow_remote_dashboard {
        Ipv4Addr::UNSPECIFIED
    } else {
        Ipv4Addr::LOCALHOST
    };

    let mut cors = CorsLayer::new()
        .allow_methods([Method::GET, Method::POST])
        .allow_headers([CONTENT_TYPE, X_SURFXR.parse().unwrap()]);
    if allow_untrusted_http {
        cors = cors.allow_origin(cors::Any);
    }

    let router = Router::new()
        .nest(
            "/api",
            Router::new()
                .route("/events", routing::get(events_websocket))
                .route("/log", routing::post(set_log))
                .nest(
                    "/session",
                    Router::new()
                        .route("/", routing::get(get_session).post(update_session))
                        .route("/values", routing::post(set_session_values))
                        .route("/frame-settings", routing::post(frame_settings))
                        .route(
                            "/client-connections",
                            routing::post(update_client_connections),
                        ),
                )
                .route("/buttons", routing::post(set_buttons))
                .route("/insert-idr", routing::post(insert_idr))
                .nest(
                    "/recording",
                    Router::new()
                        .route("/start", routing::post(start_recording))
                        .route("/stop", routing::post(stop_recording)),
                )
                .nest(
                    "/firewall-rules",
                    Router::new()
                        .route("/add", routing::post(add_firewall_rules))
                        .route("/remove", routing::post(remove_firewall_rules)),
                )
                .nest(
                    "/drivers",
                    Router::new()
                        .route("/", routing::get(get_driver_list))
                        .route("/register-surfxr", routing::post(register_surfxr_driver))
                        .route("/unregister", routing::post(unregister_driver)),
                )
                .nest(
                    "/steamvr",
                    Router::new()
                        .route("/restart", routing::post(restart_steamvr))
                        .route("/shutdown", routing::post(shutdown_steamvr)),
                )
                .route(
                    "/version",
                    routing::get(async || surfxr_common::SURFXR_VERSION.to_string()),
                )
                .route("/frame-desktop-tracking", routing::get(async || Json(crate::frame_desktop_tracking::snapshot())))
                .route("/ping", routing::get(async || ())),
        )
        .layer(cors)
        .layer(SetResponseHeaderLayer::overriding(
            CACHE_CONTROL,
            HeaderValue::from_static("no-cache, no-store, must-revalidate"),
        ))
        .layer(middleware::from_fn(ensure_preflight))
        .with_state(connection_context);

    axum::serve(
        TcpListener::bind(SocketAddr::new(bind_address.into(), web_server_port))
            .await
            .unwrap(),
        router,
    )
    .await?;

    Ok(())
}

async fn events_websocket(ws: WebSocketUpgrade) -> Response {
    ws.on_upgrade(async |mut ws| {
        let mut events_receiver = crate::restart_events::RESTART.subscribe(&EVENTS_SENDER);
        while let Some(event) = events_receiver.next().await {
            if let Err(e) = ws
                .send(Message::Text(json::to_string(&event).unwrap().into()))
                .await
            {
                info!("Failed to send event with websocket: {e}");
                break;
            }
        }
    })
}

async fn set_log(Json(entry): Json<LogEntry>) {
    let level = entry.severity.into_log_level();
    log::log!(level, "{}", entry.content);
}

async fn get_session() -> Json<SessionConfig> {
    let session = SESSION_MANAGER.read().session().clone();
    // Existing dashboards consume the event. Desktop admission also needs a
    // synchronous snapshot from the driver while it owns the trust/settings.
    surfxr_events::send_event(EventType::Session(Box::new(session.clone())));
    Json(session)
}

async fn update_session(Json(config): Json<SessionConfig>) {
    let mut manager = SESSION_MANAGER.write();
    *manager.session_mut() = config;
    crate::logging_backend::set_level(manager.settings().extra.logging.log_level);
}

async fn set_session_values(Json(descs): Json<Vec<PathValuePair>>) {
    let mut manager=SESSION_MANAGER.write();
    if let Err(e) = manager.set_session_values(descs) {
        error!("Failed to set session values: {e}");
        return;
    }
    crate::logging_backend::set_level(manager.settings().extra.logging.log_level);
    drop(manager);
}

async fn frame_settings(
    State(ctx): State<Arc<ConnectionContext>>,
    Json(request): Json<surfxr_session::frame_settings::FrameSettingsRequest>,
) -> Result<Json<surfxr_session::frame_settings::FrameSettingsSnapshot>, axum::http::StatusCode> {
    let mut manager=SESSION_MANAGER.write();
    let (snapshot,exit)=manager.frame_request(&request).map_err(|_|axum::http::StatusCode::BAD_REQUEST)?;
    drop(manager);
    if exit { ctx.events_sender.send(ServerCoreEvent::ShutdownPending).ok(); }
    Ok(Json(snapshot))
}

async fn update_client_connections(
    State(ctx): State<Arc<ConnectionContext>>,
    Json((hostname, mut action)): Json<(String, ClientConnectionsAction)>,
) {
    let mut session_manager = SESSION_MANAGER.write();
    if matches!(action, ClientConnectionsAction::RemoveEntry)
        && let Some(entry) = session_manager.client_list().get(&hostname)
        && entry.connection_state != ConnectionState::Disconnected
    {
        ctx.clients_to_be_removed.lock().insert(hostname.clone());

        action = ClientConnectionsAction::SetConnectionState(ConnectionState::Disconnecting);
    }

    session_manager.update_client_connections(hostname, action);
}

async fn insert_idr(State(ctx): State<Arc<ConnectionContext>>) {
    ctx.events_sender.send(ServerCoreEvent::RequestIDR).ok();
}

async fn start_recording(State(ctx): State<Arc<ConnectionContext>>) {
    crate::create_recording_file(&ctx, crate::SESSION_MANAGER.read().settings())
}

async fn stop_recording(State(ctx): State<Arc<ConnectionContext>>) {
    *ctx.video_recording_file.lock() = None;
}

async fn add_firewall_rules() {
    if let Err(e) =
        surfxr_server_io::firewall_rules(FirewallRulesAction::Add, FILESYSTEM_LAYOUT.get().unwrap())
    {
        error!("Failed to add firewall rules! code: {e}");
    } else {
        info!("Successfully added firewall rules!");
    }
}

async fn remove_firewall_rules() {
    if let Err(e) = surfxr_server_io::firewall_rules(
        FirewallRulesAction::Remove,
        FILESYSTEM_LAYOUT.get().unwrap(),
    ) {
        error!("Failed to remove firewall rules! code: {e}");
    } else {
        info!("Successfully removed firewall rules!");
    }
}

async fn get_driver_list() {
    if let Ok(list) = surfxr_server_io::get_registered_drivers() {
        surfxr_events::send_event(EventType::DriversList(list));
    }
}

async fn register_surfxr_driver() {
    surfxr_server_io::driver_registration(
        &[FILESYSTEM_LAYOUT
            .get()
            .unwrap()
            .openvr_driver_root_dir
            .clone()],
        true,
    )
    .ok();

    if let Ok(list) = surfxr_server_io::get_registered_drivers() {
        surfxr_events::send_event(EventType::DriversList(list));
    }
}

async fn unregister_driver(Json(path): Json<PathBuf>) {
    surfxr_server_io::driver_registration(&[path], false).ok();

    if let Ok(list) = surfxr_server_io::get_registered_drivers() {
        surfxr_events::send_event(EventType::DriversList(list));
    }
}

async fn restart_steamvr(State(ctx): State<Arc<ConnectionContext>>) {
    ctx.frame_sessions.restarting();
    ctx.events_sender.send(ServerCoreEvent::RestartPending).ok();
}

async fn shutdown_steamvr(State(ctx): State<Arc<ConnectionContext>>) {
    ctx.events_sender
        .send(ServerCoreEvent::ShutdownPending)
        .ok();
}

async fn set_buttons(
    State(ctx): State<Arc<ConnectionContext>>,
    Json(button_events): Json<Vec<ButtonEvent>>,
) {
    let button_entries = button_events
        .iter()
        .map(|b| ButtonEntry {
            path_id: surfxr_common::hash_string(&b.path),
            value: b.value,
        })
        .collect();

    ctx.events_sender
        .send(ServerCoreEvent::Buttons(button_entries))
        .ok();
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    #[tokio::test]
    async fn session_get_returns_driver_snapshot_over_http() {
        // Exercise the production handler on a reserved endpoint. No driver,
        // desktop host, user session file or live SURFXR port is opened.
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
        let address = listener.local_addr().unwrap();
        let router = Router::new()
            .route("/api/session", routing::get(get_session))
            .layer(middleware::from_fn(ensure_preflight));
        let server = tokio::spawn(async move { axum::serve(listener, router).await.unwrap() });
        let mut socket = tokio::net::TcpStream::connect(address).await.unwrap();
        socket.write_all(b"GET /api/session HTTP/1.1\r\nHost: localhost\r\nX-SURFXR: true\r\nConnection: close\r\n\r\n").await.unwrap();
        let mut response = Vec::new();
        tokio::time::timeout(std::time::Duration::from_secs(2), socket.read_to_end(&mut response))
            .await.unwrap().unwrap();
        server.abort();
        let body = response.windows(4).position(|s| s == b"\r\n\r\n").unwrap() + 4;
        assert!(response.starts_with(b"HTTP/1.1 200"));
        let session: SessionConfig = json::from_slice(&response[body..]).unwrap();
        assert_eq!(json::to_value(&session).unwrap(),
            json::to_value(SESSION_MANAGER.read().session()).unwrap());
    }
}
