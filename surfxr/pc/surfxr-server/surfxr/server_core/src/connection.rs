mod restart_config;
mod pipeline;
mod negotiation;
mod audio;
mod control;
mod statistics;
pub use restart_config::compute_restart_settings_hash;
use pipeline::connection_pipeline;
use crate::{
    ConnectionContext, FILESYSTEM_LAYOUT, SESSION_MANAGER, ServerCoreEvent,
    ServerNegotiatedStreamingConfig,
    bitrate::BitrateManager,
    input_mapping::ButtonMappingManager,
    sockets::WelcomeSocket,
    statistics::StatisticsManager,
    tracking::{self, TrackingManager},
    video_sender::VideoSendWorker,
};
use surfxr_common::{
    SurfxrFoveatedEncodingParams, AnyhowToCon, BUTTON_INFO, CONTROLLER_PROFILE_INFO, ConResult,
    ConnectionError, ConnectionState, LifecycleState, QUEST_CONTROLLER_PROFILE_PATH, con_bail,
    dbg_connection, debug, error,
    glam::{UVec2, Vec2},
    info,
    parking_lot::{Condvar, Mutex, RwLock},
    settings_schema::Switch,
    warn,
};
use surfxr_events::{ButtonEvent, EventType};
use surfxr_packets::{
    AUDIO, ClientConnectionResult, ClientConnectionsAction, ClientControlPacket,
    ClientNegotiatedStreamingConfig, ClientStatistics, HAPTICS, RealTimeConfig, STATISTICS,
    ServerControlPacket, StreamConfigPacket, TRACKING, TrackingData, VIDEO,
};
use surfxr_session::{
    BodyTrackingSinkConfig, CodecType, ControllersEmulationMode, FrameSize, H264Profile, Settings,
    SocketProtocol, SteamvrHmdInitConfig,
};
use surfxr_sockets::{
    KEEPALIVE_INTERVAL, KEEPALIVE_TIMEOUT, ProtoControlSocket, SocketConnection,
    StreamSocketConfig,
};
use std::{
    collections::{HashMap, hash_map::DefaultHasher},
    hash::{Hash, Hasher},
    net::{IpAddr},
    process::Command,
    sync::Arc,
    thread,
    time::{Duration, Instant},
};

const RETRY_CONNECT_MIN_INTERVAL: Duration = Duration::from_secs(1);
const HANDSHAKE_ACTION_TIMEOUT: Duration = Duration::from_secs(2);
pub const STREAMING_RECV_TIMEOUT: Duration = Duration::from_millis(500);
const REAL_TIME_UPDATE_INTERVAL: Duration = Duration::from_secs(1);

const MAX_UNREAD_PACKETS: usize = 10; // Applies per stream

fn align32(value: f32) -> u32 {
    ((value / 32.).floor() * 32.) as u32
}

fn is_streaming(client_hostname: &str) -> bool {
    SESSION_MANAGER
        .read()
        .client_list()
        .get(client_hostname)
        .is_some_and(|c| c.connection_state == ConnectionState::Streaming)
}

/// Align one center coordinate using the same rule for static and gaze-driven foveation.
pub fn align_foveation_center_shift(center_shift: f32, edge_size: f32, edge_ratio: f32) -> f32 {
    if !center_shift.is_finite()
        || !edge_size.is_finite()
        || !edge_ratio.is_finite()
        || edge_size <= 0.0
        || edge_ratio <= 0.0
    {
        return 0.0;
    }

    let step = edge_ratio * 2.0 / edge_size;
    if !step.is_finite() || step >= 1.0 {
        return 0.0;
    }

    // Reserve one alignment step on each edge to avoid singular inverse coefficients.
    // Do not replace this with division by `step`: f32 rounding can change the ceiling.
    let aligned =
        (center_shift * edge_size / (edge_ratio * 2.0)).ceil() * (edge_ratio * 2.0) / edge_size;

    aligned.clamp(-1.0 + step, 1.0 - step)
}

