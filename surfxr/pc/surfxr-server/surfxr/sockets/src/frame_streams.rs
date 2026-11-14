//! Opt-in Frame extension. Existing SURFXR packet bytes/settings remain unchanged.
use crate::CONTROL_PORT;
use surfxr_common::{anyhow::Result, anyhow::bail};
use surfxr_session::SocketProtocol;
use serde_json::{Value, json};

pub fn negotiate_frame_streams(
    capabilities: &str,
    protocol: SocketProtocol,
    video_port: u16,
) -> Result<(String, Option<u16>)> {
    // A client that does not opt in retains its original transport, including
    // older clients with an empty/non-JSON opaque extension string.
    let extension: Value = serde_json::from_str(capabilities).unwrap_or(Value::Null);
    if extension.get("frame_udp_only").and_then(Value::as_u64) == Some(1)
        && matches!(protocol, SocketProtocol::Tcp)
    {
        bail!("Frame TCP media is deprecated; select UDP in SURFXR connection settings");
    }
    // UDP is a separate capability: a previously deployed TCP-only Frame must
    // never receive a UDP acknowledgement that it cannot implement.
    let key = match protocol {
        SocketProtocol::Tcp => "frame_split_streams",
        SocketProtocol::Udp => "frame_split_streams_udp",
    };
    if extension.get(key).and_then(Value::as_u64) != Some(1) {
        return Ok(("{}".into(), None));
    }
    let Some(priority_port) = video_port.checked_add(1) else {
        bail!("Frame priority port exceeds transport port range");
    };
    if video_port < 1024 || priority_port == CONTROL_PORT || video_port == CONTROL_PORT {
        bail!("Frame stream ports collide with control port");
    }
    Ok((
        json!({key: {"version": 1, "priority_port": priority_port}}).to_string(),
        Some(priority_port),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        ClientControlPacket, ProtoControlSocket, ServerControlPacket, SocketConnection,
        StreamSocketConfig,
    };
    use surfxr_session::{SocketBufferConfig, SocketBufferSize};
    use std::{
        io::{Read, Write},
        net::{TcpListener, TcpStream},
        sync::{
            Arc,
            atomic::{AtomicBool, Ordering},
        },
        thread,
        time::{Duration, Instant},
    };

    #[test]
    fn negotiation_keeps_legacy_and_tcp_only_clients_unchanged() {
        for caps in [
            "",
            "opaque-legacy-extension",
            "{}",
            r#"{"other_extension":1}"#,
            r#"{"frame_split_streams":2}"#,
        ] {
            assert_eq!(
                negotiate_frame_streams(caps, SocketProtocol::Tcp, 19944).unwrap(),
                ("{}".into(), None)
            );
        }
        assert_eq!(
            negotiate_frame_streams(r#"{"frame_split_streams":1}"#, SocketProtocol::Udp, 19944)
                .unwrap()
                .1,
            None
        );
    }

    #[test]
    fn negotiation_validates_ports_and_acknowledges_version() {
        let caps = r#"{"frame_split_streams":1}"#;
        let (ack, port) = negotiate_frame_streams(caps, SocketProtocol::Tcp, 19944).unwrap();
        assert_eq!(port, Some(19945));
        assert_eq!(
            serde_json::from_str::<Value>(&ack).unwrap()["frame_split_streams"]["version"],
            1
        );
        for port in [1023, 19942, 19943, 65535] {
            assert!(negotiate_frame_streams(caps, SocketProtocol::Tcp, port).is_err());
        }
    }

    #[test]
    fn udp_negotiation_requires_its_own_capability() {
        let caps = r#"{"frame_split_streams":1,"frame_split_streams_udp":1}"#;
        let (ack, port) = negotiate_frame_streams(caps, SocketProtocol::Udp, 19944).unwrap();
        assert_eq!(port, Some(19945));
        let ack: Value = serde_json::from_str(&ack).unwrap();
        assert_eq!(ack["frame_split_streams_udp"]["version"], 1);
        assert!(ack.get("frame_split_streams").is_none());
        for port in [1023, 19942, 19943, 65535] {
            assert!(negotiate_frame_streams(caps, SocketProtocol::Udp, port).is_err());
        }
        assert_eq!(
            negotiate_frame_streams(
                r#"{"frame_split_streams_udp":2}"#,
                SocketProtocol::Udp,
                19944
            )
            .unwrap()
            .1,
            None
        );
        assert_eq!(
            negotiate_frame_streams(
                r#"{"frame_split_streams_udp":1}"#,
                SocketProtocol::Tcp,
                19944
            )
            .unwrap()
            .1,
            None
        );
    }

    #[test]
    fn udp_only_frame_rejects_tcp_without_affecting_legacy_clients() {
        let caps = r#"{"frame_udp_only":1,"frame_split_streams_udp":1}"#;
        assert!(negotiate_frame_streams(caps, SocketProtocol::Tcp, 19944).is_err());
        assert_eq!(
            negotiate_frame_streams(caps, SocketProtocol::Udp, 19944)
                .unwrap()
                .1,
            Some(19945)
        );
        assert!(negotiate_frame_streams("{}", SocketProtocol::Tcp, 19944).is_ok());
    }

    #[test]
    fn legacy_tcp_retains_fragmented_prefix_and_payload() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let timeout = Duration::from_millis(50);
        let mut socket = crate::StreamSocketBuilder::connect_to_client(
            timeout,
            "127.0.0.1".parse().unwrap(),
            listener.local_addr().unwrap().port(),
            SocketProtocol::Tcp,
            None,
            SocketBufferConfig::default(),
            1400,
        )
        .ok()
        .unwrap();
        let mut peer = listener.accept().unwrap().0;
        let mut receiver = socket.subscribe_to_stream::<u64>(surfxr_packets::TRACKING, 2);
        peer.write_all(&[0, 0, 0, 0]).unwrap();
        assert!(socket.recv().is_err());
        peer.write_all(&[0, 0, 4, 0, 0, 0, 42, 1]).unwrap();
        assert!(socket.recv().is_err());
        peer.write_all(&[2, 3]).unwrap();
        assert!(socket.recv().is_ok());
        let packet = receiver.recv(timeout).ok().unwrap();
        let (header, payload) = packet.get().unwrap();
        assert_eq!(header, 42);
        assert_eq!(payload, [1, 2, 3]);
        peer.shutdown(std::net::Shutdown::Both).unwrap();
        assert!(socket.recv().is_err());
    }

    #[test]
    fn priority_packets_bypass_a_blocked_video_writer_and_reader() {
        // Reserve all endpoints before starting. Never use live SURFXR ports.
        let control = TcpListener::bind("127.0.0.1:0").unwrap();
        let video = TcpListener::bind("127.0.0.1:0").unwrap();
        let priority = TcpListener::bind("127.0.0.1:0").unwrap();
        socket2::SockRef::from(&video)
            .set_recv_buffer_size(4096)
            .unwrap();
        let control_port = control.local_addr().unwrap().port();
        let video_port = video.local_addr().unwrap().port();
        let priority_port = priority.local_addr().unwrap().port();
        let timeout = Duration::from_secs(1);
        let peer = thread::spawn(move || {
            let mut control = ProtoControlSocket {
                inner: control.accept().unwrap().0,
            };
            assert_eq!(
                control.recv::<String>(timeout).ok().unwrap(),
                "synthetic-config"
            );
            assert!(matches!(
                control.recv(timeout).ok().unwrap(),
                ServerControlPacket::StartStream
            ));
            control.send(&ClientControlPacket::StreamReady).unwrap();
            (
                control,
                video.accept().unwrap().0,
                priority.accept().unwrap().0,
            )
        });
        let mut connection = SocketConnection::from_client_connection_split(
            ProtoControlSocket {
                inner: TcpStream::connect(("127.0.0.1", control_port)).unwrap(),
            },
            timeout,
            "synthetic-config",
            StreamSocketConfig {
                protocol: SocketProtocol::Tcp,
                port: video_port,
                buffer_config: SocketBufferConfig {
                    send_size_bytes: SocketBufferSize::Custom(4096),
                    recv_size_bytes: SocketBufferSize::Custom(4096),
                },
                max_packet_size: 1400,
                dscp: None,
            },
            Some(priority_port),
        )
        .ok()
        .unwrap();
        let (_control, video_peer, mut priority_peer) = peer.join().unwrap();
        priority_peer.set_read_timeout(Some(timeout)).unwrap();
        video_peer.set_read_timeout(Some(timeout)).unwrap();
        let mut video_sender = connection.request_unreliable_stream::<()>(surfxr_packets::VIDEO);
        let finished = Arc::new(AtomicBool::new(false));
        let sender_finished = Arc::clone(&finished);
        let blocked = thread::spawn(move || {
            let payload = vec![0x55; 4 * 1024 * 1024];
            let mut result = Ok(());
            // Fill finite Windows loopback write slack without reading video.
            for _ in 0..32 {
                result = video_sender.send_header_with_payload(&(), &payload);
                if result.is_err() {
                    break;
                }
            }
            sender_finished.store(true, Ordering::SeqCst);
            result
        });
        // Seeing the prefix proves the production video writer has begun.
        let mut prefix = [0; 10];
        assert_eq!(video_peer.peek(&mut prefix).unwrap(), 10);
        thread::sleep(Duration::from_millis(50));
        assert!(!finished.load(Ordering::SeqCst));
        let started = Instant::now();
        connection
            .request_unreliable_stream::<()>(surfxr_packets::AUDIO)
            .send_header_with_payload(&(), &[1, 2, 3, 4])
            .unwrap();
        priority_peer.read_exact(&mut prefix).unwrap();
        assert_eq!(
            u16::from_le_bytes(prefix[..2].try_into().unwrap()),
            surfxr_packets::AUDIO
        );
        let mut pcm = [0; 4];
        priority_peer.read_exact(&mut pcm).unwrap();
        assert_eq!(pcm, [1, 2, 3, 4]);
        assert!(started.elapsed() < Duration::from_millis(500));
        assert!(!finished.load(Ordering::SeqCst));
        // Tracking reception also ignores the unused video read socket.
        let mut tracking =
            connection.subscribe_to_unreliable_stream::<u64>(surfxr_packets::TRACKING, 2);
        priority_peer
            .write_all(&[0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 42])
            .unwrap();
        assert!(connection.recv_poll().is_ok());
        assert_eq!(
            tracking.recv(timeout).ok().unwrap().get_header().unwrap(),
            42
        );
        // Preserve a partially consumed prefix across a read timeout.
        priority_peer.write_all(&[0, 0, 1]).unwrap();
        assert!(connection.recv_poll().is_err());
        priority_peer.write_all(&[0, 0, 0, 1, 0, 0, 0, 43]).unwrap();
        assert!(connection.recv_poll().is_ok());
        assert_eq!(
            tracking.recv(timeout).ok().unwrap().get_header().unwrap(),
            43
        );
        priority_peer.shutdown(std::net::Shutdown::Both).unwrap();
        assert!(connection.recv_poll().is_err());
        drop(video_peer);
        let _ = blocked.join().unwrap();
    }
}
