use crate::{HEAD_POSE_QUEUE, LOCAL_VIEW_PARAMS, SERVER_CORE_CONTEXT, bindings::*, tracking};
use surfxr_common::{HAND_LEFT_ID, HAND_RIGHT_ID, ViewParams};
use surfxr_packets::Haptics;
use std::{ptr, time::Duration};

#[unsafe(export_name = "FrameWorkerSource")]
extern "C" fn source(timestamp: u64, output: *mut FfiFrameWorkerSource) -> bool {
    if output.is_null() {
        return false;
    }
    let stamp = Duration::from_nanos(timestamp);
    let Some(head) = HEAD_POSE_QUEUE
        .lock()
        .iter()
        .find_map(|(ts, pose)| (*ts == stamp).then_some(*pose))
    else {
        return false;
    };
    let local = *LOCAL_VIEW_PARAMS.read();
    let views = std::array::from_fn(|eye| ViewParams {
        pose: head * local[eye].pose,
        fov: local[eye].fov,
    });
    let guard = SERVER_CORE_CONTEXT.read();
    let Some((views, crops)) = guard
        .as_ref()
        .and_then(|context| context.external_video_source(stamp, views))
    else {
        return false;
    };
    unsafe {
        ptr::write(
            output,
            FfiFrameWorkerSource {
                valid: 1,
                views: [
                    tracking::to_ffi_view_params(views[0]),
                    tracking::to_ffi_view_params(views[1]),
                ],
                crops,
                captureTimestampNs: 0, // filled by the capture owner before GPU handoff
            },
        )
    };
    true
}
#[unsafe(export_name = "FrameWorkerTransport")]
extern "C" fn transport(output: *mut FfiFrameWorkerTransport) -> bool {
    if output.is_null() {
        return false;
    }
    let guard = SERVER_CORE_CONTEXT.read();
    let Some((socket, packet_size, capacity, pacing, fec, haptics, avoid, image_timestamps, gaze)) = guard
        .as_ref()
        .and_then(|context| context.external_video_transport())
    else {
        return false;
    };
    unsafe {
        ptr::write(
            output,
            FfiFrameWorkerTransport {
                socket: socket as u64,
                pacingBytesPerSecond: pacing,
                packetSize: packet_size,
                queueCapacity: capacity,
                fec: u32::from(fec),
                haptics: u32::from(haptics),
                avoidGlitching: u32::from(avoid),
                imageTimestamps: u32::from(image_timestamps),
                gaze: u32::from(gaze),
            },
        )
    };
    true
}
#[unsafe(export_name = "FrameWorkerDaemonRequired")]
extern "C" fn daemon_required()->bool {
    SERVER_CORE_CONTEXT.read().as_ref().is_some_and(|context|context.external_video_daemon())
}
#[unsafe(export_name = "FrameWorkerObserve")]
extern "C" fn observe(
    timestamp: u64,
    bytes: u32,
    idr: bool,
    repeated: bool,
    payload: *const u8,
    length: u32,
) {
    if length > 8 * 1024 * 1024 || (length > 0 && payload.is_null()) {
        return;
    }
    let data = if length == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(payload, length as usize) }
    };
    if let Some(context) = SERVER_CORE_CONTEXT.read().as_ref() {
        context.observe_external_video(
            Duration::from_nanos(timestamp),
            bytes as usize,
            idr,
            repeated,
            data,
        );
    }
}
#[unsafe(export_name = "FrameWorkerCaptureEnabled")]
extern "C" fn capture_enabled() -> bool {
    SERVER_CORE_CONTEXT
        .read()
        .as_ref()
        .is_some_and(|context| context.external_video_capture_enabled())
}
fn haptics_observer(haptics: Haptics) {
    let hand = if haptics.device_id == *HAND_LEFT_ID {
        0
    } else if haptics.device_id == *HAND_RIGHT_ID {
        1
    } else {
        return;
    };
    unsafe {
        FrameWorkerUpdateHaptics(
            hand,
            haptics.amplitude,
            haptics.duration.as_micros().min(5_000_000) as u64,
        )
    };
}
#[unsafe(export_name = "FrameWorkerHapticsObserver")]
extern "C" fn set_observer(enabled: bool) {
    if let Some(context) = SERVER_CORE_CONTEXT.read().as_ref() {
        context.set_external_video_haptics_observer(enabled.then_some(haptics_observer));
        if !enabled {
            context.restore_external_video_transport();
        }
    }
}
