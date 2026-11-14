// Note: for StreamSocket, the client uses a server socket, the server uses a client socket.
// This is because of certificate management. The server needs to trust a client and its certificate
//
// StreamSender and StreamReceiver endpoints allow for convenient conversion of the header to/from
// bytes while still handling the additional byte buffer with zero copies and extra allocations.

// Performance analysis:
// We want to minimize the transmission time for various sizes of packets.
// The writer lock spans one complete packet, including all UDP shards. Negotiated
// Frame sessions use an independent priority socket/writer so small audio and
// tracking packets do not wait behind video packet transmission.
// Note: We can't clone the underlying socket for each StreamSender and the mutex around the socket
// cannot be removed. This is because we need to make sure at least shards are written whole.

mod frame_qos;
mod external;
mod pacing;
mod udp_timing;
mod video_fec;
pub use frame_qos::{FrameUdpThread,FrameQos};
mod tcp;
mod udp;
#[cfg(test)]
mod udp_tests;

use surfxr_common::{
    AnyhowToCon, ConResult, HandleTryAgain, ToCon, anyhow::Result, parking_lot::Mutex,
};
use surfxr_session::{DscpTos, SocketBufferConfig, SocketProtocol};
use bincode::config;
use serde::{Serialize, de::DeserializeOwned};
use std::{
    cmp::Ordering,
    collections::HashMap,
    marker::PhantomData,
    mem,
    net::{IpAddr, TcpListener, UdpSocket},
    ops::{Deref, DerefMut},
    sync::{Arc, mpsc},
    time::Duration,
};

trait MultiplexedSocketWriter {
    // Note: consts are not trait-safe, we require a method
    fn payload_offset(&self) -> usize;

    #[cfg(windows)]
    fn frame_udp_socket_for_process(&mut self) -> Option<usize> { None }

    fn set_video_pacing(&mut self, _bytes_per_second: u64) {}
    fn update_video_bitrate(&mut self, _bitrate_bps: u64, _overhead: u32) {}
    fn set_frame_video_fec(&mut self, _enabled: bool) {}
    fn set_frame_udp_qos(&mut self, _priority: bool) {}
    fn frame_udp_priority(&self) -> Option<bool> {
        None
    }

    fn send(&mut self, stream_id: u16, packet_index: u32, buffer: &mut Vec<u8>) -> Result<()>;
}

struct ReconstructedPacket {
    index: u32,
    buffer: Vec<u8>,
}

struct StreamRecvQueues {
    used_buffer_sender: mpsc::Sender<Vec<u8>>,
    used_buffer_receiver: mpsc::Receiver<Vec<u8>>,
    packet_queue: mpsc::Sender<ReconstructedPacket>,
}

trait MultiplexedSocketReader {
    fn payload_offset(&self) -> usize;

    fn configure_local_udp_poll(&self, _timeout: Duration) -> Result<()> {
        surfxr_common::anyhow::bail!("Local daemon polling requires UDP")
    }

    fn recv(&mut self, stream_queues: &HashMap<u16, StreamRecvQueues>) -> ConResult;
}

/// Memory buffer that contains a hidden prefix
#[derive(Default)]
pub struct Buffer<H = ()> {
    inner: Vec<u8>,
    raw_payload_offset: usize, // this corresponds to prefix + header
    _phantom: PhantomData<H>,
}

impl<H> Deref for Buffer<H> {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.inner[self.raw_payload_offset..]
    }
}

impl<H> DerefMut for Buffer<H> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.inner[self.raw_payload_offset..]
    }
}

#[derive(Clone)]
pub struct StreamSender<H> {
    inner: Arc<Mutex<Box<dyn MultiplexedSocketWriter + Send>>>,
    stream_id: u16,
    payload_offset: usize,
    next_packet_index: u32,
    used_buffers: Vec<Vec<u8>>,
    _phantom: PhantomData<H>,
}

