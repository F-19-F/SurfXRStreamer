use super::{
    MultiplexedSocketReader, MultiplexedSocketWriter, ReconstructedPacket, StreamRecvQueues,
};
use crate::LOCAL_IP;
use surfxr_common::{
    ConResult, HandleTryAgain, ToCon,
    anyhow::{Result, ensure},
};
use surfxr_session::{DscpTos, SocketBufferConfig};
use socket2::{MaybeUninitSlice, Socket};
use std::ffi::c_int;
use std::{
    cmp::Ordering,
    collections::{HashMap, HashSet},
    mem::{self, MaybeUninit},
    net::{IpAddr, UdpSocket},
    ptr,
    time::Duration,
};

pub const SHARD_PREFIX_SIZE: usize = mem::size_of::<u16>() // stream ID
    + mem::size_of::<u32>() // packet index
    + mem::size_of::<u32>() // shards count
    + mem::size_of::<u32>(); // shards index

#[cfg(windows)]
fn prevent_inheritance(socket: &UdpSocket) -> Result<()> {
    use std::os::windows::io::AsRawSocket;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn SetHandleInformation(handle: usize, mask: u32, flags: u32) -> i32;
    }
    if unsafe { SetHandleInformation(socket.as_raw_socket() as usize, 1, 0) } == 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(())
}

fn socket_peek(socket: &mut Socket, buffer: &mut [u8]) -> ConResult<usize> {
    #[cfg(windows)]
    const FLAGS: c_int = 0x02 | 0x8000; // MSG_PEEK | MSG_PARTIAL
    #[cfg(not(windows))]
    const FLAGS: c_int = 0x02 | 0x20; // MSG_PEEK | MSG_TRUNC

    let buffer =
        MaybeUninitSlice::new(unsafe { &mut *(ptr::from_mut(buffer) as *mut [MaybeUninit<u8>]) });
    // NB: Using the non vectored call doesn't seem to work
    Ok(socket
        .recv_vectored_with_flags(&mut [buffer], FLAGS)
        .handle_try_again()?
        .0)
}

// Create tokio socket, convert to socket2, apply settings, convert back to tokio. This is done to
// let tokio set all the internal parameters it needs from the start.
pub fn bind(
    port: u16,
    dscp: Option<DscpTos>,
    buffer_config: SocketBufferConfig,
) -> Result<UdpSocket> {
    bind_on(LOCAL_IP,port,dscp,buffer_config)
}
pub fn bind_on(local:IpAddr,port:u16,dscp:Option<DscpTos>,buffer_config:SocketBufferConfig)->Result<UdpSocket>{
    let socket = UdpSocket::bind((local, port))?;
    #[cfg(windows)]
    prevent_inheritance(&socket)?;
    let socket = socket.into();

    crate::set_socket_buffers(&socket, buffer_config).ok();
    crate::set_dscp(&socket, dscp);

    Ok(socket.into())
}

pub fn connect(socket: &UdpSocket, peer_ip: IpAddr, port: u16, timeout: Duration) -> Result<()> {
    socket.connect((peer_ip, port))?;
    socket.set_read_timeout(Some(timeout))?;

    Ok(())
}

// The callback borrows one shard synchronously; the negotiated daemon owns
// its physical socket and must join this writer before releasing the owner.
enum DatagramSink {
    Socket(UdpSocket),
    Callback(Box<dyn Fn(&[u8]) -> Result<()> + Send>),
}
impl DatagramSink {
    fn socket(&self) -> Option<&UdpSocket> { match self { Self::Socket(socket) => Some(socket), _ => None } }
    fn send(&self, bytes: &[u8]) -> Result<usize> {
        match self { Self::Socket(socket) => Ok(socket.send(bytes)?), Self::Callback(send) => {send(bytes)?;Ok(bytes.len())} }
    }
}
pub struct MultiplexedUdpWriter {
    inner: DatagramSink,
    max_packet_size: usize,
    payload_limit: Option<usize>,
    video_pacer: Option<super::pacing::VideoPacer>,
    video_fec: Option<super::video_fec::VideoParity>,
    frame_qos: Option<super::frame_qos::FrameQos>,
    timing: super::udp_timing::UdpTiming,
}

