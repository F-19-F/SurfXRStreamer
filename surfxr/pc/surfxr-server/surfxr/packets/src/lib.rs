use surfxr_common::{
    SurfxrFoveatedEncodingParams, BodySkeleton, ConnectionState, DeviceMotion, LogSeverity, Pose,
    ViewParams,
    anyhow::{Error, Result},
    glam::{Quat, UVec2, Vec2},
    semver::Version,
};
use surfxr_session::{
    ClientsidePostProcessingConfig, CodecType, PassthroughMode, PerformanceLevel, SessionConfig,
    Settings,
};
use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json as json;
use std::{
    collections::HashSet,
    fmt::{self, Debug, Write as _},
    net::IpAddr,
    sync::LazyLock,
    time::Duration,
};

pub const TRACKING: u16 = 0;
pub const HAPTICS: u16 = 1;
pub const AUDIO: u16 = 2;
pub const VIDEO: u16 = 3;
pub const STATISTICS: u16 = 4;

#[derive(Serialize, Deserialize, Clone)]
pub struct VideoStreamingCapabilitiesExt {
    // Nothing for now
}

#[derive(Serialize, Deserialize, Clone)]
pub struct VideoStreamingCapabilities {
    pub default_view_resolution: UVec2,
    pub max_view_resolution: UVec2,
    pub refresh_rates: Vec<f32>,
    pub microphone_sample_rate: u32,
    pub foveated_encoding: bool,
    pub encoder_high_profile: bool,
    pub encoder_10_bits: bool,
    pub encoder_av1: bool,
    pub prefer_10bit: bool,
    pub preferred_encoding_gamma: f32,
    pub prefer_hdr: bool,
    pub ext_str: String,
}

impl VideoStreamingCapabilities {
    pub fn with_ext(self, ext: VideoStreamingCapabilitiesExt) -> Self {
        Self {
            ext_str: json::to_string(&ext).unwrap(),
            ..self
        }
    }

    pub fn ext(&self) -> Result<VideoStreamingCapabilitiesExt> {
        let _ext_json = json::from_str::<json::Value>(&self.ext_str)?;

        // decode values here

        Ok(VideoStreamingCapabilitiesExt {})
    }
}

#[derive(Serialize, Deserialize)]
pub struct ConnectionAcceptedInfo {
    pub client_protocol_id: u64,
    pub platform_string: String,
    pub server_ip: IpAddr, // must be unused for now
    pub streaming_capabilities: Option<VideoStreamingCapabilities>,
}

#[derive(Serialize, Deserialize)]
pub enum ClientConnectionResult {
    ConnectionAccepted(Box<ConnectionAcceptedInfo>),
    ClientStandby,
}

#[derive(Serialize, Deserialize)]
pub struct NegotiatedStreamingConfigExt {
    // Nothing for now
}

#[derive(Serialize, Deserialize, Clone)]
pub struct ClientNegotiatedStreamingConfig {
    pub view_resolution: UVec2,
    pub refresh_rate_hint: f32,
    pub game_audio_sample_rate: u32,
    pub foveated_encoding: Option<SurfxrFoveatedEncodingParams>,
    pub encoding_gamma: f32,
    pub enable_hdr: bool,
    pub wired: bool,
    pub ext_str: String,
}

impl ClientNegotiatedStreamingConfig {
    pub fn with_ext(self, ext: NegotiatedStreamingConfigExt) -> Self {
        Self {
            ext_str: json::to_string(&ext).unwrap(),
            ..self
        }
    }

    pub fn ext(&self) -> Result<NegotiatedStreamingConfigExt> {
        let _ext_json = json::from_str::<json::Value>(&self.ext_str)?;

        // decode values here

        Ok(NegotiatedStreamingConfigExt {})
    }
}

#[derive(Serialize, Deserialize)]
pub struct StreamConfigPacket {
    pub session: String, // JSON session that allows for extrapolation
    pub negotiated: ClientNegotiatedStreamingConfig,
}

#[derive(Serialize, Deserialize, Clone)]
pub struct ClientStreamConfig {
    pub server_version: Version,
    pub settings: Settings,
    pub negotiated_config: ClientNegotiatedStreamingConfig,
}

impl StreamConfigPacket {
    pub fn new(
        session: &SessionConfig,
        negotiated: ClientNegotiatedStreamingConfig,
    ) -> Result<Self> {
        let mut wire_session = session.clone();
        wire_session.server_version = surfxr_common::WIRE_VERSION.parse().unwrap();
        Ok(Self {
            session: json::to_string(&wire_session)?,
            negotiated,
        })
    }

