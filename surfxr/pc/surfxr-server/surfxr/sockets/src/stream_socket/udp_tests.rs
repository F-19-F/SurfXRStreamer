//! Real UDP sockets on reserved ephemeral loopback endpoints; no Streamer ports.
use super::*;
use crate::{ProtoControlSocket, SocketConnection};
use std::net::TcpStream;
use std::time::Instant;

fn pair(priority: bool) -> (StreamSocket, UdpSocket) {
    let pc = UdpSocket::bind("127.0.0.1:0").unwrap();
    let frame = UdpSocket::bind("127.0.0.1:0").unwrap();
    pc.connect(frame.local_addr().unwrap()).unwrap();
    frame.connect(pc.local_addr().unwrap()).unwrap();
    pc.set_read_timeout(Some(Duration::from_millis(20)))
        .unwrap();
    frame
        .set_read_timeout(Some(Duration::from_millis(500)))
        .unwrap();
    let (send_socket, receive_socket) = if priority {
        udp::split_priority(pc, 1400).unwrap()
    } else {
        udp::split_multiplexed(pc, 1400).unwrap()
    };
    (
        StreamSocket {
            send_socket: Arc::new(Mutex::new(send_socket)),
            receive_socket,
            queues: HashMap::new(),
        },
        frame,
    )
}

fn shard(stream: u16, index: u32, count: u32, part: u32, payload: &[u8]) -> Vec<u8> {
    let mut bytes = Vec::new();
    bytes.extend(stream.to_le_bytes());
    bytes.extend(index.to_le_bytes());
    bytes.extend(count.to_le_bytes());
    bytes.extend(part.to_le_bytes());
    bytes.extend(payload);
    bytes
}

#[test]
fn released_frame_udp_owners_allow_rebinding_the_same_endpoint() {
    for exported in [false, true] {
        let pc = UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = pc.local_addr().unwrap();
        let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
        pc.connect(peer.local_addr().unwrap()).unwrap();
        let (writer, reader) = udp::split_multiplexed(pc, 1400).unwrap();
        let socket = StreamSocket {
            send_socket: Arc::new(Mutex::new(writer)),
            receive_socket: reader,
            queues: HashMap::new(),
        };
        socket.configure_frame_udp_qos(false);
        let sender = socket.request_stream::<()>(3).external_owner_endpoint();
        #[cfg(windows)]
        if exported {
            let raw = sender.frame_udp_socket_for_process().unwrap();
            #[link(name = "kernel32")]
            unsafe extern "system" {
                fn GetHandleInformation(handle: usize, flags: *mut u32) -> i32;
            }
            let mut flags = 0;
            assert_ne!(unsafe { GetHandleInformation(raw, &mut flags) }, 0);
            assert_eq!(flags & 1, 0, "Frame UDP writer is inheritable");
            sender.restore_frame_udp_qos();
        }
        #[cfg(not(windows))]
        let _ = exported;
        drop(socket);
        drop(sender);
        UdpSocket::bind(address).expect("previous Frame session retained its UDP endpoint");
    }
}