impl MultiplexedSocketWriter for MultiplexedUdpWriter {
    #[cfg(windows)]
    fn frame_udp_socket_for_process(&mut self) -> Option<usize> {
        use std::os::windows::io::AsRawSocket;
        // WSADuplicateSocket cannot export a QoS-enabled socket. The child
        // registers its own flow after importing it; priority stays separate.
        self.frame_qos = None;
        self.inner.socket().map(|socket|socket.as_raw_socket() as usize)
    }
    fn set_frame_video_fec(&mut self, enabled: bool) {
        self.video_fec = enabled.then(|| super::video_fec::VideoParity::new(self.max_packet_size));
    }
    fn set_frame_udp_qos(&mut self, priority: bool) {
        self.frame_qos = self.inner.socket().map(|socket|super::frame_qos::FrameQos::new(socket, priority));
    }
    fn frame_udp_priority(&self) -> Option<bool> {
        if self.inner.socket().is_none() { return Some(false); }
        self.frame_qos
            .as_ref()
            .map(|_| self.payload_limit.is_some())
    }
    fn set_video_pacing(&mut self, bytes_per_second: u64) {
        self.video_pacer =
            (bytes_per_second > 0).then(|| super::pacing::VideoPacer::new(bytes_per_second));
    }

    fn update_video_bitrate(&mut self, bitrate_bps: u64, overhead: u32) {
        if let Some(pacer) = &mut self.video_pacer {
            let size = self.max_packet_size as u64;
            pacer.update_bitrate(bitrate_bps, size, overhead as u64);
        }
    }

    fn payload_offset(&self) -> usize {
        SHARD_PREFIX_SIZE
    }

    fn send(&mut self, stream_id: u16, packet_index: u32, buffer: &mut Vec<u8>) -> Result<()> {
        let started = std::time::Instant::now();
        let fec = stream_id == 3 && self.video_fec.is_some();
        let max_shard_size = self.max_packet_size - SHARD_PREFIX_SIZE
            - if fec { super::video_fec::LENGTH_BYTES } else { 0 };
        let payload_size = buffer.len() - SHARD_PREFIX_SIZE;
        if let Some(limit) = self.payload_limit {
            ensure!(
                payload_size <= limit,
                "Frame priority UDP packet exceeds limit"
            );
        }
        // rounding up:
        let shards_count = payload_size.div_ceil(max_shard_size);
        ensure!(!fec || payload_size <= 8 * 1024 * 1024, "Frame video FEC packet exceeds limit");

        for shard_idx in 0..shards_count {
            if stream_id == 3
                && shard_idx % 4 == 0
                && let Some(pacer) = &mut self.video_pacer
            {
                pacer.wait_batch();
            }
            // this overlaps with the previous shard, this is intended behavior and allows to
            // reduce allocations
            let shard_start_position = shard_idx * max_shard_size;
            let shard_size = usize::min(max_shard_size, payload_size - shard_start_position);

            let shard_view = &mut buffer[shard_start_position..][..SHARD_PREFIX_SIZE + shard_size];

            shard_view[0..2].copy_from_slice(&stream_id.to_le_bytes());
            shard_view[2..6].copy_from_slice(&packet_index.to_le_bytes());
            shard_view[6..10].copy_from_slice(&(shards_count as u32).to_le_bytes());
            shard_view[10..14].copy_from_slice(&(shard_idx as u32).to_le_bytes());

            self.inner.send(shard_view)?;
            if stream_id == 3
                && let Some(pacer) = &mut self.video_pacer
            {
                pacer.charge(shard_view.len());
            }
            if fec && let Some(parity) = self.video_fec.as_mut().unwrap().accumulate(
                &shard_view[SHARD_PREFIX_SIZE..], payload_size, packet_index, shards_count, shard_idx,
            ) {
                self.inner.send(parity)?;
                if let Some(pacer) = &mut self.video_pacer { pacer.charge(parity.len()); }
            }
        }

        if let Some(qos) = &self.frame_qos {
            self.timing.packet(stream_id, started, payload_size, qos);
        }
        Ok(())
    }
}

// We need to store the size seaparately because we use use the buffer as unallocated memory and
// the capacity cannot be set precisely.
// Note: Why do we need to keep space for the prefix in the final buffer?
// UDP works with discrete packets, reading must be done in a single call. There is no way to
// preemptively discard the bytes of the prefix.
struct InProgressPacket {
    buffer: Vec<u8>,    // contains the prefix
    buffer_size: usize, // size of the packet counting prefix
    shards_count: usize,
    received_shard_indices: HashSet<usize>,
}

pub struct MultiplexedUdpReader {
    inner: Socket,
    max_packet_size: usize,
    in_progress_packets: HashMap<u16, HashMap<u32, InProgressPacket>>,
    payload_limit: Option<usize>,
    peek_buffer: Vec<u8>,
}