impl<H> StreamSender<H> {
    pub fn frame_udp_priority(&self) -> Option<bool> {
        self.inner.lock().frame_udp_priority()
    }
    pub fn prioritize_frame_udp_thread(&self) -> Option<FrameUdpThread> {
        self.inner
            .lock()
            .frame_udp_priority()
            .map(FrameUdpThread::new)
    }
    /// Shard and send a buffer with zero copies and zero allocations.
    /// The prefix of each shard is written over the previously sent shard to avoid reallocations.
    pub fn send(&mut self, mut buffer: Buffer<H>) -> Result<()> {
        let result = self.inner
            .lock()
            .send(self.stream_id, self.next_packet_index, &mut buffer.inner);

        self.used_buffers.push(buffer.inner);

        self.next_packet_index = self.next_packet_index.wrapping_add(1);

        // A failed UDP send may already have sent a prefix of this packet.
        // Reusing its index would mix shards from different encoded pictures.
        result
    }
}

impl<H: Serialize> StreamSender<H> {
    pub fn get_buffer(&mut self, header: &H, raw_payload_len: usize) -> Result<Buffer<H>> {
        let mut buffer = self.used_buffers.pop().unwrap_or_default();

        buffer.resize(self.payload_offset, 0);

        let encoded_payload_size =
            bincode::serde::encode_into_std_write(header, &mut buffer, config::standard())?;

        let raw_payload_offset = self.payload_offset + encoded_payload_size;

        buffer.resize(raw_payload_offset + raw_payload_len, 0);

        Ok(Buffer {
            inner: buffer,
            raw_payload_offset,
            _phantom: PhantomData,
        })
    }

    pub fn send_header_with_payload(&mut self, header: &H, raw_payload: &[u8]) -> Result<()> {
        let mut buffer = self.get_buffer(header, raw_payload.len())?;
        buffer.copy_from_slice(raw_payload);
        self.send(buffer)
    }

    pub fn send_header(&mut self, header: &H) -> Result<()> {
        self.send_header_with_payload(header, &[])
    }
}

pub struct ReceiverData<H> {
    buffer: Vec<u8>,
    payload_offset: usize,
    used_buffer_queue: mpsc::Sender<Vec<u8>>,
    had_packet_loss: bool,
    _phantom: PhantomData<H>,
}

impl<H> ReceiverData<H> {
    pub fn had_packet_loss(&self) -> bool {
        self.had_packet_loss
    }
}

impl<H: DeserializeOwned> ReceiverData<H> {
    pub fn get(&self) -> Result<(H, &[u8])> {
        let payload = &self.buffer[self.payload_offset..];

        let (header, decoded_size) =
            bincode::serde::decode_from_slice(payload, config::standard())?;

        Ok((header, &payload[decoded_size..]))
    }

    pub fn get_header(&self) -> Result<H> {
        Ok(self.get()?.0)
    }
}

impl<H> Drop for ReceiverData<H> {
    fn drop(&mut self) {
        self.used_buffer_queue
            .send(mem::take(&mut self.buffer))
            .ok();
    }
}

pub struct StreamReceiver<H> {
    payload_offset: usize,
    packet_receiver: mpsc::Receiver<ReconstructedPacket>,
    used_buffer_queue: mpsc::Sender<Vec<u8>>,
    last_packet_index: Option<u32>,
    _phantom: PhantomData<H>,
}

// Wrapping comparison for packet indices.
// Problem: packet indices have a finite bit-width and we have to wrap around upon reaching the
// maximum value. This function provides proper ordering capability when wrapping around. The
// maximum index distance that two packets can have is u32::MAX / 2 (which is plenty for any
// reasonable circumstance).
fn wrapping_cmp(lhs: u32, rhs: u32) -> Ordering {
    const LOWER_BOUND: u32 = u32::MAX / 4;
    const UPPER_BOUND: u32 = u32::MAX - LOWER_BOUND;

    if lhs < LOWER_BOUND && rhs > UPPER_BOUND {
        // lhs wrapped around, so its "unwrapped" value would be `lhs as u64 + u32::MAX`
        Ordering::Greater
    } else if lhs > UPPER_BOUND && rhs < LOWER_BOUND {
        // rhs wrapped around
        Ordering::Less
    } else {
        lhs.cmp(&rhs)
    }
}