#[test]
#[cfg(windows)]
fn exported_frame_udp_socket_closes_after_child_and_parent_exit() {
    use std::{io::{Read, Write}, os::windows::io::FromRawSocket, process::{Command, Stdio}};
    #[link(name = "ws2_32")]
    unsafe extern "system" {
        fn WSADuplicateSocketW(socket: usize, process: u32, info: *mut u8) -> i32;
        fn WSASocketW(family: i32, kind: i32, protocol: i32, info: *const u8, group: u32, flags: u32) -> usize;
    }
    #[repr(C, align(8))]
    struct Protocol([u8; 1024]);
    if std::env::var_os("SURFXR_UDP_LIFETIME_TEST_CHILD").is_some() {
        let initialize_winsock = UdpSocket::bind("127.0.0.1:0").unwrap();
        drop(initialize_winsock);
        let mut protocol = Protocol([0; 1024]);
        std::io::stdin().read_exact(&mut protocol.0).unwrap();
        let raw = unsafe { WSASocketW(-1, -1, -1, protocol.0.as_ptr(), 0, 1 | 0x80) };
        assert_ne!(raw, usize::MAX);
        let socket = unsafe { UdpSocket::from_raw_socket(raw as _) };
        let mut sender = StreamSender::<()>::from_external_frame_udp(socket, 1400, true, 0).unwrap();
        sender.send_header_with_payload(&(), &[0; 1920]).unwrap();
        drop(sender);
        return;
    }
    let pc = UdpSocket::bind("127.0.0.1:0").unwrap();
    let address = pc.local_addr().unwrap();
    let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
    pc.connect(peer.local_addr().unwrap()).unwrap();
    let (writer, reader) = udp::split_multiplexed(pc, 1400).unwrap();
    let socket = StreamSocket {
        send_socket: Arc::new(Mutex::new(writer)), receive_socket: reader, queues: HashMap::new(),
    };
    socket.configure_frame_udp_qos(false);
    let sender = socket.request_stream::<()>(3).external_owner_endpoint();
    let raw = sender.frame_udp_socket_for_process().unwrap();
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "stream_socket::udp_tests::exported_frame_udp_socket_closes_after_child_and_parent_exit"])
        .env("SURFXR_UDP_LIFETIME_TEST_CHILD", "1")
        .stdin(Stdio::piped()).stdout(Stdio::null()).spawn().unwrap();
    let mut protocol = Protocol([0; 1024]);
    assert_eq!(unsafe { WSADuplicateSocketW(raw, child.id(), protocol.0.as_mut_ptr()) }, 0);
    child.stdin.take().unwrap().write_all(&protocol.0).unwrap();
    assert!(child.wait().unwrap().success());
    drop(socket);
    drop(sender);
    UdpSocket::bind(address).expect("exported Frame session retained its UDP endpoint");
}

#[test]
fn external_video_owner_keeps_fifo_indices_fec_and_payload_bytes() {
    let pc = UdpSocket::bind("127.0.0.1:0").unwrap();
    let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
    pc.connect(peer.local_addr().unwrap()).unwrap();
    peer.connect(pc.local_addr().unwrap()).unwrap();
    peer.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
    let mut sender = StreamSender::<()>::from_external_frame_udp(pc, 256, true, 1_000_000).unwrap();
    assert_eq!(sender.frame_udp_priority(), Some(false));
    for index in 0..2u32 {
        let payload: Vec<_> = (0..700).map(|i| ((i * 37 + index) % 251) as u8).collect();
        sender.send_header_with_payload(&(), &payload).unwrap();
        let mut restored = Vec::new();
        let mut parity = [0u8; 238];
        for part in 0..4u32 {
            let mut datagram = [0u8; 256];
            let size = peer.recv(&mut datagram).unwrap();
            assert_eq!(&datagram[..14], &shard(3, index, 3, part, &[]));
            if part < 3 {
                for (sum, byte) in parity.iter_mut().zip(&datagram[14..size]) { *sum ^= byte; }
                restored.extend_from_slice(&datagram[14..size]);
            } else {
                assert_eq!(&datagram[14..18], &700u32.to_le_bytes());
                assert_eq!(&datagram[18..size], &parity);
            }
        }
        assert_eq!(restored, payload);
    }
}

