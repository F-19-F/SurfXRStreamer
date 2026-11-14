//! Frame initiates discovery and reliable control; legacy SURFXR clients keep mDNS/dial-out.
use crate::{ConnectionContext, SESSION_MANAGER, connection};
use surfxr_common::{
    ConnectionState, LifecycleState, parking_lot::RwLock, settings_schema::Switch, warn,
};
use surfxr_packets::{ClientConnectionResult, ClientConnectionsAction};
use surfxr_server_io::ServerSessionManager;
use surfxr_sockets::{CONTROL_PORT, ProtoControlSocket};
use std::{
    net::{IpAddr, Ipv4Addr, TcpListener, UdpSocket},
    sync::Arc,
    thread::{self, JoinHandle},
    time::Duration,
};

fn resolve_client(manager: &mut ServerSessionManager, ip: IpAddr, caps: &str) -> Option<String> {
    let ext: serde_json::Value = serde_json::from_str(caps).ok()?;
    if ext["frame_client_connect"].as_u64() != Some(1) {
        return None;
    }
    let requested = ext["frame_client_hostname"].as_str()?;
    let origin=crate::frame_origin::ip(caps,ip);
    if requested.is_empty()
        || requested.len() > 63
        || !requested
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
    {
        return None;
    }
    // Reuse an existing manual registration, including its trust decision.
    let hostname = manager
        .client_list()
        .iter()
        .find(|(_, c)| c.manual_ips.contains(&origin))
        .map(|(name, _)| name.clone())
        .unwrap_or_else(|| requested.into());
    let auto_trust = matches!(&manager.settings().connection.client_discovery,
        Switch::Enabled(config) if config.auto_trust_clients);
    manager.update_client_connections(
        hostname.clone(),
        ClientConnectionsAction::AddIfMissing {
            trusted: auto_trust,
            manual_ips: vec![],
        },
    );
    let client = manager.client_list().get(&hostname)?;
    if !client.trusted || client.connection_state != ConnectionState::Disconnected {
        return None;
    }
    // Reserve the entry before another incoming/outgoing handshake can claim it.
    manager.update_client_connections(
        hostname.clone(),
        ClientConnectionsAction::SetConnectionState(ConnectionState::Connecting),
    );
    manager.update_client_connections(hostname.clone(),ClientConnectionsAction::UpdateCurrentIp(Some(origin)));
    Some(hostname)
}

pub(crate) fn spawn(
    ctx: Arc<ConnectionContext>,
    state: Arc<RwLock<LifecycleState>>,
) -> Option<JoinHandle<()>> {
    let listener = match TcpListener::bind((Ipv4Addr::UNSPECIFIED, CONTROL_PORT)) {
        Ok(listener) => listener,
        Err(e) => {
            warn!("Frame incoming control unavailable: {e}");
            return None;
        }
    };
    listener.set_nonblocking(true).ok()?;
    // Dashboard normally owns discovery, including while SteamVR is stopped.
    // A driver without a Dashboard can still advertise itself on the same port.
    let discovery = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, CONTROL_PORT))
        .ok()
        .and_then(|s| {
            s.set_nonblocking(true).ok()?;
            Some(s)
        });
    Some(thread::spawn(move || {
        let mut query = [0u8; 128];
        while *state.read() != LifecycleState::ShuttingDown {
            // Bound both work per tick and reply size. Discovery never adds/trusts a client.
            if let Some(discovery) = &discovery {
                for _ in 0..16 {
                    let Ok((size, peer)) = discovery.recv_from(&mut query) else {
                        break;
                    };
                    if let Some(reply) =
                        surfxr_sockets::frame_discovery_reply(&query[..size], CONTROL_PORT)
                            .or_else(|| surfxr_sockets::frame_start_reply(&query[..size], true))
                    {
                        discovery.send_to(&reply, peer).ok();
                    }
                }
            }
            if let Ok((stream, peer)) = listener.accept() {
                let timeout = Duration::from_millis(500);
                if let Ok(mut socket) = ProtoControlSocket::from_accepted(stream, timeout) {
                    if let Ok(result) = socket.recv::<ClientConnectionResult>(timeout) {
                        let caps = match &result {
                            ClientConnectionResult::ConnectionAccepted(info)
                                if info.client_protocol_id == surfxr_common::protocol_id_u64() =>
                            {
                                info.streaming_capabilities
                                    .as_ref()
                                    .map(|c| c.ext_str.as_str())
                            }
                            _ => None,
                        };
                        let hostname = caps.and_then(|caps| {
                            resolve_client(&mut SESSION_MANAGER.write(), peer.ip(), caps)
                        });
                        if let Some(hostname) = hostname {
                            connection::spawn_connection(
                                Arc::clone(&ctx),
                                Arc::clone(&state),
                                socket,
                                result,
                                hostname,
                                peer.ip(),
                            );
                        }
                    }
                }
            }
            thread::sleep(Duration::from_millis(20));
        }
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn incoming_frame_requires_trust_and_cannot_claim_an_active_entry() {
        let mut manager = ServerSessionManager::new(None);
        manager
            .session_mut()
            .session_settings
            .connection
            .client_discovery
            .content
            .auto_trust_clients = false;
        let ip: IpAddr = "127.0.0.2".parse().unwrap();
        let caps = r#"{"frame_client_connect":1,"frame_client_hostname":"frame-test"}"#;
        assert!(resolve_client(&mut manager, ip, caps).is_none());
        manager.update_client_connections("frame-test".into(), ClientConnectionsAction::Trust);
        assert_eq!(
            resolve_client(&mut manager, ip, caps).as_deref(),
            Some("frame-test")
        );
        assert!(resolve_client(&mut manager, ip, caps).is_none());
        assert!(resolve_client(&mut manager, ip, "{}").is_none());
    }
}