impl<H: DeserializeOwned + Serialize> StreamReceiver<H> {
    pub fn recv(&mut self, timeout: Duration) -> ConResult<ReceiverData<H>> {
        let packet = self
            .packet_receiver
            .recv_timeout(timeout)
            .handle_try_again()?;

        let mut had_packet_loss = false;

        if let Some(last_idx) = self.last_packet_index {
            // Use wrapping arithmetics
            match wrapping_cmp(packet.index, last_idx.wrapping_add(1)) {
                Ordering::Equal => (),
                Ordering::Greater => {
                    // Skipped some indices
                    had_packet_loss = true
                }
                Ordering::Less => {
                    // Old packet, discard
                    self.used_buffer_queue.send(packet.buffer).to_con()?;
                    return surfxr_common::try_again();
                }
            }
        }
        self.last_packet_index = Some(packet.index);

        Ok(ReceiverData {
            buffer: packet.buffer,
            payload_offset: self.payload_offset,
            used_buffer_queue: self.used_buffer_queue.clone(),
            had_packet_loss,
            _phantom: PhantomData,
        })
    }
}

pub enum StreamSocketBuilder {
    Tcp(TcpListener),
    Udp(UdpSocket),
}

impl StreamSocketBuilder {
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn connect_priority_to_client(
        timeout: Duration,
        client_ip: IpAddr,
        port: u16,
        protocol: SocketProtocol,
        dscp: Option<DscpTos>,
        buffer_config: SocketBufferConfig,
        max_packet_size: usize,
    ) -> ConResult<StreamSocket> {
        let (send_socket, receive_socket) = match protocol {
            SocketProtocol::Tcp => {
                let socket: socket2::Socket =
                    tcp::connect_to_client(timeout, &[client_ip], port, buffer_config)?.into();
                crate::set_dscp(&socket, dscp);
                tcp::split_priority(socket.into(), timeout).to_con()?
            }
            SocketProtocol::Udp => {
                let local=if client_ip.is_loopback(){"127.0.0.1".parse().unwrap()}else{crate::LOCAL_IP};
                let socket = udp::bind_on(local,port, dscp, buffer_config).to_con()?;
                udp::connect(&socket, client_ip, port, timeout).to_con()?;
                socket.set_write_timeout(Some(timeout)).to_con()?;
                udp::split_priority(socket, max_packet_size).to_con()?
            }
        };
        Ok(StreamSocket {
            send_socket: Arc::new(Mutex::new(send_socket)),
            receive_socket,
            queues: HashMap::new(),
        })
    }
    pub fn listen_for_server(
        timeout: Duration,
        port: u16,
        stream_socket_config: SocketProtocol,
        stream_tos_config: Option<DscpTos>,
        buffer_config: SocketBufferConfig,
    ) -> Result<Self> {
        Ok(match stream_socket_config {
            SocketProtocol::Udp => {
                StreamSocketBuilder::Udp(udp::bind(port, stream_tos_config, buffer_config)?)
            }
            SocketProtocol::Tcp => StreamSocketBuilder::Tcp(tcp::bind(
                timeout,
                port,
                stream_tos_config,
                buffer_config,
            )?),
        })
    }

