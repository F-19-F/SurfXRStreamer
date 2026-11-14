mod control_socket;
mod frame_streams;
mod frame_fec;
mod frame_video;
mod frame_gaze;
mod frame_discovery;
mod stream_socket;
pub use stream_socket::FrameQos;

use surfxr_common::{AnyhowToCon, ConResult, ToCon, anyhow::Result, con_bail, info};
use surfxr_packets::{ClientControlPacket, ServerControlPacket};
use surfxr_session::{DscpTos, SocketBufferConfig, SocketBufferSize, SocketProtocol};
use serde::{Serialize, de::DeserializeOwned};
use socket2::Socket;
use std::{
    marker::PhantomData,
    net::{IpAddr, Ipv4Addr, TcpListener},
    time::Duration,
};

pub use control_socket::*;
pub use frame_streams::negotiate_frame_streams;
pub use frame_fec::negotiate_frame_video_fec;
pub use frame_video::{FrameVideoHeader, negotiate_frame_video_haptics, negotiate_frame_video_timestamps};
pub use frame_gaze::{FrameGazeConfig, FrameGazeEye, FrameGazeSample, decode_frame_gaze, frame_gaze_requested, negotiate_frame_gaze};
pub use frame_discovery::{frame_discovery_reply, frame_start_hostname, frame_start_reply,frame_start_status_hostname,frame_start_status_reply};
pub use stream_socket::*;

pub const LOCAL_IP: IpAddr = IpAddr::V4(Ipv4Addr::UNSPECIFIED);
pub const CONTROL_PORT: u16 = surfxr_common::ports::VR_CONTROL;
pub const HANDSHAKE_PACKET_SIZE_BYTES: usize = 56; // this may change in future protocols
pub const KEEPALIVE_INTERVAL: Duration = Duration::from_millis(500);
pub const KEEPALIVE_TIMEOUT: Duration = Duration::from_secs(2);

pub const MDNS_SERVICE_TYPE: &str = "_surfxr._tcp.local.";
pub const MDNS_PROTOCOL_KEY: &str = "protocol";
pub const MDNS_DEVICE_ID_KEY: &str = "device_id";

pub const WIRED_CLIENT_HOSTNAME: &str = "client.wired";

fn set_socket_buffers(socket: &socket2::Socket, buffer_config: SocketBufferConfig) -> Result<()> {
    info!(
        "Initial socket buffer size: send: {}B, recv: {}B",
        socket.send_buffer_size()?,
        socket.recv_buffer_size()?
    );

    {
        let maybe_size = match buffer_config.send_size_bytes {
            SocketBufferSize::Default => None,
            SocketBufferSize::Maximum => Some(u32::MAX),
            SocketBufferSize::Custom(size) => Some(size),
        };

        if let Some(size) = maybe_size {
            if let Err(e) = socket.set_send_buffer_size(size as usize) {
                info!("Error setting socket send buffer: {e}");
            } else {
                info!(
                    "Set socket send buffer succeeded: {}",
                    socket.send_buffer_size()?
                );
            }
        }
    }

    {
        let maybe_size = match buffer_config.recv_size_bytes {
            SocketBufferSize::Default => None,
            SocketBufferSize::Maximum => Some(u32::MAX),
            SocketBufferSize::Custom(size) => Some(size),
        };

        if let Some(size) = maybe_size {
            if let Err(e) = socket.set_recv_buffer_size(size as usize) {
                info!("Error setting socket recv buffer: {e}");
            } else {
                info!(
                    "Set socket recv buffer succeeded: {}",
                    socket.recv_buffer_size()?
                );
            }
        }
    }

    Ok(())
}

fn set_dscp(socket: &Socket, dscp: Option<DscpTos>) {
    // https://en.wikipedia.org/wiki/Differentiated_services
    if let Some(dscp) = dscp {
        let tos = match dscp {
            DscpTos::BestEffort => 0,
            DscpTos::ClassSelector(precedence) => precedence << 3,
            DscpTos::AssuredForwarding {
                class,
                drop_probability,
            } => (class << 3) | drop_probability as u8,
            DscpTos::ExpeditedForwarding => 0b101110,
        };

        socket.set_tos_v4((tos << 2) as u32).ok();
    }
}