impl MultiplexedSocketReader for MultiplexedUdpReader {
    fn configure_local_udp_poll(&self, timeout: Duration) -> Result<()> {
        self.inner.set_read_timeout(Some(timeout))?;
        Ok(())
    }
    fn payload_offset(&self) -> usize {
        SHARD_PREFIX_SIZE
    }

    fn recv(&mut self, stream_queues: &HashMap<u16, StreamRecvQueues>) -> ConResult {
        let max_shard_data_size = self.max_packet_size - SHARD_PREFIX_SIZE;

        let discard_and_try_again = move |socket: &Socket| {
            // Reading with any sized buffer (even 0) will consume the whole datagram
            socket.recv(&mut []).ok();
            surfxr_common::try_again()
        };

        let peek_size = socket_peek(&mut self.inner, &mut self.peek_buffer)?;
        if peek_size < SHARD_PREFIX_SIZE {
            return discard_and_try_again(&self.inner);
        }

        // The values obtained from the prefix (stream ID, packet index, shards count, shard index)
        // could be corrupted somehow. This method has safety checks against corrupted values and
        // the relative packet would be discarded.
        let prefix_bytes = &self.peek_buffer;
        let stream_id = u16::from_le_bytes(prefix_bytes[0..2].try_into().unwrap());
        let packet_index = u32::from_le_bytes(prefix_bytes[2..6].try_into().unwrap());
        let maybe_shards_count =
            u32::from_le_bytes(prefix_bytes[6..10].try_into().unwrap()) as usize;
        let shard_index = u32::from_le_bytes(prefix_bytes[10..14].try_into().unwrap()) as usize;

        if maybe_shards_count == 0 {
            return discard_and_try_again(&self.inner);
        }
        // Priority shards are validated before taking/reallocating a pooled
        // buffer. Full datagram peek also excludes uninitialized gaps caused by
        // short non-final shards or datagrams larger than the negotiated MTU.
        if let Some(limit) = self.payload_limit {
            if !matches!(stream_id, 0 | 2 | 4)
                || peek_size <= SHARD_PREFIX_SIZE
                || peek_size > self.max_packet_size
                || maybe_shards_count > limit.div_ceil(max_shard_data_size)
                || shard_index >= maybe_shards_count
                || shard_index * max_shard_data_size + peek_size - SHARD_PREFIX_SIZE > limit
                || (shard_index + 1 < maybe_shards_count && peek_size != self.max_packet_size)
            {
                return discard_and_try_again(&self.inner);
            }
        }

        let Some(queues) = stream_queues.get(&stream_id) else {
            return discard_and_try_again(&self.inner);
        };
        let in_progress_packets = self.in_progress_packets.entry(stream_id).or_default();

        let in_progress_packet = if let Some(packet) = in_progress_packets.get_mut(&packet_index) {
            packet
        } else if let Some(mut buffer) = queues.used_buffer_receiver.try_recv().ok().or_else(|| {
            // By default, try to dequeue a used buffer. In case none were found, recycle one of the
            // in progress packets, chances are these buffers are "dead" because one of their shards
            // has been dropped by the network.
            let idx = *in_progress_packets.iter().next()?.0;
            Some(in_progress_packets.remove(&idx).unwrap().buffer)
        }) {
            // The first shard prefix will dictate the actual number of shards of the packet. The
            // reserved capacity is an upper bound: we don't know yet the exact size, the last
            // shard could be smaller than max_shard_data_size
            buffer.clear();
            let capacity = max_shard_data_size * maybe_shards_count;
            buffer.reserve(
                SHARD_PREFIX_SIZE
                    + self
                        .payload_limit
                        .map_or(capacity, |limit| capacity.min(limit)),
            );

            in_progress_packets
                .entry(packet_index)
                .or_insert(InProgressPacket {
                    buffer,
                    buffer_size: 0,
                    shards_count: maybe_shards_count,
                    // todo: find a way to skipping this allocation
                    received_shard_indices: HashSet::with_capacity(maybe_shards_count),
                })
        } else {
            // This branch may be hit in case the thread related to the stream hangs for some reason
            return discard_and_try_again(&self.inner);
        };

        if shard_index >= in_progress_packet.shards_count
            || (self.payload_limit.is_some()
                && maybe_shards_count != in_progress_packet.shards_count)
            || in_progress_packet
                .received_shard_indices
                .contains(&shard_index)
        {
            return discard_and_try_again(&self.inner);
        }

        // Note: there is no prefix offset, since we want to write the prefix too.
        let packet_start_index = shard_index * max_shard_data_size;

        // Note: this is a MaybeUninit slice
        let sub_buffer = &mut in_progress_packet.buffer.spare_capacity_mut()[packet_start_index..];

        // Safety: bound checks lead from the previous code
        let overwritten_data_backup: [_; SHARD_PREFIX_SIZE] =
            sub_buffer[..SHARD_PREFIX_SIZE].try_into().unwrap();

        // This call should never fail because the peek call succeded before.
        // Note: in unexpected circumstances, here .to_con() is used not to emit TryAgain, which
        // would mess with the state of the code. The connection would need to be closed instead.
        // NB: the received_size contains the prefix
        let received_size = self.inner.recv(sub_buffer).to_con()?;

        // Restore backed up bytes
        sub_buffer[..SHARD_PREFIX_SIZE].copy_from_slice(&overwritten_data_backup);

        in_progress_packet.buffer_size = usize::max(
            in_progress_packet.buffer_size,
            packet_start_index + received_size,
        );

        in_progress_packet
            .received_shard_indices
            .insert(shard_index);

        // Check if packet is complete (and not dummy) and send
        if in_progress_packet.received_shard_indices.len() == in_progress_packet.shards_count {
            if let Some(mut packet) = in_progress_packets.remove(&packet_index) {
                // All writing was done to uninit capacity, here we set the final buffer length
                unsafe { packet.buffer.set_len(packet.buffer_size) };

                queues
                    .packet_queue
                    .send(ReconstructedPacket {
                        index: packet_index,
                        buffer: packet.buffer,
                    })
                    .ok();
            }

            // Discard older in-progress packets
            while let Some((idx, _)) = in_progress_packets
                .iter()
                .find(|(idx, _)| super::wrapping_cmp(**idx, packet_index) == Ordering::Less)
            {
                let idx = *idx; // fix borrow rule
                let packet = in_progress_packets.remove(&idx).unwrap();

                // Recycle buffer
                queues.used_buffer_sender.send(packet.buffer).ok();
            }
        }

        Ok(())
    }
}

