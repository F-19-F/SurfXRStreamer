use super::is_child;
use crate::bindings::*;
use surfxr_common::{
    Fov, Pose, ViewParams,
    glam::{Quat, Vec3},
    parking_lot::Mutex,
};
use surfxr_packets::VideoPacketHeader;
use surfxr_server_core::{
    haptics::FrameVideoHaptics,
    video_sender::{VideoPacket, VideoSendWorker},
};
use surfxr_sockets::{FrameVideoHeader, StreamSender};
use std::{
    net::UdpSocket,
    os::windows::io::FromRawSocket,
    sync::{Arc, mpsc},
    time::{Duration, Instant},
};

struct Source {
    timestamp: u64,
    image_timestamp_ns: Option<u64>,
    views: [ViewParams; 2],
    crops: [[u32; 2]; 2],
    repeated: bool,
}
struct Child {
    sender: mpsc::SyncSender<VideoPacket>,
    worker: Mutex<Option<VideoSendWorker>>,
    haptics: Arc<Mutex<Option<FrameVideoHaptics>>>,
    hand_serials: Mutex<[[u64; 2]; 2]>,
    source: Mutex<Option<Source>>,
    corrupted: Mutex<bool>,
    avoid_glitching: bool,
    capture_clock: Mutex<surfxr_server_core::video_clock::CaptureClock>,
    image_timestamps: bool,
    gaze: bool,
    pacing_bitrate: std::sync::atomic::AtomicU64,
    media_overhead: u32,
}
static CONTEXT: Mutex<Option<Arc<Child>>> = Mutex::new(None);
fn view(ffi: FfiViewParams) -> ViewParams {
    ViewParams {
        pose: Pose {
            orientation: Quat::from_xyzw(
                ffi.pose.orientation.x,
                ffi.pose.orientation.y,
                ffi.pose.orientation.z,
                ffi.pose.orientation.w,
            ),
            position: Vec3::from_array(ffi.pose.position),
        },
        fov: Fov {
            left: ffi.fov.left,
            right: ffi.fov.right,
            up: ffi.fov.up,
            down: ffi.fov.down,
        },
    }
}
#[unsafe(export_name = "FrameWorkerNetworkReady")]
extern "C" fn network_ready(socket: u64, config: *const FfiFrameWorkerTransport) -> bool {
    if config.is_null() {
        return false;
    }
    let config = unsafe { *config };
    let socket = unsafe { UdpSocket::from_raw_socket(socket) };
    let Ok(sender) = StreamSender::<FrameVideoHeader>::from_external_frame_udp(
        socket,
        config.packetSize as usize,
        config.fec != 0,
        config.pacingBytesPerSecond,
    ) else {
        return false;
    };
    start_network(sender,config,0)
}
#[unsafe(export_name = "FrameWorkerNetworkReadyDirect")]
extern "C" fn network_ready_direct(owner:*mut std::ffi::c_void,send:FfiFrameWorkerSend,config:*const FfiFrameWorkerTransport)->bool {
    let Some(send)=send else {return false;};
    if owner.is_null()||config.is_null(){return false;}
    let config=unsafe{*config};let owner=owner as usize;
    // Native Worker retains this owner until network_stop joins VideoSendWorker.
    let Ok(sender)=StreamSender::<FrameVideoHeader>::from_frame_callback(move|bytes|{
        surfxr_common::anyhow::ensure!(unsafe{send(owner as *mut _,bytes.as_ptr(),bytes.len() as _)},"Daemon video shard send failed");Ok(())
    },config.packetSize as usize,config.fec!=0,config.pacingBytesPerSecond) else {return false;};
    start_network(sender,config,28)
}
fn start_network(sender:StreamSender<FrameVideoHeader>,config:FfiFrameWorkerTransport,media_overhead:u32)->bool {
    let (tx, rx) = mpsc::sync_channel((config.queueCapacity as usize).clamp(1, 16));
    let haptics = Arc::new(Mutex::new(
        (config.haptics != 0).then(FrameVideoHaptics::default),
    ));
    let worker = VideoSendWorker::spawn(sender, rx, Arc::clone(&haptics), config.haptics != 0,
        config.imageTimestamps != 0);
    let mut context=CONTEXT.lock();
    if context.is_some(){worker.request_stop();worker.join();return false;}
    *context=Some(Arc::new(Child {
            sender: tx,
            worker: Mutex::new(Some(worker)),
            haptics,
            hand_serials: Mutex::new([[0; 2]; 2]),
            source: Mutex::new(None),
            corrupted: Mutex::new(true),
            avoid_glitching: config.avoidGlitching != 0,
            capture_clock: Mutex::new(Default::default()),
            image_timestamps: config.imageTimestamps != 0,
            gaze: config.gaze != 0,
            pacing_bitrate: std::sync::atomic::AtomicU64::new(0),
            media_overhead,
        }));
    true
}
#[unsafe(export_name = "FrameWorkerNetworkStop")]
extern "C" fn network_stop() {
    if let Some(context) = CONTEXT.lock().take() {
        let worker = context.worker.lock().take();
        if let Some(worker) = worker {
            worker.request_stop();
            worker.join();
        }
    }
}
#[unsafe(export_name = "FrameWorkerBeginVideo")]
extern "C" fn begin_video(source: *const FfiFrameWorkerSource, timestamp: u64, repeated: bool) {
    let Some(context) = CONTEXT.lock().clone() else {
        return;
    };
    let source = unsafe { source.as_ref() }.filter(|source| source.valid == 1);
    *context.source.lock() = source.map(|source| Source {
        timestamp,
        image_timestamp_ns: context.image_timestamps.then(|| context.capture_clock.lock()
            .timestamp(source.captureTimestampNs, Duration::from_nanos(timestamp)))
            .flatten().map(|stamp| stamp.as_nanos() as u64),
        views: [view(source.views[0]), view(source.views[1])],
        crops: source.crops,
        repeated,
    });
}
#[unsafe(export_name = "FrameWorkerImportHaptics")]
extern "C" fn import_haptics(
    hand: u32,
    serial: u64,
    amplitude: f32,
    remaining: u64,
    pending_serial: u64,
    pending_amplitude: f32,
    pending_age: u64,
) {
    let Some(context) = CONTEXT.lock().clone() else {
        return;
    };
    let hand = hand as usize;
    if hand >= 2 {
        return;
    }
    let mut serials = context.hand_serials.lock();
    if serials[hand][0] == serial {
        return;
    }
    serials[hand][0] = serial;
    let pending = (serials[hand][1] != pending_serial)
        .then_some((pending_amplitude, Duration::from_micros(pending_age)));
    serials[hand][1] = pending_serial;
    if let Some(state) = context.haptics.lock().as_mut() {
        state.import_process_hand(
            hand,
            amplitude,
            Duration::from_micros(remaining),
            pending,
            Instant::now(),
        );
    }
}
pub(crate) fn send_video(timestamp: u64, bytes: *mut u8, size: i32, idr: bool) -> bool {
    if !is_child() {
        return false;
    }
    let Some(context) = CONTEXT.lock().clone() else {
        return true;
    };
    if bytes.is_null() || !(1..=8 * 1024 * 1024).contains(&size) {
        return true;
    }
    let source = context.source.lock();
    let Some(source) = source
        .as_ref()
        .filter(|source| source.timestamp == timestamp)
    else {
        return true;
    };
    let mut corrupted = context.corrupted.lock();
    if idr {
        *corrupted = false;
    }
    if *corrupted && context.avoid_glitching {
        return true;
    }
    let data = unsafe { std::slice::from_raw_parts(bytes, size as usize) };
    let packet = VideoPacket {
        pacing_bitrate: Some((context.pacing_bitrate.load(std::sync::atomic::Ordering::Relaxed), context.media_overhead)),
        image_timestamp_ns: source.image_timestamp_ns,
        queued_at: Instant::now(),
        gaze_crops: context.gaze.then_some(source.crops),
        header: VideoPacketHeader {
            timestamp: Duration::from_nanos(timestamp),
            global_view_params: source.views,
            foveation_center_shifts: None,
            is_idr: idr,
        },
        payload: super::keyboard::payload(data),
    };
    if context.sender.try_send(packet).is_err() {
        *corrupted = true;
        unsafe { FrameWorkerChildRequestIDR() };
        return true;
    }
    unsafe { FrameWorkerRelayVideo(timestamp, size as u32, idr, source.repeated, bytes) };
    true
}

pub(crate) fn encoder_params() -> FfiDynamicEncoderParams {
    let params = unsafe { FrameWorkerEncoderParams() };
    if params.updated != 0 {
        if let Some(context) = CONTEXT.lock().as_ref() {
            context.pacing_bitrate.store(params.bitrate_bps, std::sync::atomic::Ordering::Relaxed);
        }
    }
    params
}
