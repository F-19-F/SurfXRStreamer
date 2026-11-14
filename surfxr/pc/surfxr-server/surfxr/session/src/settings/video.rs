use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Debug, PartialEq)]
pub enum MediacodecPropType {
    Float,
    Int32,
    Int64,
    String,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Debug, PartialEq)]
pub struct MediacodecProperty {
    #[schema(strings(display_name = "Type"))]
    pub ty: MediacodecPropType,
    pub value: String,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
pub struct EncoderLatencyLimiter {
    #[schema(strings(
        help = "Allowed percentage of frame interval to allocate for video encoding"
    ))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.3, max = 1.0, step = 0.01)))]
    pub max_saturation_multiplier: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct DecoderLatencyLimiter {
    #[schema(strings(
        display_name = "Maximum decoder latency",
        help = "When the decoder latency goes above this threshold, the bitrate will be reduced"
    ))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 1, max = 50)), suffix = "ms")]
    pub max_decoder_latency_ms: u64,

    #[schema(strings(
        display_name = "latency overstep",
        help = "Number of consecutive frames above the threshold to trigger a bitrate reduction"
    ))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 1, max = 100)), suffix = " frames")]
    pub latency_overstep_frames: usize,

    #[schema(strings(
        help = "Controls how much the bitrate is reduced when the decoder latency goes above the threshold"
    ))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.5, max = 1.0)))]
    pub latency_overstep_multiplier: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(gui = "button_group")]
pub enum BitrateMode {
    #[schema(strings(display_name = "Constant"))]
    ConstantMbps(#[schema(gui(slider(min = 5, max = 1000, logarithmic)), suffix = "Mbps")] u64),

    #[schema(collapsible)]
    Adaptive {
        #[schema(strings(
            help = "Percentage of network bandwidth to allocate for video transmission"
        ))]
        #[schema(flag = "real-time")]
        #[schema(gui(slider(min = 0.5, max = 5.0, step = 0.01)))]
        saturation_multiplier: f32,

        #[schema(strings(display_name = "Maximum bitrate"))]
        #[schema(flag = "real-time")]
        #[schema(gui(slider(min = 1, max = 1000, logarithmic)), suffix = "Mbps")]
        max_throughput_mbps: Switch<u64>,

        #[schema(strings(display_name = "Minimum bitrate"))]
        #[schema(flag = "real-time")]
        #[schema(gui(slider(min = 1, max = 100, logarithmic)), suffix = "Mbps")]
        min_throughput_mbps: Switch<u64>,

        #[schema(strings(display_name = "Maximum network latency"))]
        #[schema(flag = "real-time")]
        #[schema(gui(slider(min = 1, max = 50)), suffix = "ms")]
        max_network_latency_ms: Switch<u64>,

        #[schema(flag = "real-time")]
        encoder_latency_limiter: Switch<EncoderLatencyLimiter>,

        #[schema(strings(
            help = "Currently there is a bug where the decoder latency keeps rising when above a certain bitrate"
        ))]
        #[schema(flag = "real-time")]
        decoder_latency_limiter: Switch<DecoderLatencyLimiter>,
    },
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
pub struct BitrateAdaptiveFramerateConfig {
    #[schema(strings(
        display_name = "FPS reset threshold multiplier",
        help = "If the framerate changes more than this factor, trigger a parameters update",
    ))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 1.0, max = 3.0, step = 0.1)))]
    pub framerate_reset_threshold_multiplier: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct BitrateConfig {
    #[schema(flag = "real-time")]
    pub mode: BitrateMode,

    #[schema(strings(
        help = "Ensure that the specified bitrate value is respected regardless of the framerate"
    ))]
    #[schema(flag = "real-time")]
    pub adapt_to_framerate: Switch<BitrateAdaptiveFramerateConfig>,

    #[schema(strings(help = "Controls the smoothness during calculations"))]
    pub history_size: usize,

    #[schema(strings(
        help = "When this is enabled, an IDR frame is requested after the bitrate is changed.
This has an effect only on AMD GPUs."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub image_corruption_fix: bool,
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Copy, Clone, PartialEq)]
pub enum ClientsideFoveationLevel {
    Low = 1,
    Medium = 2,
    High = 3,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
pub enum ClientsideFoveationMode {
    Static { level: ClientsideFoveationLevel },
    Dynamic { max_level: ClientsideFoveationLevel },
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
pub struct ClientsideFoveationConfig {
    pub mode: ClientsideFoveationMode,

    #[schema(strings(display_name = "Foveation offset"))]
    #[schema(gui(slider(min = -45.0, max = 45.0, step = 0.1)), suffix = "掳")]
    pub vertical_offset_deg: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct FoveatedEncodingConfig {
    #[schema(strings(help = "Force enable on smartphone clients"))]
    pub force_enable: bool,

    #[schema(strings(
        display_name = "Center region size",
        help = "Width and height, in X/Y order"
    ))]
    #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
    #[schema(flag = "steamvr-restart")]
    pub center_size: [f32; 2],

