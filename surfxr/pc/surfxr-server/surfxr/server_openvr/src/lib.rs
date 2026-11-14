mod native_log;
mod events;
mod runtime_settings;
mod render_pose;
use events::spawn_event_loop;
use runtime_settings::make_settings;
mod foveated_encoding;
mod graphics;
mod runtime_start;
mod props;
mod tracking;
mod vsync;
#[cfg(windows)]
mod frame_pacing;
#[cfg(windows)]
pub mod frame_worker;

#[allow(
    non_camel_case_types,
    non_upper_case_globals,
    dead_code,
    non_snake_case,
    clippy::unseparated_literal_suffix
)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}
use bindings::*;

use surfxr_common::{
    BUTTON_INFO, HAND_LEFT_ID, HAND_RIGHT_ID, HAND_TRACKER_LEFT_ID, HAND_TRACKER_RIGHT_ID, HEAD_ID,
    Pose, ViewParams, error,
    parking_lot::{Mutex, RwLock},
    settings_schema::Switch,
    warn,
};
use surfxr_filesystem as afs;
use surfxr_packets::{ButtonValue, Haptics};
use surfxr_server_core::{
    HandType, ServerCoreContext, ServerCoreEvent, ServerNegotiatedStreamingConfig,
};
use surfxr_session::{
    BodyTrackingSinkConfig, CodecType, ControllersConfig, ControllersEmulationMode,
};
use foveated_encoding::EyeTrackedFoveation;
use std::{
    collections::VecDeque,
    ffi::{CString, OsStr, c_char, c_void},
    ptr,
    sync::{Once, mpsc},
    thread,
    time::{Duration, Instant},
};

const FOVEATION_CENTER_HISTORY_CAPACITY: usize = 360;

static SERVER_CORE_CONTEXT: RwLock<Option<ServerCoreContext>> = RwLock::new(None);
static LOCAL_VIEW_PARAMS: RwLock<[ViewParams; 2]> = RwLock::new([ViewParams::DUMMY; 2]);
static HEAD_POSE_QUEUE: Mutex<VecDeque<(Duration, Pose)>> = Mutex::new(VecDeque::new());
static FOVEATION_CENTER_QUEUE: Mutex<VecDeque<(Duration, [[f32; 2]; 2])>> =
    Mutex::new(VecDeque::new());
static EYE_TRACKED_FOVEATION: Mutex<Option<EyeTrackedFoveation>> = Mutex::new(None);
static EVENT_LOOP_HANDLE: Mutex<Option<thread::JoinHandle<()>>> = Mutex::new(None);
static IDLE_INIT_HANDLE: Mutex<Option<thread::JoinHandle<()>>> = Mutex::new(None);
static FACTORY_INIT_DATA: Mutex<Option<FactoryInitData>> = Mutex::new(None);

struct FactoryInitData {
    filesystem_layout: afs::Layout,
    early_hmd_initialization: bool,
}

#[unsafe(export_name = "DriverReadyIdle")]
extern "C" fn driver_ready_idle(_set_default_chap: bool) {
    let handle = thread::spawn(move || {
        unsafe { InitOpenvrClient() };

        // Do not write a placeholder rectangle before the real room arrives.
        // Frame applies one confirmed boundary; legacy peers still initialize
        // their rectangle (including the 2 m fallback) via PlayspaceSync.
    });
    *IDLE_INIT_HANDLE.lock() = Some(handle);
}

/// # Safety
/// `instance_ptr` is a valid pointer to a `TrackedDevice` instance
#[unsafe(export_name = "RegisterButtons")]
pub unsafe extern "C" fn register_buttons(instance_ptr: *mut c_void, device_id: u64) {
    let mapped_device_id = if device_id == *HAND_TRACKER_LEFT_ID {
        *HAND_LEFT_ID
    } else if device_id == *HAND_TRACKER_RIGHT_ID {
        *HAND_RIGHT_ID
    } else {
        device_id
    };

    for button_id in surfxr_server_core::registered_button_set() {
        if let Some(info) = BUTTON_INFO.get(&button_id) {
            if info.device_id == mapped_device_id {
                unsafe { RegisterButton(instance_ptr, button_id) };
            }
        } else {
            error!("Cannot register unrecognized button ID {button_id}");
        }
    }
}