#[test]
fn direct_video_callback_keeps_wire_fec_pacing_and_failure_indices() {
    let received=Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
    let output=Arc::clone(&received);
    let mut sender=StreamSender::<()>::from_frame_callback(move|bytes|{output.lock().push(bytes.to_vec());Ok(())},256,true,1_000_000).unwrap();
    assert_eq!(sender.frame_udp_priority(),Some(false));
    for index in 0..2u32 {
        let payload:Vec<_>=(0..700).map(|i|((i*37+index)%251) as u8).collect();
        sender.send_header_with_payload(&(),&payload).unwrap();
        let mut packets=received.lock();assert_eq!(packets.len(),4);
        let mut restored=Vec::new();let mut parity=[0u8;238];
        for (part,packet) in packets.iter().enumerate() {
            assert_eq!(&packet[..14],&shard(3,index,3,part as u32,&[]));
            if part<3 {restored.extend_from_slice(&packet[14..]);for(sum,byte)in parity.iter_mut().zip(&packet[14..]){*sum^=byte;}}
            else {assert_eq!(&packet[14..18],&700u32.to_le_bytes());assert_eq!(&packet[18..],&parity);}
        }
        assert_eq!(restored,payload);packets.clear();
    }
    // Reuse the real shard writer on failure, including rollover and a
    // partially delivered frame. The callback never keeps a borrowed slice.
    let indices=Arc::new(Mutex::new(Vec::new()));let output=Arc::clone(&indices);
    let mut sender=StreamSender::<()>::from_frame_callback(move|bytes|{
        let mut indices=output.lock();indices.push(u32::from_le_bytes(bytes[2..6].try_into().unwrap()));
        if indices.len()==2 {surfxr_common::anyhow::bail!("fixture partial send failure");}Ok(())
    },256,false,0).unwrap();
    sender.next_packet_index=u32::MAX;
    assert!(sender.send_header_with_payload(&(),&[7;500]).is_err());
    sender.send_header_with_payload(&(),&[8]).unwrap();
    assert_eq!(&*indices.lock(),&[u32::MAX,u32::MAX,0]);
}

#[test]
fn video_fec_uses_real_writer_and_exports_cross_language_fixture() {
    let (socket, peer) = pair(false);
    socket.configure_frame_video_fec(true);
    let payload: Vec<u8> = (0..50123usize).map(|i| (i * 37 + i / 101) as u8).collect();
    let mut sender = socket.request_stream::<()>(3);
    let send = std::thread::spawn(move || sender.send_header_with_payload(&(), &payload));
    let count = 50123usize.div_ceil(1382);
    let mut fixture = Vec::new();
    let mut parts = HashMap::new();
    for _ in 0..count + count.div_ceil(16) {
        let mut packet = [0u8; 1400];
        let n = peer.recv(&mut packet).unwrap();
        assert_eq!(u16::from_le_bytes(packet[..2].try_into().unwrap()), 3);
        assert_eq!(u32::from_le_bytes(packet[2..6].try_into().unwrap()), 0);
        assert_eq!(u32::from_le_bytes(packet[6..10].try_into().unwrap()), count as u32);
        let part = u32::from_le_bytes(packet[10..14].try_into().unwrap()) as usize;
        assert!(parts.insert(part, packet[..n].to_vec()).is_none());
        fixture.extend_from_slice(&(n as u16).to_le_bytes());
        fixture.extend_from_slice(&packet[..n]);
    }
    send.join().unwrap().unwrap();
    for block in 0..count.div_ceil(16) {
        let parity = &parts[&(count + block)];
        assert_eq!(parity.len(), 1400);
        assert_eq!(u32::from_le_bytes(parity[14..18].try_into().unwrap()), 50123);
        let mut expected = vec![0u8; 1382];
        for part in block * 16..usize::min(block * 16 + 16, count) {
            let data = &parts[&part];
            for (sum, byte) in expected.iter_mut().zip(&data[14..]) { *sum ^= byte; }
        }
        assert_eq!(&parity[18..], &expected);
    }
    if let Ok(path) = std::env::var("SURFXR_FRAME_FEC_FIXTURE") {
        std::fs::write(path, fixture).unwrap();
    }
}

#[test]
fn failed_send_never_reuses_a_partially_transmitted_packet_index() {
    struct PartialFailure { indices: Arc<Mutex<Vec<u32>>> }
    impl MultiplexedSocketWriter for PartialFailure {
        fn payload_offset(&self) -> usize { 14 }
        fn send(&mut self, _: u16, index: u32, _: &mut Vec<u8>) -> Result<()> {
            let mut indices = self.indices.lock();
            indices.push(index);
            if indices.len() == 1 { surfxr_common::anyhow::bail!("simulated failure after a shard"); }
            Ok(())
        }
    }
    let indices = Arc::new(Mutex::new(Vec::new()));
    let mut sender = StreamSender::<()> {
        inner: Arc::new(Mutex::new(Box::new(PartialFailure { indices: Arc::clone(&indices) }))),
        stream_id: 3, payload_offset: 14, next_packet_index: u32::MAX,
        used_buffers: vec![], _phantom: PhantomData,
    };
    assert!(sender.send_header_with_payload(&(), &[7]).is_err());
    sender.send_header_with_payload(&(), &[8]).unwrap();
    assert_eq!(&*indices.lock(), &[u32::MAX, 0]);
}