    #[schema(strings(
        display_name = "Center shift",
        help = "Horizontal and vertical shift, in X/Y order. Used for fixed foveated encoding and when eye tracking is unavailable. Eye-tracking centers replace this shift rather than adding to it."
    ))]
    #[schema(gui(slider(min = -1.0, max = 1.0, step = 0.01)))]
    #[schema(flag = "steamvr-restart")]
    pub center_shift: [f32; 2],

    #[schema(strings(help = "Horizontal and vertical compression ratios, in X/Y order"))]
    #[schema(gui(slider(min = 1.0, max = 10.0, step = 1.0)))]
    #[schema(flag = "steamvr-restart")]
    pub edge_ratio: [f32; 2],
}

#[repr(C)]
#[derive(SettingsSchema, Clone, Copy, Serialize, Deserialize, Pod, Zeroable)]
pub struct ColorCorrectionConfig {
    #[schema(gui(slider(min = -1.0, max = 1.0, step = 0.001)))]
    #[schema(flag = "steamvr-restart")]
    pub brightness: f32,

    #[schema(gui(slider(min = -1.0, max = 1.0, step = 0.001)))]
    #[schema(flag = "steamvr-restart")]
    pub contrast: f32,

    #[schema(gui(slider(min = -1.0, max = 1.0, step = 0.01)))]
    #[schema(flag = "steamvr-restart")]
    pub saturation: f32,

    #[schema(gui(slider(min = 0.0, max = 5.0, step = 0.01)))]
    #[schema(flag = "steamvr-restart")]
    pub gamma: f32,

    #[schema(gui(slider(min = -1.0, max = 5.0, step = 0.01)))]
    #[schema(flag = "steamvr-restart")]
    pub sharpening: f32,
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Debug, Copy, Clone, PartialEq, Eq, Default)]
#[schema(gui = "button_group")]
pub enum CodecType {
    #[default]
    #[schema(strings(display_name = "h264"))]
    H264 = 0,
    #[schema(strings(display_name = "HEVC"))]
    Hevc = 1,
    #[schema(strings(display_name = "AV1"))]
    AV1 = 2,
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Debug, Copy, Clone, PartialEq, Eq)]
#[schema(gui = "button_group")]
pub enum H264Profile {
    #[schema(strings(display_name = "High"))]
    High = 0,
    #[schema(strings(display_name = "Main"))]
    Main = 1,
    #[schema(strings(display_name = "Baseline"))]
    Baseline = 2,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Debug)]
pub struct RgbChromaKeyConfig {
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0, max = 255)))]
    pub red: u8,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0, max = 255)))]
    pub green: u8,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0, max = 255)))]
    pub blue: u8,

    #[schema(strings(help = "The threshold is applied per-channel"))]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 1, max = 255)))]
    pub distance_threshold: u8,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = 0.01, max = 1.0, step = 0.01)))]
    pub feathering: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Debug)]
pub struct HsvChromaKeyConfig {
    #[schema(strings(display_name = "Hue start max"), suffix = "掳")]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -179.0, max = 539.0, step = 1.0)))]
    pub hue_start_max_deg: f32,

    #[schema(strings(display_name = "Hue start min"), suffix = "掳")]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -179.0, max = 539.0, step = 1.0)))]
    pub hue_start_min_deg: f32,

    #[schema(strings(display_name = "Hue end min"), suffix = "掳")]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -179.0, max = 539.0, step = 1.0)))]
    pub hue_end_min_deg: f32,

    #[schema(strings(display_name = "Hue end max"), suffix = "掳")]
    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -179.0, max = 539.0, step = 1.0)))]
    pub hue_end_max_deg: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub saturation_start_max: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub saturation_start_min: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub saturation_end_min: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5,step = 0.01)))]
    pub saturation_end_max: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub value_start_max: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub value_start_min: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub value_end_min: f32,

    #[schema(flag = "real-time")]
    #[schema(gui(slider(min = -0.5, max = 1.5, step = 0.01)))]
    pub value_end_max: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Debug)]
#[schema(gui = "button_group")]
pub enum PassthroughMode {
    Blend {
        #[schema(strings(
            help = "Enabling this will adapt transparency based on the brightness of each pixel.
This is a similar effect to AR glasses."
        ))]
        #[schema(flag = "real-time")]
        premultiplied_alpha: bool,

        #[schema(flag = "real-time")]
        #[schema(gui(slider(min = 0.0, max = 1.0, step = 0.01)))]
        threshold: f32,
    },

    #[schema(strings(display_name = "RGB Chroma Key"))]
    RgbChromaKey(#[schema(flag = "real-time")] RgbChromaKeyConfig),

    #[schema(strings(display_name = "HSV Chroma Key"))]
    HsvChromaKey(#[schema(flag = "real-time")] HsvChromaKeyConfig),
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Debug, Copy, Clone, PartialEq, Eq)]
#[schema(gui = "button_group")]
pub enum ClientsidePostProcessingSuperSamplingMode {
    Disabled = 0,
    Normal = 1 << 0,
    Quality = 1 << 1,
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Debug, Copy, Clone, PartialEq, Eq)]
#[schema(gui = "button_group")]
pub enum ClientsidePostProcessingSharpeningMode {
    Disabled = 0,
    Normal = 1 << 2,
    Quality = 1 << 3,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Debug)]
