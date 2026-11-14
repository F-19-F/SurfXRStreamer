mod recording;
pub use recording::create_recording_file;
mod bitrate;
mod connection;
mod hand_gestures;
pub mod haptics;
mod frame_gaze;
pub mod frame_pose;
pub mod frame_clock;
pub mod video_clock;
mod frame_inputs;
pub mod frame_controller_state;
mod frame_connect;
mod restart_events;
mod frame_origin;
mod frame_desktop_tracking;
mod frame_lifecycle;
mod frame_runtime;
pub mod frame_skeleton;
pub mod frame_boundary;
mod input_mapping;
mod logging_backend;
#[cfg(not(target_os = "linux"))]
mod microphone;
mod sockets;
mod statistics;
mod tracking;
mod web_server;
mod video;
pub mod video_sender;
pub mod external_video;

pub use connection::align_foveation_center_shift;
pub use logging_backend::init_logging;
pub use tracking::HandType;

use crate::video_sender::VideoPacket;
use surfxr_common::{
    SurfxrFoveatedEncodingParams, ConnectionState, DEVICE_ID_TO_PATH, DeviceMotion, LifecycleState,
    Pose, ViewParams, dbg_server_core, error,
    glam::{Quat, UVec2, Vec2},
    parking_lot::{Mutex, RwLock},
    settings_schema::Switch,
    warn,
};
use surfxr_events::{EventType, HapticsEvent};
use surfxr_filesystem as afs;
use surfxr_packets::{
    BatteryInfo, ButtonEntry, ClientConnectionsAction, DecoderInitializationConfig, Haptics,
};
use surfxr_server_io::ServerSessionManager;
use surfxr_session::{CodecType, H264Profile, OpenvrProperty, Settings, SteamvrHmdInitConfig};
use surfxr_sockets::StreamSender;
use bitrate::BitrateManager;
use statistics::StatisticsManager;
use std::{
    collections::HashSet,
    env,
    ffi::OsStr,
    fs::File,
    io::Write,
    sync::{
        Arc, LazyLock, OnceLock,
        mpsc::{self, SyncSender},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use tokio::{runtime::Runtime, sync::broadcast};
use tracking::TrackingManager;

static FILESYSTEM_LAYOUT: OnceLock<afs::Layout> = OnceLock::new();

// This is lazily initialized when initializing logging or ServerCoreContext. So FILESYSTEM_LAYOUT
// needs to be initialized first using initialize_environment().
// NB: this must remain a global because only one instance should exist for the whole application
// execution time.
static SESSION_MANAGER: LazyLock<RwLock<ServerSessionManager>> = LazyLock::new(|| {
    RwLock::new(ServerSessionManager::new(
        FILESYSTEM_LAYOUT.get().map(|l| l.session()),
    ))
});

pub fn initialize_environment(layout: afs::Layout) {
    FILESYSTEM_LAYOUT.set(layout).unwrap();

    // This ensures that the session is written to disk
    SESSION_MANAGER.write().session_mut();
}

pub struct ServerNegotiatedStreamingConfig {
    pub frame_media_lease: Option<Arc<surfxr_server_io::frame_media_lock::FrameMediaLease>>,
    pub frame_gaze: Option<surfxr_sockets::FrameGazeConfig>,
    pub transcoding_view_resolution: UVec2,
    pub emulated_headset_view_resolution: UVec2,
    pub refresh_rate: f32,
    pub foveated_encoding: Option<SurfxrFoveatedEncodingParams>,
    pub codec: CodecType,
    pub h264_profile: H264Profile,
    pub use_10bit_encoder: bool,
    pub encoding_gamma: f32,
    pub enable_hdr: bool,
}

pub enum ServerCoreEvent {
    SetOpenvrProperty {
        device_id: u64,
        prop: OpenvrProperty,
    },
    ClientConnected(ServerNegotiatedStreamingConfig),
    ClientDisconnected,
    Battery(BatteryInfo),
    PlayspaceSync(Vec2),
    LocalViewParams([ViewParams; 2]), // In relation to head
    Tracking {
        poll_timestamp: Duration,
    },
    Buttons(Vec<ButtonEntry>), // Note: this is after mapping
    FrameSkeleton(frame_skeleton::FrameSkeleton),
    FrameBoundary(frame_boundary::FrameBoundary),
    FrameControllerState(frame_controller_state::ControllerState),
    RequestIDR,
    GameRenderLatencyFeedback(Duration), // only used for SteamVR
    ShutdownPending,
    RestartPending,
    ProximityState(bool),
}

pub struct ConnectionContext {
    frame_sessions: frame_lifecycle::FrameSessions,
    events_sender: mpsc::Sender<ServerCoreEvent>,
    statistics_manager: RwLock<Option<StatisticsManager>>,
    bitrate_manager: Mutex<BitrateManager>,
    tracking_manager: RwLock<TrackingManager>,
    decoder_config: Mutex<Option<DecoderInitializationConfig>>,
    video_mirror_sender: Mutex<Option<broadcast::Sender<Vec<u8>>>>,
    video_recording_file: Mutex<Option<File>>,
    connection_threads: Mutex<Vec<JoinHandle<()>>>,
    clients_to_be_removed: Mutex<HashSet<String>>,
    video_channel_sender: Mutex<Option<SyncSender<VideoPacket>>>,
    external_video_transport: Mutex<Option<external_video::ExternalVideoTransport>>,
    external_video_haptics_observer: Mutex<Option<fn(Haptics)>>,
    haptics_sender: Mutex<Option<StreamSender<Haptics>>>,
    frame_video_haptics: Arc<Mutex<Option<haptics::FrameVideoHaptics>>>,
    frame_gaze: Mutex<Option<frame_gaze::FrameGazeState>>,
    frame_pose: Mutex<Option<frame_pose::FramePoseState>>,
}

pub fn notify_restart_driver() {
    if sysinfo::System::new_all()
        .processes_by_name(OsStr::new(&afs::dashboard_fname()))
        .next()
        .is_some()
    {
        restart_events::RESTART.request();
        surfxr_events::send_event(EventType::ServerRequestsSelfRestart);
    } else {
        error!("Cannot restart SteamVR. No dashboard process found on local device.");
    }
}

pub fn settings() -> Settings {
    SESSION_MANAGER.read().settings().clone()
}

static RUNTIME_SETTINGS: LazyLock<Settings> = LazyLock::new(settings);
pub fn runtime_settings() -> Settings { RUNTIME_SETTINGS.clone() }

pub fn steamvr_hmd_init_config() -> SteamvrHmdInitConfig {
    let saved = SESSION_MANAGER.read().session().steamvr_hmd_init_config.clone();
    frame_runtime::initial_config(&runtime_settings(), saved)
}

pub fn registered_button_set() -> HashSet<u64> {
    let session_manager = SESSION_MANAGER.read();
    if let Switch::Enabled(input_mapping) = &session_manager.settings().headset.controllers {
        input_mapping::registered_button_set(&input_mapping.emulation_mode)
    } else {
        HashSet::new()
    }
}

pub struct ServerCoreContext {
    lifecycle_state: Arc<RwLock<LifecycleState>>,
    connection_context: Arc<ConnectionContext>,
    connection_thread: Arc<RwLock<Option<JoinHandle<()>>>>,
    webserver_runtime: Option<Runtime>,
}

impl ServerCoreContext {
    pub fn compose_frame_gaze(&self, timestamp: Duration) -> [[u32; 2]; 2] {
        self.connection_context.frame_gaze.lock().as_mut()
            .map_or([[0; 2]; 2], |state| state.compose(timestamp, Instant::now()))
    }
    pub fn new() -> (Self, mpsc::Receiver<ServerCoreEvent>) {
        dbg_server_core!("Creating");

        if SESSION_MANAGER
            .read()
            .settings()
            .extra
            .logging
            .prefer_backtrace
        {
            unsafe { env::set_var("RUST_BACKTRACE", "1") };
        }

        SESSION_MANAGER.write().clean_client_list();

        let (events_sender, events_receiver) = mpsc::channel();

        // Create a temporary StatisticsManager until a headset connects
        let initial_settings = runtime_settings();
        let stats = StatisticsManager::new(
            initial_settings.connection.statistics_history_size,
            Duration::from_secs_f32(1.0 / 90.0),
            if let Switch::Enabled(config) = &initial_settings.headset.controllers {
                config.steamvr_pipeline_frames
            } else {
                0.0
            },
        );

        let connection_context = Arc::new(ConnectionContext {
            events_sender,
            statistics_manager: RwLock::new(Some(stats)),
            bitrate_manager: Mutex::new(BitrateManager::new(256, 60.0)),
            tracking_manager: RwLock::new(TrackingManager::new(
                initial_settings.connection.statistics_history_size,
            )),
            decoder_config: Mutex::new(None),
            video_mirror_sender: Mutex::new(None),
            video_recording_file: Mutex::new(None),
            external_video_transport: Mutex::new(None),
            external_video_haptics_observer: Mutex::new(None),
            connection_threads: Mutex::new(Vec::new()),
            frame_sessions: frame_lifecycle::FrameSessions::default(),
            clients_to_be_removed: Mutex::new(HashSet::new()),
            video_channel_sender: Mutex::new(None),
            haptics_sender: Mutex::new(None),
            frame_video_haptics: Arc::new(Mutex::new(None)),
            frame_gaze: Mutex::new(None),
            frame_pose: Mutex::new(None),
        });

        let webserver_runtime = Runtime::new().unwrap();
        webserver_runtime.spawn({
            let connection_context = Arc::clone(&connection_context);
            async move { surfxr_common::show_err(web_server::web_server(connection_context).await) }
        });

        (
            Self {
                lifecycle_state: Arc::new(RwLock::new(LifecycleState::StartingUp)),
                connection_context,
                connection_thread: Arc::new(RwLock::new(None)),
                webserver_runtime: Some(webserver_runtime),
            },
            events_receiver,
        )
    }

    pub fn start_connection(&self) {
        dbg_server_core!("start_connection");

        // Note: Idle state is not used on the server side
        *self.lifecycle_state.write() = LifecycleState::Resumed;

        let connection_context = Arc::clone(&self.connection_context);
        let lifecycle_state = Arc::clone(&self.lifecycle_state);
        *self.connection_thread.write() = Some(thread::spawn(move || {
            connection::handshake_loop(connection_context, lifecycle_state);
        }));
    }

    pub fn get_device_motion(
        &self,
        device_id: u64,
        sample_timestamp: Duration,
    ) -> Option<DeviceMotion> {
        dbg_server_core!("get_device_motion: dev={device_id} sample_ts={sample_timestamp:?}");

        self.connection_context
            .tracking_manager
            .read()
            .get_device_motion(device_id, sample_timestamp)
    }

    pub fn get_hand_skeleton(
        &self,
        hand_type: HandType,
        timestamp: Duration,
    ) -> Option<[Pose; 26]> {
        dbg_server_core!("get_hand_skeleton: hand={hand_type:?} ts={timestamp:?}");

        self.connection_context
            .tracking_manager
            .read()
            .get_hand_skeleton(hand_type, timestamp)
            .copied()
    }

    /// Return head-local gaze for an exact retained tracking timestamp, with -Z along the gaze.
    /// Returns None if the sample has no gaze or is no longer in the bounded history.
    pub fn get_combined_eye_gaze(&self, sample_timestamp: Duration) -> Option<Quat> {
        dbg_server_core!("get_combined_eye_gaze: sample_ts={sample_timestamp:?}");

        self.connection_context
            .tracking_manager
            .read()
            .get_combined_eye_gaze(sample_timestamp)
    }

    pub fn get_motion_to_photon_latency(&self) -> Duration {
        dbg_server_core!("get_motion_to_photon_latency");

        let latency = self
            .connection_context
            .statistics_manager
            .read()
            .as_ref()
            .map(|stats| stats.motion_to_photon_latency_average())
            .unwrap_or_default();

        let max_prediction =
            Duration::from_millis(SESSION_MANAGER.read().settings().headset.max_prediction_ms);

        if latency > max_prediction {
            warn!("Latency is too high. Clamping prediction");

            max_prediction
        } else {
            latency
        }
    }

    pub fn get_tracker_pose_time_offset(&self) -> Duration {
        dbg_server_core!("get_tracker_pose_time_offset");

        self.connection_context
            .statistics_manager
            .read()
            .as_ref()
            .map(|stats| stats.tracker_pose_time_offset())
            .unwrap_or_default()
    }

    pub fn send_haptics(&self, haptics: Haptics) {
        dbg_server_core!("send_haptics");

        let haptics_config = {
            let session_manager_lock = SESSION_MANAGER.read();

            if session_manager_lock.settings().extra.logging.log_haptics {
                surfxr_events::send_event(EventType::Haptics(HapticsEvent {
                    path: DEVICE_ID_TO_PATH.get(&haptics.device_id).map_or_else(
                        || format!("Unknown (ID: {:#16x})", haptics.device_id),
                        |p| (*p).to_owned(),
                    ),
                    duration: haptics.duration,
                    frequency: haptics.frequency,
                    amplitude: haptics.amplitude,
                }))
            }

            session_manager_lock
                .settings()
                .headset
                .controllers
                .as_option()
                .and_then(|c| c.haptics.as_option().cloned())
        };

        if let Some(config) = haptics_config {
            let mapped = haptics::map_haptics(&config, haptics);
            let observer = *self.connection_context.external_video_haptics_observer.lock();
            if let Some(observer) = observer {
                observer(Haptics {
                    device_id: mapped.device_id,
                    duration: mapped.duration,
                    frequency: mapped.frequency,
                    amplitude: mapped.amplitude,
                });
            }
            {
                let mut state = self.connection_context.frame_video_haptics.lock();
                if let Some(state) = state.as_mut() {
                    state.update(mapped, Instant::now());
                    return;
                }
            }
            if let Some(sender) = &mut *self.connection_context.haptics_sender.lock() {
                sender.send_header(&mapped).ok();
            }
        }
    }

    pub fn duration_until_next_vsync(&self) -> Option<Duration> {
        dbg_server_core!("duration_until_next_vsync");

        self.connection_context
            .statistics_manager
            .write()
            .as_mut()
            .map(|stats| stats.duration_until_next_vsync())
    }
}

impl Drop for ServerCoreContext {
    fn drop(&mut self) {
        dbg_server_core!("Drop");

        // Invoke connection runtimes shutdown
        self.connection_context.frame_sessions.shutdown();
        *self.lifecycle_state.write() = LifecycleState::ShuttingDown;

        dbg_server_core!("Setting clients as Disconnecting");
        {
            let mut session_manager_lock = SESSION_MANAGER.write();

            let hostnames = session_manager_lock
                .client_list()
                .iter()
                .filter(|&(_, info)| {
                    !matches!(
                        info.connection_state,
                        ConnectionState::Disconnected | ConnectionState::Disconnecting
                    )
                })
                .map(|(hostname, _)| hostname.clone())
                .collect::<Vec<_>>();

            for hostname in hostnames {
                session_manager_lock.update_client_connections(
                    hostname,
                    ClientConnectionsAction::SetConnectionState(ConnectionState::Disconnecting),
                );
            }
        }

        dbg_server_core!("Joining connection thread");
        if let Some(thread) = self.connection_thread.write().take() {
            thread.join().ok();
        }

        // apply openvr config for the next launch
        dbg_server_core!("Setting restart settings cache");
        {
            let mut session_manager_lock = SESSION_MANAGER.write();
            let new_steamvr_hmd_init_config = session_manager_lock
                .session()
                .steamvr_hmd_init_config
                .clone();
            let settings = session_manager_lock.session().to_settings();
            let new_hash =
                connection::compute_restart_settings_hash(&new_steamvr_hmd_init_config, &settings);
            let mut session = session_manager_lock.session_mut();
            session.steamvr_hmd_init_config = new_steamvr_hmd_init_config;
            session.restart_settings_hash = new_hash;
        }

        // todo: check if this is still needed
        while SESSION_MANAGER
            .read()
            .client_list()
            .iter()
            .any(|(_, info)| info.connection_state != ConnectionState::Disconnected)
        {
            thread::sleep(Duration::from_millis(100));
        }

        self.webserver_runtime.take();
    }
}