// Alternate connection trials with manual IPs and clients discovered on the local network
pub fn handshake_loop(ctx: Arc<ConnectionContext>, lifecycle_state: Arc<RwLock<LifecycleState>>) {
    dbg_connection!("handshake_loop: Begin");
    let welcome_socket = match WelcomeSocket::new() {
        Ok(socket) => socket,
        Err(e) => {
            error!("Failed to create discovery socket: {e:?}");
            return;
        }
    };
    let frame_thread = crate::frame_connect::spawn(Arc::clone(&ctx), Arc::clone(&lifecycle_state));

    while *lifecycle_state.read() != LifecycleState::ShuttingDown {
        dbg_connection!("handshake_loop: Try connect to manual IPs");

        let available_manual_client_ips = {
            let mut manual_client_ips = HashMap::new();
            for (hostname, connection_info) in
                SESSION_MANAGER
                    .read()
                    .client_list()
                    .iter()
                    .filter(|(_, info)| {
                        info.connection_state == ConnectionState::Disconnected

                    })
            {
                for ip in &connection_info.manual_ips {
                    manual_client_ips.insert(*ip, hostname.clone());
                }
            }
            manual_client_ips
        };

        if !available_manual_client_ips.is_empty()
            && try_connect(
                Arc::clone(&ctx),
                Arc::clone(&lifecycle_state),
                available_manual_client_ips,
            )
            .is_ok()
        {
            thread::sleep(RETRY_CONNECT_MIN_INTERVAL);
            continue;
        }

        let discovery_config = SESSION_MANAGER
            .read()
            .settings()
            .connection
            .client_discovery
            .clone();
        if let Switch::Enabled(config) = discovery_config {
            dbg_connection!("handshake_loop: Discovering clients");

            let clients = match welcome_socket.recv_all() {
                Ok(clients) => clients,
                Err(e) => {
                    warn!("mDNS listening error: {e:?}");

                    thread::sleep(RETRY_CONNECT_MIN_INTERVAL);
                    continue;
                }
            };

            if clients.is_empty() {
                thread::sleep(RETRY_CONNECT_MIN_INTERVAL);
                continue;
            }

            for (client_hostname, client_ip) in clients {
                let trusted = {
                    let mut session_manager = SESSION_MANAGER.write();

                    session_manager.update_client_connections(
                        client_hostname.clone(),
                        ClientConnectionsAction::AddIfMissing {
                            trusted: false,
                            manual_ips: vec![],
                        },
                    );

                    if config.auto_trust_clients {
                        session_manager.update_client_connections(
                            client_hostname.clone(),
                            ClientConnectionsAction::Trust,
                        );
                    }

                    session_manager
                        .client_list()
                        .get(&client_hostname)
                        .is_some_and(|c| c.trusted)
                };

                // do not attempt connection if the client is already connected
                if trusted
                    && SESSION_MANAGER
                        .read()
                        .client_list()
                        .get(&client_hostname)
                        .is_some_and(|c| c.connection_state == ConnectionState::Disconnected)
                    && let Err(e) = try_connect(
                        Arc::clone(&ctx),
                        Arc::clone(&lifecycle_state),
                        [(client_ip, client_hostname.clone())].into_iter().collect(),
                    )
                {
                    error!("Could not initiate connection for {client_hostname}: {e}");
                }

                thread::sleep(RETRY_CONNECT_MIN_INTERVAL);
            }
        } else {
            thread::sleep(RETRY_CONNECT_MIN_INTERVAL);
        }
    }

    surfxr_common::dbg_connection!("handshake_loop: Joining connection threads");

    // At this point, LIFECYCLE_STATE == ShuttingDown, so all threads are already terminating
    if let Some(thread) = frame_thread { thread.join().ok(); }
    for thread in ctx.connection_threads.lock().drain(..) {
        thread.join().ok();
    }

    surfxr_common::dbg_connection!("handshake_loop: End");
}

fn try_connect(
    ctx: Arc<ConnectionContext>,
    lifecycle_state: Arc<RwLock<LifecycleState>>,
    mut client_ips: HashMap<IpAddr, String>,
) -> ConResult {
    dbg_connection!("try_connect: Finding client and creating control socket");

    let (socket, client_ip, connection_result) = surfxr_sockets::connect_to_client(
        client_ips.keys().cloned().collect(),
        Duration::from_secs(1),
    )?;

    let Some(client_hostname) = client_ips.remove(&client_ip) else {
        con_bail!("unreachable");
    };

    spawn_connection(ctx, lifecycle_state, socket, connection_result, client_hostname, client_ip);
    Ok(())
}

pub(crate) fn spawn_connection(
    ctx: Arc<ConnectionContext>, lifecycle_state: Arc<RwLock<LifecycleState>>,
    socket: ProtoControlSocket, connection_result: ClientConnectionResult,
    client_hostname: String, client_ip: IpAddr,
) {
    ctx.connection_threads.lock().push(thread::spawn({
        let ctx = Arc::clone(&ctx);
        move || {
            if let Err(e) = connection_pipeline(
                Arc::clone(&ctx),
                lifecycle_state,
                socket,
                connection_result,
                client_hostname.clone(),
                client_ip,
            ) {
                error!("Handshake error for {client_hostname}: {e}");
            }
            ctx.frame_sessions.remove(&client_hostname);

            let mut clients_to_be_removed = ctx.clients_to_be_removed.lock();

            let action = if clients_to_be_removed.contains(&client_hostname) {
                clients_to_be_removed.remove(&client_hostname);

                ClientConnectionsAction::RemoveEntry
            } else {
                ClientConnectionsAction::SetConnectionState(ConnectionState::Disconnected)
            };
            SESSION_MANAGER
                .write()
                .update_client_connections(client_hostname, action);
        }
    }));

}