pub struct ClientsidePostProcessingConfig {
    #[schema(strings(
        help = "Reduce flicker for high contrast edges.\nUseful when the input resolution is high compared to the headset display"
    ))]
    pub super_sampling: ClientsidePostProcessingSuperSamplingMode,
    #[schema(strings(
        help = "Improve clarity of high contrast edges and counteract blur.\nUseful when the input resolution is low compared to the headset display"
    ))]
    pub sharpening: ClientsidePostProcessingSharpeningMode,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq, Debug)]
pub struct UpscalingConfig {
    #[schema(strings(
        help = "Improves visual quality by using the edge direction to upscale at a slight performance loss"
    ))]
    pub edge_direction: bool,
    #[schema(gui(slider(min = 1.0, max = 16.0, step = 1.0)))]
    pub edge_threshold: f32,
    #[schema(gui(slider(min = 1.0, max = 2.0, step = 0.01)))]
    pub edge_sharpness: f32,
    #[schema(gui(slider(min = 1.0, max = 3.0, step = 0.01)))]
    #[schema(strings(
        help = "Dimensional resolution multiplier, high values will cause performance issues with weaker headset hardware or higher resolutions"
    ))]
    pub upscale_factor: f32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct VideoConfig {
    pub frame_gaze: FrameGazeConfig,
    #[schema(flag = "real-time")]
    pub passthrough: Switch<PassthroughMode>,

    pub bitrate: BitrateConfig,

    #[schema(strings(
        help = "HEVC may provide better visual fidelity at the cost of increased encoder latency"
    ))]
    #[schema(flag = "steamvr-restart")]
    pub preferred_codec: CodecType,

    #[schema(strings(
        notice = r"These controls tune upstream SURFXR center/edge encoding. For Frame gaze clients, Dynamic Foveated Streaming takes priority and these values are ignored while it is enabled.
To use these controls, turn off Quick setup > Picture & performance. Increase center size to retain more edge detail.
The upstream Eye-tracked foveated encoding option under Headset > Face tracking is separate from the SurfXR gaze atlas."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub foveated_encoding: Switch<FoveatedEncodingConfig>,

    #[schema(flag = "steamvr-restart")]
    pub color_correction: Switch<ColorCorrectionConfig>,

    #[schema(
        strings(
            display_name = "Maximum buffering",
            help = "Increasing this value will help reduce stutter but it will increase latency"
        ),
        gui(slider(min = 1.0, max = 10.0, step = 0.1, logarithmic)),
        suffix = " frames"
    )]
    pub max_buffering_frames: f32,

    #[schema(gui(slider(min = 0.50, max = 0.99, step = 0.01)))]
    pub buffering_history_weight: f32,

    #[cfg_attr(not(target_os = "windows"), schema(flag = "hidden"))]
    #[schema(strings(
        help = r"This works only on Windows. It shouldn't be disabled except in certain circumstances when you know the VR game will not meet the target framerate."
    ))]
    #[schema(flag = "real-time")]
    pub enforce_server_frame_pacing: bool,

    #[schema(flag = "steamvr-restart")]
    pub encoder_config: EncoderConfig,

    #[schema(strings(
        help = "Attempts to use a software decoder on the device. Slow, but may work around broken codecs."
    ))]
    pub force_software_decoder: bool,

    pub mediacodec_extra_options: Vec<(String, MediacodecProperty)>,

    #[schema(strings(
        help = "Resolution used for encoding and decoding. Relative to a single eye view."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub transcoding_view_resolution: FrameSize,

    #[schema(strings(
        help = "This is the resolution that SteamVR will use as default for the game rendering. Relative to a single eye view."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub emulated_headset_view_resolution: FrameSize,

    #[schema(strings(display_name = "Preferred FPS"))]
    #[schema(gui(slider(min = 60.0, max = 144.0)), suffix = "Hz")]
    #[schema(flag = "steamvr-restart")]
    pub preferred_fps: f32,

    #[cfg_attr(not(target_os = "windows"), schema(flag = "hidden"))]
    #[schema(strings(
        help = "You probably don't want to change this. Allows for changing adapter for SURFXR compositor."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub adapter_index: u32,

    #[schema(strings(display_name = "Client-side foveation"))]
    pub clientside_foveation: Switch<ClientsideFoveationConfig>,

    #[schema(strings(
        display_name = "Client-side post-processing",
        help = "Hardware optimized algorithms, available on Quest and Pico headsets"
    ))]
    #[schema(flag = "real-time")]
    pub clientside_post_processing: Switch<ClientsidePostProcessingConfig>,

    #[schema(strings(help = "Snapdragon Game Super Resolution client-side upscaling"))]
    pub upscaling: Switch<UpscalingConfig>,
}