// connect_to_client should be used on the server side.
// At the moment, the TcpListener is implemened on the client side so the API for this function
// is non-standard
// todo: convert to class when storing a TcpListener
pub fn connect_to_client<T: DeserializeOwned>(
    client_ips: Vec<IpAddr>,
    timeout: Duration,
) -> ConResult<(ProtoControlSocket, IpAddr, T)> {
    let (mut control_socket, client_ip) =
        ProtoControlSocket::connect_to(timeout, PeerType::AnyClient(client_ips))?;

    let res = control_socket.recv(timeout)?;

    Ok((control_socket, client_ip, res))
}

pub fn listen_to_server<T: DeserializeOwned>(
    listener_socket: &TcpListener,
    timeout: Duration,
    client_info: &impl Serialize,
) -> ConResult<(ProtoControlSocket, T)> {
    let (mut control_socket, _) =
        ProtoControlSocket::connect_to(timeout, PeerType::Server(listener_socket))?;

    control_socket.send(client_info).to_con()?;

    let config_packet = control_socket.recv(timeout)?;

    Ok((control_socket, config_packet))
}

pub fn send_restart_signal(
    mut control_socket: ProtoControlSocket,
    stream_config_packet: impl Serialize,
) -> ConResult<()> {
    // We must send the config packet before, which will be unused
    control_socket.send(&stream_config_packet).to_con()?;

    control_socket
        .send(&ServerControlPacket::Restarting)
        .to_con()
}

pub struct StreamSocketConfig {
    pub protocol: SocketProtocol,
    pub port: u16,
    pub buffer_config: SocketBufferConfig,
    pub max_packet_size: usize,
    pub dscp: Option<DscpTos>,
}

pub enum ServerConnectionResult {
    Connected(SocketConnection),
    Restarting,
}

pub struct SocketConnection {
    control_socket: ProtoControlSocket,
    stream_socket: StreamSocket,
    priority_socket: Option<StreamSocket>,
}

impl SocketConnection {
    // Only the authenticated local daemon calls this after the unchanged
    // handshake. Legacy/client receive and keepalive deadlines remain intact.
    pub fn configure_frame_daemon_poll(&self) -> Result<()> {
        let Some(priority) = &self.priority_socket else {
            surfxr_common::anyhow::bail!("Local daemon polling requires split UDP")
        };
        priority.configure_local_udp_poll(Duration::from_millis(20))
    }
    pub fn configure_frame_video_fec(&self, enabled: bool) {
        self.stream_socket.configure_frame_video_fec(enabled);
    }
    // A twice-encoding-rate send budget spreads Frame UDP video bursts.
    // It does not change encoder bitrate or discard a packet. TCP and ordinary
    // clients retain their existing writer; adaptive bitrate is not capped here.
    pub fn configure_frame_udp_video(&self, bitrate: &surfxr_session::BitrateConfig) {
        if let Some(priority) = &self.priority_socket {
            self.stream_socket.configure_frame_udp_qos(false);
            priority.configure_frame_udp_qos(true);
            if let surfxr_session::BitrateMode::ConstantMbps(mbps) = bitrate.mode {
                self.stream_socket
                    .configure_video_pacing(mbps.saturating_mul(2_000_000) / 8);
            }
        }
    }

    // Note: the timeout resets after each internal operation
    pub fn from_client_connection(
        control_socket: ProtoControlSocket,
        timeout: Duration,
        stream_config_packet: impl Serialize,
        socket_config: StreamSocketConfig,
    ) -> ConResult<Self> {
        Self::from_client_connection_split(
            control_socket,
            timeout,
            stream_config_packet,
            socket_config,
            None,
        )
    }