    pub fn to_stream_config(self) -> Result<ClientStreamConfig> {
        let mut session_config = SessionConfig::default();
        session_config.merge_from_json(&json::from_str(&self.session)?)?;
        let settings = session_config.to_settings();

        Ok(ClientStreamConfig {
            server_version: session_config.server_version,
            settings,
            negotiated_config: self.negotiated,
        })
    }
}

#[derive(Serialize, Deserialize, Clone)]
pub struct DecoderInitializationConfig {
    pub codec: CodecType,
    pub config_buffer: Vec<u8>, // e.g. SPS + PPS NALs
    pub ext_str: String,
}

#[derive(Serialize, Deserialize)]
pub enum ServerControlPacket {
    StartStream,
    DecoderConfig(DecoderInitializationConfig),
    Restarting,
    KeepAlive,
    RealTimeConfig(RealTimeConfig),
    Reserved(String),
    ReservedBuffer(Vec<u8>),
}

#[derive(Serialize, Deserialize, Clone)]
pub struct BatteryInfo {
    pub device_id: u64,
    pub gauge_value: f32, // range [0, 1]
    pub is_plugged: bool,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug)]
pub enum ButtonValue {
    Binary(bool),
    Scalar(f32),
}

#[derive(Serialize, Deserialize)]
pub struct ButtonEntry {
    pub path_id: u64,
    pub value: ButtonValue,
}

#[derive(Serialize, Deserialize)]
pub enum ClientControlPacket {
    PlayspaceSync(Option<Vec2>),
    RequestIdr,
    KeepAlive,
    StreamReady, // This flag notifies the server the client streaming socket is ready listening
    LocalViewParams([ViewParams; 2]), // In relation to head
    Battery(BatteryInfo),
    Buttons(Vec<ButtonEntry>),
    ActiveInteractionProfile {
        device_id: u64,
        profile_id: u64,
        input_ids: HashSet<u64>,
    },
    Log {
        level: LogSeverity,
        message: String,
    },
    ProximityState(bool),
    Reserved(String),
    ReservedBuffer(Vec<u8>),
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub enum FaceExpressions {
    Fb(Vec<f32>), // 70 values
    Bd(Vec<f32>), // 52 values
    Htc {
        eye: Option<Vec<f32>>, // 14 values
        lip: Option<Vec<f32>>, // 37 values
    },
}

#[derive(Serialize, Deserialize, Clone, Default, Debug)]
pub struct FaceData {
    /// Head-local orientation whose -Z axis follows the combined gaze direction.
    /// The sample is associated with TrackingData::poll_timestamp, not an independent gaze clock.
    pub eyes_combined: Option<Quat>,
    /// Head-local per-eye orientations, potentially smoothed by the runtime for social presence.
    /// Also used as a foveation input fallback when native combined gaze is unavailable.
    pub eyes_social: [Option<Quat>; 2],

    pub face_expressions: Option<FaceExpressions>,
}

#[derive(Serialize, Deserialize)]
pub struct TrackingData {
    pub poll_timestamp: Duration,
    pub device_motions: Vec<(u64, DeviceMotion)>,
    pub hand_skeletons: [Option<[Pose; 26]>; 2],
    pub face: FaceData,
    pub body: Option<BodySkeleton>,
}

#[derive(Serialize, Deserialize)]
pub struct VideoPacketHeader {
    pub timestamp: Duration,
    pub global_view_params: [ViewParams; 2],
    /// Centers used to encode this frame, already aligned. Normally absent when FFR is disabled.
    pub foveation_center_shifts: Option<[[f32; 2]; 2]>,
    pub is_idr: bool,
}

#[derive(Serialize, Deserialize)]
pub struct Haptics {
    pub device_id: u64,
    pub duration: Duration,
    pub frequency: f32,
    pub amplitude: f32,
}

#[derive(Serialize, Deserialize, Clone)]
pub enum PathSegment {
    Name(String),
    Index(usize),
}

impl fmt::Display for PathSegment {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PathSegment::Name(name) => write!(f, "{name}"),
            PathSegment::Index(index) => write!(f, "[{index}]"),
        }
    }
}

impl Debug for PathSegment {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self}")
    }
}

impl From<&str> for PathSegment {
    fn from(value: &str) -> Self {
        PathSegment::Name(value.to_owned())
    }
}