#[test]
fn udp_priority_bypasses_video_lock_and_recovers_reordered_tracking() {
    let (video, frame_video) = pair(false);
    let (priority, frame_priority) = pair(true);
    let control_listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let control_peer = TcpStream::connect(control_listener.local_addr().unwrap()).unwrap();
    let control = control_listener.accept().unwrap().0;
    let mut connection = SocketConnection {
        control_socket: ProtoControlSocket { inner: control },
        stream_socket: video,
        priority_socket: Some(priority),
    };
    let mut audio = connection.request_unreliable_stream::<()>(2);
    let mut tracking = connection.subscribe_to_unreliable_stream::<u64>(0, 2);
    // Hold the production video writer lock for the entire assertion. UDP
    // receive loss does not reliably backpressure a sender, so a lock fixture
    // gives a deterministic reproduction of a long video shard send.
    let video_writer = Arc::clone(&connection.stream_socket.send_socket);
    let video_lock = video_writer.lock();
    let started = Instant::now();
    let send_task = std::thread::spawn(move || audio.send_header_with_payload(&(), &[1, 2, 3, 4]));
    let mut received = [0; 1400];
    let received_size = frame_priority.recv(&mut received);
    if received_size.is_err() {
        drop(video_lock);
        send_task.join().unwrap().unwrap();
        panic!("Priority UDP audio was blocked by video writer");
    }
    let n = received_size.unwrap();
    send_task.join().unwrap().unwrap();
    assert_eq!(u16::from_le_bytes(received[..2].try_into().unwrap()), 2);
    assert_eq!(&received[udp::SHARD_PREFIX_SIZE..n], &[1, 2, 3, 4]);
    let baseline = bincode::serde::encode_to_vec(41u64, config::standard()).unwrap();
    frame_priority.send(&shard(0, 0, 1, 0, &baseline)).unwrap();
    assert!(connection.recv_poll().is_ok());
    assert_eq!(
        tracking
            .recv(Duration::from_millis(50))
            .ok()
            .expect("baseline tracking")
            .get_header()
            .unwrap(),
        41
    );
    // An incomplete video packet must not be polled by the PC priority reader.
    frame_video.send(&shard(3, 0, 2, 0, &[7; 1386])).unwrap();
    frame_priority.send(&shard(0, 1, 2, 0, &[9; 1386])).unwrap();
    let mut payload = bincode::serde::encode_to_vec(42u64, config::standard()).unwrap();
    payload.extend(vec![5; 4000]);
    let parts: Vec<_> = payload.chunks(1386).collect();
    for part in (0..parts.len()).rev() {
        frame_priority
            .send(&shard(0, 2, parts.len() as u32, part as u32, parts[part]))
            .unwrap();
    }
    for _ in 0..1 + parts.len() {
        assert!(connection.recv_poll().is_ok());
    }
    let packet = tracking
        .recv(Duration::from_millis(50))
        .ok()
        .expect("tracking packet");
    let (header, bytes) = packet.get().unwrap();
    assert_eq!(header, 42);
    assert_eq!(bytes, &[5; 4000]);
    assert!(packet.had_packet_loss());
    assert!(started.elapsed() < Duration::from_millis(500));
    connection.configure_frame_daemon_poll().unwrap();
    let idle = Instant::now();
    assert!(connection.recv_poll().is_err());
    assert!(idle.elapsed() < Duration::from_millis(250), "Local daemon receiver retained handshake timeout");
    drop(video_lock);
    drop(control_peer);
}