#[unsafe(export_name = "HapticsSend")]
extern "C" fn send_haptics(device_id: u64, duration_s: f32, frequency: f32, amplitude: f32) {
    if let Ok(duration) = Duration::try_from_secs_f32(duration_s)
        && let Some(context) = &*SERVER_CORE_CONTEXT.read()
    {
        context.send_haptics(Haptics {
            device_id,
            duration,
            frequency,
            amplitude,
        });
    }
}

#[unsafe(export_name = "SetVideoConfigNals")]
extern "C" fn set_video_config_nals(buffer_ptr: *const u8, len: i32, codec: i32) {
    #[cfg(windows)]
    if frame_worker::is_child() {
        if len >= 0 { unsafe { FrameWorkerRelayConfig(buffer_ptr, len as u32, codec) }; }
        return;
    }
    let codec = if codec == 0 {
        CodecType::H264
    } else if codec == 1 {
        CodecType::Hevc
    } else {
        CodecType::AV1
    };

    let mut config_buffer = vec![0; len as usize];

    unsafe { ptr::copy_nonoverlapping(buffer_ptr, config_buffer.as_mut_ptr(), len as usize) };

    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        context.set_video_config_nals(config_buffer, codec);
    }
}

#[unsafe(export_name = "GetEyeTrackedFoveationCenters")]
extern "C" fn get_eye_tracked_foveation_centers(timestamp_ns: u64) -> FfiFoveationCenters {
    let centers = EYE_TRACKED_FOVEATION
        .lock()
        .as_ref()
        .and_then(|foveation| foveation.centers(Duration::from_nanos(timestamp_ns)));

    FfiFoveationCenters {
        valid: centers.is_some(),
        centerShifts: centers.unwrap_or_default(),
    }
}

#[unsafe(export_name = "GetFrameGazeCrops")]
extern "C" fn get_frame_gaze_crops(timestamp_ns: u64) -> FfiFrameGazeCrops {
    let crops = SERVER_CORE_CONTEXT
        .read()
        .as_ref()
        .map_or([[0; 2]; 2], |context| {
            context.compose_frame_gaze(Duration::from_nanos(timestamp_ns))
        });
    FfiFrameGazeCrops { topLeft: crops }
}

#[unsafe(export_name = "ReportEncoderFoveationCenters")]
extern "C" fn report_encoder_foveation_centers(
    timestamp_ns: u64,
    left_x: f32,
    left_y: f32,
    right_x: f32,
    right_y: f32,
) {
    let queue_mut = &mut *FOVEATION_CENTER_QUEUE.lock();
    queue_mut.push_back((
        Duration::from_nanos(timestamp_ns),
        [[left_x, left_y], [right_x, right_y]],
    ));
    while queue_mut.len() > FOVEATION_CENTER_HISTORY_CAPACITY {
        queue_mut.pop_front();
    }
}

#[unsafe(export_name = "VideoSend")]
extern "C" fn send_video(timestamp_ns: u64, buffer_ptr: *mut u8, len: i32, is_idr: bool) {
    #[cfg(windows)]
    if frame_worker::send_video(timestamp_ns, buffer_ptr, len, is_idr) { return; }
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        let timestamp = Duration::from_nanos(timestamp_ns);
        let buffer = unsafe { std::slice::from_raw_parts(buffer_ptr, len as usize) };

        let Some(head_pose) = HEAD_POSE_QUEUE
            .lock()
            .iter()
            .find_map(|(ts, pose)| (*ts == timestamp).then_some(*pose))
        else {
            // We can't submit the frame without its pose
            return;
        };

        let local_views_params = LOCAL_VIEW_PARAMS.read();

        let global_view_params = [
            ViewParams {
                pose: head_pose * local_views_params[0].pose,
                fov: local_views_params[0].fov,
            },
            ViewParams {
                pose: head_pose * local_views_params[1].pose,
                fov: local_views_params[1].fov,
            },
        ];

        // Keep entries until eviction: an encoder may output multiple NALs for one frame.
        let foveation_center_shifts =
            FOVEATION_CENTER_QUEUE
                .lock()
                .iter()
                .rev()
                .find_map(|(frame_timestamp, centers)| {
                    (*frame_timestamp == timestamp).then_some(*centers)
                });

        context.send_video_nal(
            timestamp,
            global_view_params,
            foveation_center_shifts,
            is_idr,
            buffer.to_vec(),
        );
    }
}

