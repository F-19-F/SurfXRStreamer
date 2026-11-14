//! Private bridge for the Frame encoder/socket owner outside vrserver.
//! Source metadata is resolved before GPU handoff; observations never send.
use crate::{SESSION_MANAGER, ServerCoreContext, ServerCoreEvent, create_recording_file};
use surfxr_common::{ViewParams, parking_lot::Mutex};
use surfxr_packets::Haptics;
use surfxr_sockets::{FrameVideoHeader, StreamSender};
use std::{
    io::Write,
    sync::LazyLock,
    time::{Duration, Instant},
};

pub struct ExternalVideoTransport {
    pub sender: StreamSender<FrameVideoHeader>,
    pub packet_size: u32,
    pub queue_capacity: u32,
    pub pacing_bytes_per_second: u64,
    pub fec: bool,
    pub haptics: bool,
    pub avoid_glitching: bool,
    pub image_timestamps: bool,
    pub daemon: bool,
    pub gaze: bool,
}

impl ServerCoreContext {
    #[cfg(windows)]
    pub fn external_video_transport(&self) -> Option<(usize, u32, u32, u64, bool, bool, bool, bool, bool)> {
        let lock = self.connection_context.external_video_transport.lock();
        let data = lock.as_ref()?;
        Some((
            data.sender.frame_udp_socket_for_process()?,
            data.packet_size,
            data.queue_capacity,
            data.pacing_bytes_per_second,
            data.fec,
            data.haptics,
            data.avoid_glitching,
            data.image_timestamps,
            data.gaze,
        ))
    }
    pub fn external_video_daemon(&self)->bool {
        self.connection_context.external_video_transport.lock().as_ref().is_some_and(|value|value.daemon)
    }
    pub fn restore_external_video_transport(&self) {
        if let Some(data) = self
            .connection_context
            .external_video_transport
            .lock()
            .as_ref()
        {
            data.sender.restore_frame_udp_qos();
        }
    }
    pub fn external_video_source(
        &self,
        timestamp: Duration,
        mut views: [ViewParams; 2],
    ) -> Option<([ViewParams; 2], [[u32; 2]; 2])> {
        let crops = self
            .connection_context
            .frame_gaze
            .lock()
            .as_ref().and_then(|gaze|gaze.encoded(timestamp));
        let crops=match crops {Some(crops)=>crops,None if self.external_video_daemon()=>[[0;2];2],None=>return None};
        self.connection_context
            .tracking_manager
            .read()
            .unrecenter_view_params(&mut views);
        Some((views, crops))
    }
    pub fn set_external_video_haptics_observer(&self, observer: Option<fn(Haptics)>) {
        *self
            .connection_context
            .external_video_haptics_observer
            .lock() = observer;
    }
    pub fn external_video_capture_enabled(&self) -> bool {
        self.connection_context
            .video_mirror_sender
            .lock()
            .as_ref()
            .is_some_and(|sender| sender.receiver_count() > 0)
            || self
                .connection_context
                .video_recording_file
                .lock()
                .is_some()
            || SESSION_MANAGER
                .read()
                .settings()
                .extra
                .capture
                .rolling_video_files
                .enabled()
    }
    pub fn observe_external_video(
        &self,
        timestamp: Duration,
        bytes: usize,
        is_idr: bool,
        repeated: bool,
        payload: &[u8],
    ) {
        static LAST_IDR: LazyLock<Mutex<Instant>> = LazyLock::new(|| Mutex::new(Instant::now()));
        let rolling_duration = SESSION_MANAGER
            .read()
            .settings()
            .extra
            .capture
            .rolling_video_files
            .as_option()
            .map(|config| Duration::from_secs(config.duration_s));
        // Release the settings guard before opening a new recording. A queued
        // writer must not strand the observer in a recursive read lock.
        if let Some(duration) = rolling_duration
            && Instant::now() > *LAST_IDR.lock() + duration
        {
            self.connection_context
                .events_sender
                .send(ServerCoreEvent::RequestIDR)
                .ok();
            if is_idr {
                create_recording_file(&self.connection_context, SESSION_MANAGER.read().settings());
                *LAST_IDR.lock() = Instant::now();
            }
        }
        if !payload.is_empty() {
            if let Some(sender) = self.connection_context.video_mirror_sender.lock().as_ref() {
                sender.send(payload.to_vec()).ok();
            }
            if let Some(file) = self.connection_context.video_recording_file.lock().as_mut() {
                file.write_all(payload).ok();
            }
        }
        // Freeze repeats retain the old source timestamp and are never reported
        // as new source frames or used to move acknowledgments backwards.
        if !repeated
            && let Some(stats) = self.connection_context.statistics_manager.write().as_mut()
        {
            let latency = stats.report_frame_encoded(timestamp, bytes);
            self.connection_context
                .bitrate_manager
                .lock()
                .report_frame_encoded(timestamp, latency, bytes);
        }
    }
}