    pub fn from_client_connection_split(
        mut control_socket: ProtoControlSocket,
        timeout: Duration,
        stream_config_packet: impl Serialize,
        socket_config: StreamSocketConfig,
        priority_port: Option<u16>,
    ) -> ConResult<Self> {
        let client_ip = control_socket.inner.peer_addr().to_con()?.ip();

        control_socket.send(&stream_config_packet).to_con()?;

        control_socket
            .send(&ServerControlPacket::StartStream)
            .to_con()?;

        let signal = control_socket.recv(timeout)?;
        if !matches!(signal, ClientControlPacket::StreamReady) {
            con_bail!("Got unexpected packet waiting for stream ack");
        }

        let stream_socket = StreamSocketBuilder::connect_to_client(
            timeout,
            client_ip,
            socket_config.port,
            socket_config.protocol,
            socket_config.dscp,
            socket_config.buffer_config,
            socket_config.max_packet_size,
        )?;

        let priority_socket = if let Some(port) = priority_port {
            if port == socket_config.port || port == CONTROL_PORT || port < 1024 {
                con_bail!("Invalid Frame split transport");
            }
            Some(StreamSocketBuilder::connect_priority_to_client(
                timeout,
                client_ip,
                port,
                socket_config.protocol,
                socket_config.dscp,
                SocketBufferConfig {
                    send_size_bytes: SocketBufferSize::Custom(64 * 1024),
                    recv_size_bytes: SocketBufferSize::Custom(128 * 1024),
                },
                socket_config.max_packet_size,
            )?)
        } else {
            None
        };

        Ok(Self {
            control_socket,
            stream_socket,
            priority_socket,
        })
    }

    // Note: the timeout resets after each internal operation
    pub fn from_server_connection(
        mut control_socket: ProtoControlSocket,
        timeout: Duration,
        socket_config: StreamSocketConfig,
    ) -> ConResult<ServerConnectionResult> {
        let server_ip = control_socket.inner.peer_addr().to_con()?.ip();

        match control_socket.recv(timeout)? {
            ServerControlPacket::StartStream => (),
            ServerControlPacket::Restarting => return Ok(ServerConnectionResult::Restarting),
            _ => con_bail!("Got unexpected packet waiting for stream start"),
        }

        let stream_socket_builder = StreamSocketBuilder::listen_for_server(
            timeout,
            socket_config.port,
            socket_config.protocol,
            socket_config.dscp,
            socket_config.buffer_config,
        )
        .to_con()?;

        control_socket
            .send(&ClientControlPacket::StreamReady)
            .to_con()?;

        let stream_socket = stream_socket_builder.accept_from_server(
            server_ip,
            socket_config.port,
            socket_config.max_packet_size,
            timeout,
        )?;

        Ok(ServerConnectionResult::Connected(Self {
            control_socket,
            stream_socket,
            priority_socket: None,
        }))
    }

    pub fn request_reliable_stream<T>(&self) -> ConResult<ControlSocketSender<T>> {
        Ok(ControlSocketSender {
            inner: self.control_socket.inner.try_clone().to_con()?,
            buffer: vec![],
            _phantom: PhantomData,
        })
    }

    pub fn subscribe_to_reliable_stream<T>(&self) -> ConResult<ControlSocketReceiver<T>> {
        Ok(ControlSocketReceiver {
            inner: self.control_socket.inner.try_clone().to_con()?,
            buffer: vec![],
            recv_cursor: None,
            _phantom: PhantomData,
        })
    }

    pub fn request_unreliable_stream<T>(&self, stream_id: u16) -> StreamSender<T> {
        if stream_id != surfxr_packets::VIDEO
            && let Some(socket) = &self.priority_socket
        {
            socket.request_stream(stream_id)
        } else {
            self.stream_socket.request_stream(stream_id)
        }
    }

    pub fn subscribe_to_unreliable_stream<T>(
        &mut self,
        stream_id: u16,
        max_concurrent_buffers: usize,
    ) -> StreamReceiver<T> {
        if stream_id != surfxr_packets::VIDEO
            && let Some(socket) = &mut self.priority_socket
        {
            socket.subscribe_to_stream(stream_id, max_concurrent_buffers)
        } else {
            self.stream_socket
                .subscribe_to_stream(stream_id, max_concurrent_buffers)
        }
    }

    pub fn recv_poll(&mut self) -> ConResult<()> {
        // In split mode Frame uploads only small packets. Never poll a video
        // reader first: its timeout would delay tracking/microphone delivery.
        if let Some(socket) = &mut self.priority_socket {
            socket.recv()
        } else {
            self.stream_socket.recv()
        }
    }
}
