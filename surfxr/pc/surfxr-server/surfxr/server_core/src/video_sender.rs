//! Encoding hands immutable source metadata to a socket-owning worker.
//! This worker never reads SessionManager or TrackingManager, including during
//! startup, settings persistence and teardown. Encoded reference order is FIFO.
use crate::haptics::FrameVideoHaptics;
use surfxr_common::{info, parking_lot::Mutex};
use surfxr_packets::VideoPacketHeader;
use surfxr_sockets::{FrameVideoHeader, StreamSender};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc::{Receiver, RecvTimeoutError},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

pub struct VideoPacket {
    // Snapshot from the encoder owner; no settings lock on the socket thread.
    pub pacing_bitrate: Option<(u64, u32)>,
    pub image_timestamp_ns: Option<u64>,
    pub gaze_crops: Option<[[u32; 2]; 2]>,
    pub header: VideoPacketHeader,
    pub payload: Vec<u8>,
    pub queued_at: Instant,
}

pub struct VideoSendWorker {
    stopping: Arc<AtomicBool>,
    handle: JoinHandle<()>,
}

impl VideoSendWorker {
    pub fn spawn(
        mut sender: StreamSender<FrameVideoHeader>,
        receiver: Receiver<VideoPacket>,
        haptics: Arc<Mutex<Option<FrameVideoHaptics>>>,
        video_haptics: bool,
        image_timestamps: bool,
    ) -> Self {
        let stopping = Arc::new(AtomicBool::new(false));
        let worker_stopping = Arc::clone(&stopping);
        let handle = thread::spawn(move || {
            let _scheduling = sender.prioritize_frame_udp_thread();
            let mut timing = VideoQueueTiming::new();
            while !worker_stopping.load(Ordering::Acquire) {
                match receiver.recv_timeout(Duration::from_millis(50)) {
                    Ok(packet) => {
                        // Stop takes precedence over an already queued image.
                        if worker_stopping.load(Ordering::Acquire) {
                            break;
                        }
                        if let Some((bitrate, overhead)) = packet.pacing_bitrate {
                            sender.update_video_bitrate(bitrate, overhead);
                        }
                        let amplitudes = video_haptics.then(|| {
                            haptics
                                .lock()
                                .as_mut()
                                .map_or([0.0; 2], |state| state.snapshot(Instant::now()))
                        });
                        let started = Instant::now();
                        let image_timestamp_ns = image_timestamps.then(|| packet.image_timestamp_ns
                            .unwrap_or(packet.header.timestamp.as_nanos() as u64));
                        let header = FrameVideoHeader {
                            video: packet.header,
                            haptics: amplitudes,
                            gaze_crops: packet.gaze_crops,
                            image_timestamp_ns,
                        };
                        let success = sender
                            .send_header_with_payload(&header, &packet.payload)
                            .is_ok();
                        timing.packet(packet.queued_at, started, success);
                    }
                    Err(RecvTimeoutError::Timeout) => (),
                    Err(RecvTimeoutError::Disconnected) => break,
                }
                timing.report();
            }
        });
        Self { stopping, handle }
    }

    pub fn request_stop(&self) {
        self.stopping.store(true, Ordering::Release);
    }
    pub fn join(self) {
        self.handle.join().ok();
    }
}

struct VideoQueueTiming {
    started: Instant,
    last_source: Option<Instant>,
    packets: u64,
    send_errors: u64,
    queue_max_us: u64,
    source_gap_max_us: u64,
}