impl From<String> for PathSegment {
    fn from(value: String) -> Self {
        PathSegment::Name(value)
    }
}

impl From<usize> for PathSegment {
    fn from(value: usize) -> Self {
        PathSegment::Index(value)
    }
}

// A name segment + a (possibly empty) list of `[index]` segments.
static COMPOUND_SEGMENT: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^([A-Za-z0-9_]+)((?:\[[0-9]+\])*)").unwrap());

/// Parses a settings path like `"video.foveated_encoding.enable"` or
/// `"headset.controllers.content.gestures[2].joystick_range"` into name/index segments.
pub fn parse_path(path: &str) -> Result<Vec<PathSegment>> {
    fn parse_error(path: &str, rest: &str, expected: &str) -> Error {
        let found = rest
            .chars()
            .next()
            .map_or_else(|| "end of input".to_owned(), |c| format!("`{c}`"));
        Error::msg(format!(
            "invalid path \"{path}\" at column {}: expected {expected}, found {found}",
            path.len() - rest.len() + 1,
        ))
    }

    let mut segments = Vec::new();
    let mut rest = path;

    loop {
        let Some(caps) = COMPOUND_SEGMENT.captures(rest) else {
            return Err(parse_error(path, rest, "a name `[A-Za-z0-9_]+`"));
        };
        segments.push(PathSegment::Name(caps[1].to_owned()));
        for index in caps[2].split(['[', ']']).filter(|s| !s.is_empty()) {
            match index.parse() {
                Ok(index) => segments.push(PathSegment::Index(index)),
                Err(_) => {
                    let at = &rest[caps[1].len()..];
                    return Err(parse_error(
                        path,
                        at,
                        &format!("index `{index}` to fit in a usize"),
                    ));
                }
            }
        }
        rest = &rest[caps[0].len()..];

        if rest.is_empty() {
            return Ok(segments);
        }
        rest = rest
            .strip_prefix('.')
            .ok_or_else(|| parse_error(path, rest, "`.` or end of input"))?;
    }
}

pub fn path_to_string(path: &[PathSegment]) -> String {
    let mut string = String::new();
    for (i, segment) in path.iter().enumerate() {
        if i != 0 && matches!(segment, PathSegment::Name(_)) {
            string.push('.');
        }
        let _ = write!(string, "{segment}");
    }
    string
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub enum ClientConnectionsAction {
    AddIfMissing {
        trusted: bool,
        manual_ips: Vec<IpAddr>,
    },
    SetDisplayName(String),
    Trust,
    SetManualIps(Vec<IpAddr>),
    RemoveEntry,
    UpdateCurrentIp(Option<IpAddr>),
    SetConnectionState(ConnectionState),
}

#[derive(Serialize, Deserialize, Default, Clone)]
pub struct ClientStatistics {
    pub target_timestamp: Duration, // identifies the frame
    pub frame_interval: Duration,
    pub video_decode: Duration,
    pub video_decoder_queue: Duration,
    pub rendering: Duration,
    pub vsync_queue: Duration,
    pub total_pipeline_latency: Duration,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct PathValuePair {
    pub path: Vec<PathSegment>,
    pub value: json::Value,
}

#[derive(Serialize, Deserialize, Debug)]
pub enum FirewallRulesAction {
    Add,
    Remove,
}

// Note: server sends a packet to the client at low frequency, binary encoding, without ensuring
// compatibility between different versions, even if within the same major version.
#[derive(Serialize, Deserialize, PartialEq, Clone)]
pub struct RealTimeConfig {
    pub passthrough: Option<PassthroughMode>,
    pub clientside_post_processing: Option<ClientsidePostProcessingConfig>,
    pub cpu_performance_level: Option<PerformanceLevel>,
    pub gpu_performance_level: Option<PerformanceLevel>,
    pub ext_str: String,
}

impl RealTimeConfig {
    pub fn from_settings(settings: &Settings) -> Self {
        Self {
            passthrough: settings.video.passthrough.clone().into_option(),
            clientside_post_processing: settings
                .video
                .clientside_post_processing
                .clone()
                .into_option(),
            cpu_performance_level: settings.headset.performance_level.clone().cpu.into_option(),
            gpu_performance_level: settings.headset.performance_level.clone().gpu.into_option(),
            ext_str: String::new(), // No extensions for now
        }
    }
}

#[cfg(test)]
#[path = "path_tests.rs"]
mod tests;