#[unsafe(export_name = "GetDynamicEncoderParams")]
extern "C" fn get_dynamic_encoder_params() -> FfiDynamicEncoderParams {
    #[cfg(windows)]
    if frame_worker::is_child() { return frame_worker::encoder_params(); }
    if let Some(context) = &*SERVER_CORE_CONTEXT.read()
        && let Some(params) = context.get_dynamic_encoder_params()
    {
        FfiDynamicEncoderParams {
            updated: 1,
            bitrate_bps: params.bitrate_bps as u64,
            framerate: params.framerate,
        }
    } else {
        FfiDynamicEncoderParams::default()
    }
}

#[unsafe(export_name = "ReportComposed")]
extern "C" fn report_composed(timestamp_ns: u64, offset_ns: u64) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        context.report_composed(
            Duration::from_nanos(timestamp_ns),
            Duration::from_nanos(offset_ns),
        );
    }
}

#[unsafe(export_name = "ReportPresent")]
extern "C" fn report_present(timestamp_ns: u64, offset_ns: u64) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        context.report_present(
            Duration::from_nanos(timestamp_ns),
            Duration::from_nanos(offset_ns),
        );
    }
}

#[unsafe(export_name = "WaitForVSync")]
extern "C" fn wait_for_vsync() {
    // Default 120Hz-ish wait if StatisticsManager isn't up.
    // We use 120Hz-ish so that SteamVR doesn't accidentally get
    // any weird ideas about our display Hz with its frame pacing.
    static PRE_HEADSET_STATS_WAIT_INTERVAL: Duration = Duration::from_millis(8);

    // NB: don't sleep while locking SERVER_DATA_MANAGER or SERVER_CORE_CONTEXT
    let sleep_duration = SERVER_CORE_CONTEXT
        .read()
        .as_ref()
        .and_then(|ctx| ctx.duration_until_next_vsync());

    if let Some(duration) = sleep_duration {
        if surfxr_server_core::settings()
            .video
            .enforce_server_frame_pacing
        {
            #[cfg(windows)]
            frame_pacing::wait(duration);
            #[cfg(not(windows))]
            thread::sleep(duration);
        } else {
            thread::yield_now();
        }
    } else {
        // StatsManager isn't up because the headset hasn't connected,
        // safety fallback to prevent deadlocking.
        thread::sleep(PRE_HEADSET_STATS_WAIT_INTERVAL);
    }
}

#[unsafe(export_name = "ShutdownRuntime")]
pub extern "C" fn shutdown_driver() {
    SERVER_CORE_CONTEXT.write().take();

    // join driver threads so the dll isn't unloaded while they're still running
    if let Some(handle) = EVENT_LOOP_HANDLE.lock().take() {
        handle.join().ok();
    }
    if let Some(handle) = IDLE_INIT_HANDLE.lock().take() {
        handle.join().ok();
    }
}

/// This is the SteamVR/OpenVR entry point
/// # Safety
#[unsafe(no_mangle)]
pub unsafe extern "C" fn HmdDriverFactory(
    interface_name: *const c_char,
    return_code: *mut i32,
) -> *mut c_void {
    let Ok(driver_dir) = surfxr_server_io::get_driver_dir_from_registered() else {
        return ptr::null_mut();
    };
    let Some(filesystem_layout) =
        surfxr_filesystem::filesystem_layout_from_openvr_driver_root_dir(&driver_dir)
    else {
        return ptr::null_mut();
    };

    let system = sysinfo::System::new_all();
    let dashboard_processes = system
        .processes_by_name(OsStr::new(&afs::dashboard_fname()))
        .collect::<Vec<_>>();

    // When there is already a SURFXR dashboard running, initialize the HMD device early to
    // avoid buggy SteamVR behavior
    // defer heavy init to InitializeRuntime() so a factory probe
    // (e.g. steam.exe enumerating drivers) doesn't spawn threads
    *FACTORY_INIT_DATA.lock() = Some(FactoryInitData {
        filesystem_layout,
        early_hmd_initialization: !dashboard_processes.is_empty(),
    });

    unsafe { CppOpenvrEntryPoint(interface_name, return_code) }
}

