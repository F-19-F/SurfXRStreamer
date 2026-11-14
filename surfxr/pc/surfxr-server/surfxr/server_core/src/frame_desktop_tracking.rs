//! Trusted Frame tracking/optics control while the desktop owns the media lease.
//! No streaming initialization, video sender, audio or encoder is created here.
use crate::{ConnectionContext, SESSION_MANAGER, ServerCoreEvent};
use surfxr_common::{AnyhowToCon, ToCon, ConResult, ConnectionError, ConnectionState, LifecycleState,
    parking_lot::{Mutex, RwLock}};
use surfxr_packets::{ClientConnectionResult, ClientConnectionsAction, ClientControlPacket,
    ServerControlPacket, TrackingData};
use surfxr_sockets::ProtoControlSocket;
use std::{net::IpAddr, sync::{Arc, atomic::{AtomicBool, AtomicU64, Ordering}}, time::{Duration, Instant}};

static OWNER: Mutex<()> = Mutex::new(());
static CONNECTED: AtomicBool = AtomicBool::new(false);
static RECEIVED: AtomicU64 = AtomicU64::new(0);
static APPLIED: AtomicU64 = AtomicU64::new(0);

pub(crate) fn requested(result: &ClientConnectionResult) -> bool {
    extension_requested(result, "frame_desktop_tracking")
}
pub(crate) fn optics_requested(result: &ClientConnectionResult) -> bool {
    extension_requested(result, "frame_desktop_optics")
}
fn extension_requested(result: &ClientConnectionResult, key: &str) -> bool {
    match result {
        ClientConnectionResult::ConnectionAccepted(info) => info.streaming_capabilities.as_ref()
            .and_then(|caps| serde_json::from_str::<serde_json::Value>(&caps.ext_str).ok())
            .is_some_and(|ext| ext[key].as_u64() == Some(1)),
        _ => false,
    }
}
pub(crate) fn snapshot() -> serde_json::Value {
    serde_json::json!({"connected": CONNECTED.load(Ordering::Relaxed),
        "enabled": SESSION_MANAGER.read().settings().extra.frame_desktop.keep_steamvr_tracking,
        "received_packets": RECEIVED.load(Ordering::Relaxed), "applied_packets": APPLIED.load(Ordering::Relaxed)})
}
fn decode(bytes: &[u8]) -> ConResult<TrackingData> {
    let (tracking, used): (TrackingData, usize) = bincode::serde::decode_from_slice(
        bytes, bincode::config::standard().with_limit::<1024>()).to_con()?;
    if used != bytes.len() || tracking.device_motions.is_empty() || tracking.device_motions.len() > 3 {
        return Err(ConnectionError::Other(surfxr_common::anyhow::anyhow!("Invalid desktop tracking")));
    }
    Ok(tracking)
}
pub(crate) fn run(ctx: &Arc<ConnectionContext>, lifecycle: Arc<RwLock<LifecycleState>>,
    socket: ProtoControlSocket, hostname: &str, ip: IpAddr, boundary_enabled: bool, optics_enabled: bool) -> ConResult {
    let Some(_owner) = OWNER.try_lock() else { return Ok(()); };
    if boundary_enabled {
        // Do not inherit a previous VR session's head-relative origin.
        crate::frame_boundary::recenter(&mut ctx.tracking_manager.write(),
            &surfxr_session::RecenteringMode::Stage, true);
    }
    let (sender, mut receiver) = socket.split::<ServerControlPacket, ClientControlPacket>(Duration::from_millis(200)).to_con()?;
    let sender = Arc::new(Mutex::new(sender));
    ctx.frame_sessions.register(hostname.into(), Arc::clone(&sender));
    {
        let mut session = SESSION_MANAGER.write();
        // The connection pipeline already records the authenticated origin IP.
        let _=ip;
        session.update_client_connections(hostname.into(), ClientConnectionsAction::SetConnectionState(ConnectionState::Connected));
    }
    // The desktop pose owner uses reliable tracking, not the VR state extension.
    // Do not inherit a previous VR session's disabled gate or timestamp barrier.
    ctx.events_sender.send(ServerCoreEvent::FrameControllerState(
        crate::frame_controller_state::ControllerState::session(false))).ok();
    CONNECTED.store(true, Ordering::Relaxed);
    struct End;
    impl Drop for End { fn drop(&mut self) { CONNECTED.store(false, Ordering::Relaxed); } }
    let _end = End;
    let mut previous = None;
    let mut alive = Instant::now();
    let mut keepalive = alive;
    while *lifecycle.read() != LifecycleState::ShuttingDown && alive.elapsed() < Duration::from_secs(5) {
        let enabled = {
            let session = SESSION_MANAGER.read();
            let Some(client) = session.client_list().get(hostname) else { break; };
            if !client.trusted || client.connection_state != ConnectionState::Connected { break; }
            session.settings().extra.frame_desktop.keep_steamvr_tracking
        };
        if previous != Some(enabled) {
            sender.lock().send(&ServerControlPacket::Reserved(
                if enabled {"frame_desktop_tracking_on"} else {"frame_desktop_tracking_off"}.into())).to_con()?;
            if boundary_enabled { sender.lock().send(&ServerControlPacket::Reserved("frame_boundary_sync_v1".into())).to_con()?; }
            // Older clients treat unknown Reserved replies as tracking OFF.
            if optics_enabled { sender.lock().send(&ServerControlPacket::Reserved("frame_desktop_optics_v1".into())).to_con()?; }
            previous = Some(enabled);
        }
        if keepalive.elapsed() >= Duration::from_secs(1) {
            sender.lock().send(&ServerControlPacket::KeepAlive).to_con()?;
            keepalive = Instant::now();
        }
        match receiver.recv(Duration::from_millis(200)) {
            Ok(packet) => {
                alive = Instant::now();
                match packet {
                    ClientControlPacket::LocalViewParams(params) if optics_enabled => {
                        if enabled { ctx.events_sender.send(ServerCoreEvent::LocalViewParams(params)).ok(); }
                    }
                    ClientControlPacket::ReservedBuffer(bytes) => {
                        if boundary_enabled && bytes.starts_with(&0x56444244u32.to_le_bytes()) {
                            if enabled { crate::frame_boundary::receive(ctx, &bytes); }
                            continue;
                        }
                        let mut tracking = decode(&bytes)?;
                        RECEIVED.fetch_add(1, Ordering::Relaxed);
                        if !enabled { continue; }
                        // The existing manager owns recentering, offsets and
                        // bounded history, exactly as in the VR tracking path.
                        let mut manager = ctx.tracking_manager.write();
                        let session = SESSION_MANAGER.read();
                        let settings = session.settings();
                        for (_, motion) in &mut tracking.device_motions {
                            motion.linear_velocity *= settings.extra.velocities_multiplier;
                            motion.angular_velocity *= settings.extra.velocities_multiplier;
                        }
                        manager.report_device_motions(&settings.headset,
                            tracking.poll_timestamp, &tracking.device_motions);
                        drop(session);
                        drop(manager);
                        ctx.events_sender.send(ServerCoreEvent::Tracking {poll_timestamp: tracking.poll_timestamp}).ok();
                        APPLIED.fetch_add(1, Ordering::Relaxed);
                    }
                    ClientControlPacket::PlayspaceSync(Some(bounds)) if enabled && !boundary_enabled => {
                        ctx.events_sender.send(ServerCoreEvent::PlayspaceSync(bounds)).ok();
                    }
                    ClientControlPacket::KeepAlive | ClientControlPacket::PlayspaceSync(_) => (),
                    _ => break,
                }
            }
            Err(ConnectionError::TryAgain(_)) => (),
            Err(ConnectionError::Other(_)) => break,
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn desktop_optics_requires_explicit_versioned_capability() {
        use surfxr_packets::{ConnectionAcceptedInfo, VideoStreamingCapabilities};
        use surfxr_common::glam::UVec2;
        let peer = |ext: &str| ClientConnectionResult::ConnectionAccepted(Box::new(ConnectionAcceptedInfo {
            client_protocol_id: 0, platform_string: "test".into(), server_ip: "127.0.0.1".parse().unwrap(),
            streaming_capabilities: Some(VideoStreamingCapabilities {
                default_view_resolution: UVec2::splat(32), max_view_resolution: UVec2::splat(32),
                refresh_rates: vec![90.0], microphone_sample_rate: 48000,
                foveated_encoding: false, encoder_high_profile: false, encoder_10_bits: false,
                encoder_av1: false, prefer_10bit: false, preferred_encoding_gamma: 1.0,
                prefer_hdr: false, ext_str: ext.into(),
            }),
        }));
        let old = peer(r#"{"frame_desktop_tracking":1,"frame_boundary_sync":1}"#);
        assert!(requested(&old) && !optics_requested(&old));
        let new = peer(r#"{"frame_desktop_tracking":1,"frame_desktop_optics":1}"#);
        assert!(requested(&new) && optics_requested(&new));
        for ext in ["{}", "invalid", r#"{"frame_desktop_optics":2}"#, r#"{"frame_desktop_optics":true}"#] {
            assert!(!optics_requested(&peer(ext)));
        }
        assert!(!optics_requested(&ClientConnectionResult::ClientStandby));
    }
    #[test]
    fn pose_control_keeps_existing_tracking_bytes_and_rejects_trailing_data() {
        let tracking = TrackingData {poll_timestamp: Duration::from_secs(1),
            device_motions: vec![(1, surfxr_common::DeviceMotion::default())],
            hand_skeletons: [None; 2], face: Default::default(), body: None};
        let mut bytes = bincode::serde::encode_to_vec(&tracking, bincode::config::standard()).unwrap();
        assert_eq!(decode(&bytes).ok().unwrap().poll_timestamp, tracking.poll_timestamp);
        bytes.push(0); assert!(decode(&bytes).is_err());
        assert!(decode(&[0; 1025]).is_err());
    }
}