impl VideoQueueTiming {
    fn new() -> Self {
        Self {
            started: Instant::now(),
            last_source: None,
            packets: 0,
            send_errors: 0,
            queue_max_us: 0,
            source_gap_max_us: 0,
        }
    }
    fn packet(&mut self, queued: Instant, sent: Instant, success: bool) {
        self.packets += 1;
        self.send_errors += u64::from(!success);
        self.queue_max_us = self
            .queue_max_us
            .max(sent.saturating_duration_since(queued).as_micros() as u64);
        if let Some(previous) = self.last_source {
            self.source_gap_max_us = self
                .source_gap_max_us
                .max(queued.saturating_duration_since(previous).as_micros() as u64);
        }
        self.last_source = Some(queued);
    }
    fn report(&mut self) {
        let now = Instant::now();
        if now.duration_since(self.started) < Duration::from_secs(5) {
            return;
        }
        let data = serde_json::json!({
            "seconds":now.duration_since(self.started).as_secs_f64(),
            "packets":self.packets,"send_errors":self.send_errors,
            "queue_max_us":self.queue_max_us,"source_gap_max_us":self.source_gap_max_us,
            "source_idle_us":now.saturating_duration_since(self.last_source.unwrap_or(self.started)).as_micros() as u64,
        });
        info!("Frame video queue timing: {data}");
        self.started = now;
        self.packets = 0;
        self.send_errors = 0;
        self.queue_max_us = 0;
        self.source_gap_max_us = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_common::ViewParams;
    use surfxr_sockets::StreamSocketBuilder;
    use std::net::UdpSocket;

    #[test]
    fn worker_updates_pacing_before_sending_a_live_bitrate_change() {
        let (sent, received) = std::sync::mpsc::channel();
        let sender = StreamSender::<FrameVideoHeader>::from_frame_callback(move |_| {
            sent.send(Instant::now()).unwrap(); Ok(())
        }, 1400, false, 25_000).unwrap();
        let (tx, rx) = std::sync::mpsc::sync_channel(4);
        let worker = VideoSendWorker::spawn(sender, rx, Arc::new(Mutex::new(None)), false, false);
        tx.send(VideoPacket {
            pacing_bitrate: Some((100_000_000, 28)),
            image_timestamp_ns: None, gaze_crops: None, queued_at: Instant::now(),
            header: VideoPacketHeader { timestamp: Duration::from_secs(1),
                global_view_params: [ViewParams::DUMMY; 2], foveation_center_shifts: None, is_idr: true },
            payload: vec![0; 40_000],
        }).unwrap();
        drop(tx);
        worker.join();
        let times: Vec<_> = received.iter().collect();
        assert!(times.len() > 20);
        // The stale startup budget takes over a second. Allow ample scheduler
        // jitter while exercising the actual socket writer, with no real peer.
        assert!(times.last().unwrap().duration_since(times[0]) < Duration::from_millis(400));
    }

    #[test]
    fn frame_video_worker_sends_while_session_persistence_is_locked_and_stops_without_it() {
        // Reserve both dynamic endpoints before starting production code.
        let pc = UdpSocket::bind("127.0.0.1:0").unwrap();
        let frame = UdpSocket::bind("127.0.0.1:0").unwrap();
        frame
            .set_read_timeout(Some(Duration::from_millis(500)))
            .unwrap();
        let socket = StreamSocketBuilder::Udp(pc)
            .accept_from_server(
                frame.local_addr().unwrap().ip(),
                frame.local_addr().unwrap().port(),
                1400,
                Duration::from_millis(50),
            )
            .ok()
            .unwrap();
        let sender = socket.request_stream::<FrameVideoHeader>(surfxr_packets::VIDEO);
        let (tx, rx) = std::sync::mpsc::sync_channel(4);
        let worker = VideoSendWorker::spawn(sender, rx, Arc::new(Mutex::new(None)), true, false);
        let _session = crate::SESSION_MANAGER.write();
        let header = || VideoPacketHeader {
            timestamp: Duration::from_nanos(123456789),
            global_view_params: [ViewParams::DUMMY; 2],
            foveation_center_shifts: None,
            is_idr: true,
        };
        let mut expected = bincode::serde::encode_to_vec(
            FrameVideoHeader {
                video: header(),
                haptics: Some([0.0; 2]),
                gaze_crops: Some([[2, 4], [6, 8]]),
                image_timestamp_ns: None,
            },
            bincode::config::standard(),
        )
        .unwrap();
        expected.extend([1, 2, 3, 4]);
        tx.try_send(VideoPacket {
            pacing_bitrate: None,
            image_timestamp_ns: None,
            header: header(),
            payload: vec![1, 2, 3, 4],
            gaze_crops: Some([[2, 4], [6, 8]]),
            queued_at: Instant::now(),
        })
        .unwrap();
        let mut bytes = [0; 1400];
        let count = frame.recv(&mut bytes).unwrap();
        assert_eq!(
            u16::from_le_bytes(bytes[..2].try_into().unwrap()),
            surfxr_packets::VIDEO
        );
        assert_eq!(&bytes[14..count], &expected);
        // An explicit stop also works while the global lock remains held, even
        // if the producer still owns a sender. No global settings lookup on join.
        worker.request_stop();
        worker.join();
        assert!(
            tx.try_send(VideoPacket {
                pacing_bitrate: None,
                image_timestamp_ns: None,
                header: header(),
                payload: vec![5],
                gaze_crops: None,
                queued_at: Instant::now()
            })
            .is_err()
        );
    }
}