#[unsafe(export_name = "LogError")]
unsafe extern "C" fn log_error(string_ptr: *const c_char) {
    #[cfg(windows)]
    if frame_worker::is_child() { unsafe { FrameWorkerRelayLog(string_ptr, 0) }; return; }
    unsafe { native_log::surfxr_error(string_ptr) };
}

#[unsafe(export_name = "LogWarn")]
unsafe extern "C" fn log_warn(string_ptr: *const c_char) {
    #[cfg(windows)]
    if frame_worker::is_child() { unsafe { FrameWorkerRelayLog(string_ptr, 1) }; return; }
    unsafe { native_log::surfxr_warn(string_ptr) };
}

#[unsafe(export_name = "LogInfo")]
unsafe extern "C" fn log_info(string_ptr: *const c_char) {
    #[cfg(windows)]
    if frame_worker::is_child() { unsafe { FrameWorkerRelayLog(string_ptr, 2) }; return; }
    unsafe { native_log::surfxr_info(string_ptr) };
}

#[unsafe(export_name = "LogDebug")]
unsafe extern "C" fn log_debug(string_ptr: *const c_char) {
    #[cfg(windows)]
    if frame_worker::is_child() { return; }
    unsafe { native_log::surfxr_dbg_server_impl(string_ptr) };
}

#[unsafe(export_name = "LogEncoder")]
unsafe extern "C" fn log_encoder(string_ptr: *const c_char) {
    #[cfg(windows)]
    if frame_worker::is_child() { return; }
    unsafe { native_log::surfxr_dbg_encoder(string_ptr) };
}

#[unsafe(export_name = "LogPeriodically")]
unsafe extern "C" fn log_periodically(tag_ptr: *const c_char, string_ptr: *const c_char) {
    #[cfg(windows)]
    if frame_worker::is_child() { return; }
    unsafe { native_log::surfxr_log_periodically(tag_ptr, string_ptr) };
}

#[unsafe(export_name = "PathStringToHash")]
unsafe extern "C" fn path_string_to_hash(path: *const c_char) -> u64 {
    unsafe { native_log::surfxr_path_to_id(path) }
}

#[unsafe(export_name = "GetSerialNumber")]
extern "C" fn get_serial_number(device_id: u64, out_str: *mut c_char) -> u64 {
    props::get_serial_number(device_id, out_str)
}

#[unsafe(export_name = "SetOpenvrProps")]
extern "C" fn set_openvr_props(instance_ptr: *mut c_void, device_id: u64) {
    props::set_device_openvr_props(instance_ptr, device_id)
}

// called from DriverProvider::Init()
#[unsafe(export_name = "InitializeRuntime")]
pub extern "C" fn initialize_runtime() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let Some(init_data) = FACTORY_INIT_DATA.lock().take() else {
            return;
        };
        let filesystem_layout = init_data.filesystem_layout;

        surfxr_server_core::initialize_environment(filesystem_layout.clone());

        let log_to_disk = surfxr_server_core::settings().extra.logging.log_to_disk;

        surfxr_server_core::init_logging(
            log_to_disk.then(|| filesystem_layout.session_log()),
            Some(filesystem_layout.crash_log()),
        );

        unsafe {
            g_sessionPath = CString::new(filesystem_layout.session().to_string_lossy().to_string())
                .unwrap()
                .into_raw();
            g_driverRootDir = CString::new(
                filesystem_layout
                    .openvr_driver_root_dir
                    .to_string_lossy()
                    .to_string(),
            )
            .unwrap()
            .into_raw();
        };

        graphics::initialize_shaders();

        unsafe {
            CppInit(init_data.early_hmd_initialization ||
                surfxr_server_core::settings().extra.steamvr_launcher.enable_surfxr_on_manual_steamvr_start,
                make_settings(None));
        }

        let (context, events_receiver) = ServerCoreContext::new();

        *SERVER_CORE_CONTEXT.write() = Some(context);

        spawn_event_loop(events_receiver);
    });
}