#[test]
fn udp_priority_rejects_bad_prefixes_before_allocating_and_survives_loss() {
    let (mut socket, peer) = pair(true);
    let mut mic = socket.subscribe_to_stream::<()>(2, 1);
    let invalid = [
        shard(2, 0, u32::MAX, 0, &[1; 1386]),
        shard(2, 0, 1, 1, &[1]),
        shard(2, 0, 2, 0, &[1]),           // short non-final shard
        shard(3, 0, 1, 0, &[1]),           // video in priority socket
        shard(2, 0, 1, 0, &[1; 1387]),     // exceeds negotiated datagram size
        shard(2, 0, 139, 138, &[1; 1386]), // exceeds 192000 bytes
    ];
    for bytes in invalid {
        peer.send(&bytes).unwrap();
        assert!(socket.recv().is_err());
        // Rejection has not removed the sole reusable buffer from its pool.
        assert_eq!(
            socket.queues[&2]
                .used_buffer_receiver
                .try_recv()
                .unwrap()
                .capacity(),
            0
        );
        socket.queues[&2].used_buffer_sender.send(vec![]).unwrap();
    }
    peer.send(&shard(2, 0, 2, 0, &[1; 1386])).unwrap();
    assert!(socket.recv().is_ok());
    // With one pooled buffer, a lost packet must be recycled for the next PCM.
    peer.send(&shard(2, 1, 1, 0, &[2; 1920])).unwrap();
    // That packet is too large for the MTU and is discarded without delivery.
    assert!(socket.recv().is_err());
    peer.send(&shard(2, 1, 1, 0, &[2; 8])).unwrap();
    assert!(socket.recv().is_ok());
    let packet = mic
        .recv(Duration::from_millis(50))
        .ok()
        .expect("microphone packet");
    assert_eq!(packet.get().unwrap().1, &[2; 8]);
}

#[test]
fn paced_frame_video_preserves_every_shard_and_payload() {
    let (video, peer) = pair(false);
    video.configure_video_pacing(1_000_000);
    let expected: Vec<u8> = (0..65536).map(|i| (i % 251) as u8).collect();
    let outgoing = expected.clone();
    let task = std::thread::spawn(move || {
        let started = Instant::now();
        video
            .request_stream::<()>(3)
            .send_header_with_payload(&(), &outgoing)
            .unwrap();
        started.elapsed()
    });
    let mut restored = Vec::new();
    let mut bytes = [0; 1400];
    let count = expected.len().div_ceil(1386);
    for part in 0..count {
        let size = peer.recv(&mut bytes).unwrap();
        assert_eq!(&bytes[..2], &3u16.to_le_bytes());
        assert_eq!(&bytes[2..6], &0u32.to_le_bytes());
        assert_eq!(&bytes[6..10], &(count as u32).to_le_bytes());
        assert_eq!(&bytes[10..14], &(part as u32).to_le_bytes());
        restored.extend_from_slice(&bytes[14..size]);
    }
    let elapsed = task.join().unwrap();
    assert_eq!(restored, expected);
    assert!(elapsed >= Duration::from_millis(20));
    assert!(elapsed < Duration::from_secs(3));
}

#[test]
fn frame_udp_qos_is_scoped_optional_and_keeps_original_packet_bytes() {
    for priority in [false, true] {
        let (socket, peer) = pair(priority);
        let mut sender = socket.request_stream::<()>(if priority { 2 } else { 3 });
        assert_eq!(sender.frame_udp_priority(), None);
        socket.configure_frame_udp_qos(priority);
        let qos = super::frame_qos::FrameQos::new(&peer, priority);
        // Unsupported QoS is nonfatal; an invalid API argument is a bug.
        assert_ne!(qos.error, 87);
        assert_eq!(sender.frame_udp_priority(), Some(priority));
        let task = std::thread::spawn(move || {
            // Created/dropped inside the same worker, never transferred.
            let _scheduling = sender.prioritize_frame_udp_thread();
            sender.send_header_with_payload(&(), &[7; 1920]).unwrap();
        });
        let mut restored = Vec::new();
        let mut bytes = [0; 1400];
        for part in 0..2 {
            let n = peer.recv(&mut bytes).unwrap();
            assert_eq!(&bytes[2..6], &0u32.to_le_bytes());
            assert_eq!(&bytes[10..14], &(part as u32).to_le_bytes());
            restored.extend_from_slice(&bytes[14..n]);
        }
        task.join().unwrap();
        assert_eq!(restored, vec![7; 1920]);
    }
}
