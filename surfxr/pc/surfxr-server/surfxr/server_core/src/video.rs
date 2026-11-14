//! Encoder-facing video API; source metadata is fixed before network handoff.
use crate::{
    SESSION_MANAGER, ServerCoreContext, ServerCoreEvent, bitrate::DynamicEncoderParams,
    create_recording_file, video_sender::VideoPacket,
};
use surfxr_common::{ViewParams, dbg_server_core, parking_lot::Mutex, settings_schema::Switch, warn};
use surfxr_packets::{DecoderInitializationConfig, VideoPacketHeader};
use surfxr_session::CodecType;
use std::{
    io::Write,
    sync::{
        LazyLock,
        atomic::{AtomicBool, Ordering},
        mpsc::TrySendError,
    },
    time::{Duration, Instant},
};

impl ServerCoreContext {
    pub fn set_video_config_nals(&self, config_buffer: Vec<u8>, codec: CodecType) {
        dbg_server_core!("set_video_config_nals");

        if let Some(sender) = &*self.connection_context.video_mirror_sender.lock() {
            sender.send(config_buffer.clone()).ok();
        }

        if let Some(file) = &mut *self.connection_context.video_recording_file.lock() {
            file.write_all(&config_buffer).ok();
        }

        *self.connection_context.decoder_config.lock() = Some(DecoderInitializationConfig {
            codec,
            config_buffer,
            ext_str: String::new(),
        });
    }

    pub fn send_video_nal(
        &self,
        timestamp: Duration,
        mut global_view_params: [ViewParams; 2],
        foveation_center_shifts: Option<[[f32; 2]; 2]>,
        is_idr: bool,
        nal_buffer: Vec<u8>,
    ) {
        dbg_server_core!("send_video_nal");

        // start in the corrupts state, the client didn't receive the initial IDR yet.
        static STREAM_CORRUPTED: AtomicBool = AtomicBool::new(true);
        static LAST_IDR_INSTANT: LazyLock<Mutex<Instant>> =
            LazyLock::new(|| Mutex::new(Instant::now()));

        // Do not hold this mutex while reading SessionManager: connection
        // teardown owns its write lock before removing the channel sender.
        let sender = self.connection_context.video_channel_sender.lock().clone();
        if let Some(sender) = sender {
            let buffer_size = nal_buffer.len();

            if is_idr {
                STREAM_CORRUPTED.store(false, Ordering::SeqCst);
            }

            if let Switch::Enabled(config) = &SESSION_MANAGER
                .read()
                .settings()
                .extra
                .capture
                .rolling_video_files
                && Instant::now()
                    > *LAST_IDR_INSTANT.lock() + Duration::from_secs(config.duration_s)
            {
                self.connection_context
                    .events_sender
                    .send(ServerCoreEvent::RequestIDR)
                    .ok();

                if is_idr {
                    create_recording_file(
                        &self.connection_context,
                        SESSION_MANAGER.read().settings(),
                    );
                    *LAST_IDR_INSTANT.lock() = Instant::now();
                }
            }

            if !STREAM_CORRUPTED.load(Ordering::SeqCst)
                || !SESSION_MANAGER
                    .read()
                    .settings()
                    .connection
                    .avoid_video_glitching
            {
                if let Some(sender) = &*self.connection_context.video_mirror_sender.lock() {
                    sender.send(nal_buffer.clone()).ok();
                }

                if let Some(file) = &mut *self.connection_context.video_recording_file.lock() {
                    file.write_all(&nal_buffer).ok();
                }

                let gaze_crops = {
                    let gaze = self.connection_context.frame_gaze.lock();
                    if let Some(state) = gaze.as_ref() {
                        let Some(crops) = state.encoded(timestamp) else {
                            return;
                        };
                        Some(crops)
                    } else {
                        None
                    }
                };
                // Resolve the source reference space before the network handoff.
                // The sender never waits behind tracking or session persistence.
                self.connection_context
                    .tracking_manager
                    .read()
                    .unrecenter_view_params(&mut global_view_params);
                let pacing_bitrate = match SESSION_MANAGER.read().settings().video.bitrate.mode {
                    surfxr_session::BitrateMode::ConstantMbps(mbps) => Some((mbps.saturating_mul(1_000_000), 0)),
                    _ => None,
                };
                let sender_result = sender.try_send(VideoPacket {
                    pacing_bitrate,
                    image_timestamp_ns: None,
                    queued_at: Instant::now(),
                    gaze_crops,
                    header: VideoPacketHeader {
                        timestamp,
                        global_view_params,
                        foveation_center_shifts,
                        is_idr,
                    },
                    payload: nal_buffer,
                });
                if matches!(sender_result, Err(TrySendError::Full(_))) {
                    STREAM_CORRUPTED.store(true, Ordering::SeqCst);
                    self.connection_context
                        .events_sender
                        .send(ServerCoreEvent::RequestIDR)
                        .ok();
                    warn!("Dropping video packet. Reason: Can't push to network");
                }
            } else {
                warn!("Dropping video packet. Reason: Waiting for IDR frame");
            }

            if let Some(stats) = &mut *self.connection_context.statistics_manager.write() {
                let encoder_latency = stats.report_frame_encoded(timestamp, buffer_size);

                self.connection_context
                    .bitrate_manager
                    .lock()
                    .report_frame_encoded(timestamp, encoder_latency, buffer_size);
            }
        }
    }

    pub fn get_dynamic_encoder_params(&self) -> Option<DynamicEncoderParams> {
        dbg_server_core!("get_dynamic_encoder_params");

        let pair = {
            let session_manager_lock = SESSION_MANAGER.read();
            self.connection_context
                .bitrate_manager
                .lock()
                .get_encoder_params(&session_manager_lock.settings().video.bitrate)
        };

        pair.map(|(params, stats)| {
            if let Some(stats_manager) = &mut *self.connection_context.statistics_manager.write() {
                stats_manager.report_throughput_stats(stats);
            }
            params
        })
    }

    pub fn report_composed(&self, target_timestamp: Duration, offset: Duration) {
        dbg_server_core!("report_composed");

        if let Some(stats) = &mut *self.connection_context.statistics_manager.write() {
            stats.report_frame_composed(target_timestamp, offset);
        }
    }

    pub fn report_present(&self, target_timestamp: Duration, offset: Duration) {
        dbg_server_core!("report_present");

        if let Some(stats) = &mut *self.connection_context.statistics_manager.write() {
            stats.report_frame_present(target_timestamp, offset);
        }

        let session_manager_lock = SESSION_MANAGER.read();
        self.connection_context
            .bitrate_manager
            .lock()
            .report_frame_present(
                &session_manager_lock
                    .settings()
                    .video
                    .bitrate
                    .adapt_to_framerate,
            );
    }
}