pub fn split_multiplexed(
    socket: UdpSocket,
    max_packet_size: usize,
) -> Result<(
    Box<dyn MultiplexedSocketWriter + Send>,
    Box<dyn MultiplexedSocketReader + Send>,
)> {
    split(socket, max_packet_size, None)
}

pub fn split_priority(
    socket: UdpSocket,
    max_packet_size: usize,
) -> Result<(
    Box<dyn MultiplexedSocketWriter + Send>,
    Box<dyn MultiplexedSocketReader + Send>,
)> {
    ensure!(
        (64..=65503).contains(&max_packet_size),
        "Invalid Frame priority UDP packet size"
    );
    split(socket, max_packet_size, Some(192000))
}

fn split(
    socket: UdpSocket,
    max_packet_size: usize,
    payload_limit: Option<usize>,
) -> Result<(
    Box<dyn MultiplexedSocketWriter + Send>,
    Box<dyn MultiplexedSocketReader + Send>,
)> {
    #[cfg(windows)]
    prevent_inheritance(&socket)?;
    let send_socket: UdpSocket = socket2::SockRef::from(&socket).try_clone()?.into();
    #[cfg(windows)]
    prevent_inheritance(&send_socket)?;
    let writer = MultiplexedUdpWriter {
        // Keep both handles out of unrelated children so teardown frees ports.
        inner: DatagramSink::Socket(send_socket),
        max_packet_size,
        payload_limit,
        video_pacer: None,
        video_fec: None,
        frame_qos: None,
        timing: super::udp_timing::UdpTiming::new(),
    };

    let reader = MultiplexedUdpReader {
        inner: socket.into(),
        max_packet_size,
        in_progress_packets: HashMap::new(),
        payload_limit,
        peek_buffer: vec![
            0;
            if payload_limit.is_some() {
                max_packet_size + 1
            } else {
                SHARD_PREFIX_SIZE
            }
        ],
    };

    Ok((Box::new(writer), Box::new(reader)))
}

pub(super) fn direct_writer(send:impl Fn(&[u8])->Result<()>+Send+'static,max_packet_size:usize)->Result<Box<dyn MultiplexedSocketWriter+Send>> {
    ensure!((64..=65503).contains(&max_packet_size),"Invalid Frame direct packet size");
    Ok(Box::new(MultiplexedUdpWriter {
        inner:DatagramSink::Callback(Box::new(send)),max_packet_size,payload_limit:None,
        video_pacer:None,video_fec:None,frame_qos:None,timing:super::udp_timing::UdpTiming::new(),
    }))
}