    pub fn accept_from_server(
        self,
        server_ip: IpAddr,
        port: u16,
        max_packet_size: usize,
        timeout: Duration,
    ) -> ConResult<StreamSocket> {
        let (send_socket, receive_socket) = match self {
            StreamSocketBuilder::Udp(socket) => {
                udp::connect(&socket, server_ip, port, timeout).to_con()?;
                udp::split_multiplexed(socket, max_packet_size).to_con()?
            }
            StreamSocketBuilder::Tcp(listener) => {
                let socket = tcp::accept_from_server(&listener, Some(server_ip), timeout)?;
                tcp::split_multiplexed(socket, timeout).to_con()?
            }
        };

        Ok(StreamSocket {
            send_socket: Arc::new(Mutex::new(send_socket)),
            receive_socket,
            queues: HashMap::new(),
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn connect_to_client(
        timeout: Duration,
        client_ip: IpAddr,
        port: u16,
        protocol: SocketProtocol,
        dscp: Option<DscpTos>,
        buffer_config: SocketBufferConfig,
        max_packet_size: usize,
    ) -> ConResult<StreamSocket> {
        let (send_socket, receive_socket) = match protocol {
            SocketProtocol::Udp => {
                let local=if client_ip.is_loopback(){"127.0.0.1".parse().unwrap()}else{crate::LOCAL_IP};
                let socket = udp::bind_on(local,port, dscp, buffer_config).to_con()?;
                udp::connect(&socket, client_ip, port, timeout).to_con()?;
                udp::split_multiplexed(socket, max_packet_size).to_con()?
            }
            SocketProtocol::Tcp => {
                let socket = tcp::connect_to_client(timeout, &[client_ip], port, buffer_config)?;
                tcp::split_multiplexed(socket, timeout).to_con()?
            }
        };

        Ok(StreamSocket {
            send_socket: Arc::new(Mutex::new(send_socket)),
            receive_socket,
            queues: HashMap::new(),
        })
    }
}

pub struct StreamSocket {
    send_socket: Arc<Mutex<Box<dyn MultiplexedSocketWriter + Send>>>,
    receive_socket: Box<dyn MultiplexedSocketReader + Send>,
    queues: HashMap<u16, StreamRecvQueues>,
}

impl StreamSocket {
    pub(crate) fn configure_local_udp_poll(&self, timeout: Duration) -> Result<()> {
        self.receive_socket.configure_local_udp_poll(timeout)
    }
    pub(crate) fn configure_frame_video_fec(&self, enabled: bool) {
        self.send_socket.lock().set_frame_video_fec(enabled);
    }
    pub(crate) fn configure_video_pacing(&self, bytes_per_second: u64) {
        self.send_socket.lock().set_video_pacing(bytes_per_second);
    }
    pub(crate) fn configure_frame_udp_qos(&self, priority: bool) {
        self.send_socket.lock().set_frame_udp_qos(priority);
    }

    pub fn request_stream<T>(&self, stream_id: u16) -> StreamSender<T> {
        StreamSender {
            inner: Arc::clone(&self.send_socket),
            stream_id,
            payload_offset: self.send_socket.lock().payload_offset(),
            next_packet_index: 0,
            used_buffers: vec![],
            _phantom: PhantomData,
        }
    }

    // max_concurrent_buffers: number of buffers allocated by this call which will be reused to
    // receive packets for this stream ID. If packets are not read fast enough, the shards received
    // for this particular stream will be discarded
    pub fn subscribe_to_stream<T>(
        &mut self,
        stream_id: u16,
        max_concurrent_buffers: usize,
    ) -> StreamReceiver<T> {
        let (packet_sender, packet_receiver) = mpsc::channel();
        let (used_buffer_sender, used_buffer_receiver) = mpsc::channel();

        for _ in 0..max_concurrent_buffers {
            used_buffer_sender.send(vec![]).ok();
        }

        self.queues.insert(
            stream_id,
            StreamRecvQueues {
                used_buffer_sender: used_buffer_sender.clone(),
                used_buffer_receiver,
                packet_queue: packet_sender,
            },
        );

        StreamReceiver {
            payload_offset: self.receive_socket.payload_offset(),
            packet_receiver,
            used_buffer_queue: used_buffer_sender,
            last_packet_index: None,
            _phantom: PhantomData,
        }
    }

    pub fn recv(&mut self) -> ConResult {
        self.receive_socket.recv(&self.queues)
    }
}
