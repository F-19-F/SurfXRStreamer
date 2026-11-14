use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Debug)]
pub struct OpenvrProperty {
    pub key: OpenvrPropKey,
    pub value: String,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(gui = "button_group")]
pub enum FrameSize {
    Scale(#[schema(gui(slider(min = 0.25, max = 2.0, step = 0.01)))] f32),

    Absolute {
        #[schema(gui(slider(min = 32, max = 8192, step = 32)))]
        width: u32,
        #[schema(gui(slider(min = 32, max = 8192, step = 32)))]
        height: Option<u32>,
    },
}

#[repr(u32)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
pub enum EncoderQualityPreset {
    Quality = 0,
    Balanced = 1,
    Speed = 2,
}

#[repr(u32)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
pub enum EncoderQualityPresetNvidia {
    P1 = 1,
    P2 = 2,
    P3 = 3,
    P4 = 4,
    P5 = 5,
    P6 = 6,
    P7 = 7,
}

#[repr(u32)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
pub enum NvencTuningPreset {
    HighQuality = 1,
    LowLatency = 2,
    UltraLowLatency = 3,
    Lossless = 4,
}

#[repr(u32)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
pub enum NvencMultiPass {
    Disabled = 0,
    #[schema(strings(display_name = "1/4 resolution"))]
    QuarterResolution = 1,
    FullResolution = 2,
}

#[repr(u32)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
pub enum NvencSplitEncodeMode {
    Automatic = 0,
    ForceAutomatic = 1,
    ForceTwo = 2,
    ForceThree = 3,
    Disabled = 15,
}

#[repr(u32)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
pub enum NvencAdaptiveQuantizationMode {
    Disabled = 0,
    Spatial = 1,
    Temporal = 2,
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
#[schema(gui = "button_group")]
pub enum RateControlMode {
    #[schema(strings(display_name = "CBR"))]
    Cbr = 0,
    #[schema(strings(display_name = "VBR"))]
    Vbr = 1,
}

#[repr(u8)]
#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, PartialEq)]
#[schema(gui = "button_group")]
pub enum EntropyCoding {
    #[schema(strings(display_name = "CAVLC"))]
    Cavlc = 1,
    #[schema(strings(display_name = "CABAC"))]
    Cabac = 0,
}

/// Except for preset, the value of these fields is not applied if == -1 (flag)
#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct NvencConfig {
    #[schema(strings(
        help = "P1 is the fastest preset and P7 is the preset that produces better quality. P6 and P7 are too slow to be usable."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub quality_preset: EncoderQualityPresetNvidia,
    #[schema(flag = "steamvr-restart")]
    pub tuning_preset: NvencTuningPreset,
    #[schema(strings(
        help = "Reduce compression artifacts at the cost of small performance penalty"
    ))]
    #[schema(flag = "steamvr-restart")]
    pub multi_pass: NvencMultiPass,
    #[cfg_attr(not(target_os = "windows"), schema(flag = "hidden"))]
    #[schema(strings(
        help = "Windows HEVC/AV1: encode horizontal strips on multiple NVENC engines. Forced modes also enable splitting with P5-P7. May reduce compression quality. A single-engine GPU remains unsplit. H.264 and weighted prediction remain unsplit."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub split_encode_mode: NvencSplitEncodeMode,
    #[schema(strings(
        help = r#"Spatial: Helps reduce color banding, but high-complexity scenes might look worse.
Temporal: Helps improve overall encoding quality, very small trade-off in speed."#
    ))]
    #[schema(flag = "steamvr-restart")]
    pub adaptive_quantization_mode: NvencAdaptiveQuantizationMode,
    #[schema(flag = "steamvr-restart")]
    pub low_delay_key_frame_scale: i64,
    #[schema(flag = "steamvr-restart")]
    pub refresh_rate: i64,
    #[schema(flag = "steamvr-restart")]
    pub enable_intra_refresh: bool,
    #[schema(flag = "steamvr-restart")]
    pub intra_refresh_period: i64,
    #[schema(flag = "steamvr-restart")]
    pub intra_refresh_count: i64,
    #[schema(flag = "steamvr-restart")]
    pub max_num_ref_frames: i64,
    #[schema(flag = "steamvr-restart")]
    pub gop_length: i64,
    #[schema(flag = "steamvr-restart")]
    pub p_frame_strategy: i64,
    #[schema(flag = "steamvr-restart")]
    pub rate_control_mode: i64,
    #[schema(flag = "steamvr-restart")]
    pub rc_buffer_size: i64,
    #[schema(flag = "steamvr-restart")]
    pub rc_initial_delay: i64,
    #[schema(flag = "steamvr-restart")]
    pub rc_max_bitrate: i64,
    #[schema(flag = "steamvr-restart")]
    pub rc_average_bitrate: i64,
    #[schema(flag = "steamvr-restart")]
    pub enable_weighted_prediction: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct AmfConfig {
    #[schema(
        strings(
            display_name = "Enable High-Motion Quality Boost",
            help = r#"Enables high motion quality boost mode.
Allows the encoder to perform pre-analysis the motion of the video and use the information for better encoding"#
        ),
        flag = "steamvr-restart"
    )]
    pub enable_hmqb: bool,
    #[schema(flag = "steamvr-restart")]
    pub use_preproc: bool,
    #[schema(gui(slider(min = 0, max = 10)))]
    #[schema(flag = "steamvr-restart")]
    pub preproc_sigma: u32,
    #[schema(gui(slider(min = 0, max = 10)))]
    #[schema(flag = "steamvr-restart")]
    pub preproc_tor: u32,
    #[schema(
        strings(
            display_name = "Enable Pre-analysis",
            help = r#"Enables pre-analysis during encoding. This will likely result in reduced performance, but may increase quality.
Does not work with the "Reduce color banding" option, requires enabling "Use preproc""#
        ),
        flag = "steamvr-restart"
    )]
    pub enable_pre_analysis: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct SoftwareEncodingConfig {
    #[schema(strings(
        display_name = "Force software encoding",
        help = "Forces the encoder to use CPU instead of GPU"
    ))]
    #[schema(flag = "steamvr-restart")]
    pub force_software_encoding: bool,

    #[schema(strings(display_name = "Encoder thread count"))]
    #[schema(flag = "steamvr-restart")]
    pub thread_count: u32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct HDRConfig {
    #[schema(strings(
        display_name = "Enable HDR",
        help = "If the client has no preference, enables compositing VR layers to an RGBA float16 framebuffer, and doing sRGB/YUV conversions in shader code."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub enable: Option<bool>,

    #[schema(strings(
        display_name = "Force HDR sRGB Correction",
        help = "Forces sRGB correction on all composited SteamVR layers. Useful if an HDR-injected game is too dark."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub force_hdr_srgb_correction: bool,

    #[schema(strings(
        display_name = "Clamp HDR extended range",
        help = "Clamps HDR extended range to 0.0~1.0, useful if you only want HDR to reduce banding."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub clamp_hdr_extended_range: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, PartialEq)]
#[schema(collapsible)]
pub struct EncoderConfig {
    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        display_name = "Quality preset",
        help = "Controls overall quality preset of the encoder. Works only on Windows AMD AMF, Linux VAAPI (AMD/Intel)."
    ))]
    pub quality_preset: EncoderQualityPreset,

    #[schema(
        strings(
            display_name = "Enable VBAQ/CAQ",
            help = "Enables Variance Based Adaptive Quantization on h264 and HEVC, and Content Adaptive Quantization on AV1"
        ),
        flag = "steamvr-restart"
    )]
    pub enable_vbaq: bool,

    #[cfg_attr(not(target_os = "windows"), schema(flag = "hidden"))]
    #[schema(strings(help = r#"CBR: Constant BitRate mode. This is recommended.
VBR: Variable BitRate mode. Not commended because it may throw off the adaptive bitrate algorithm. This is only supported on Windows and only with AMD/Nvidia GPUs"#))]
    #[schema(flag = "steamvr-restart")]
    pub rate_control_mode: RateControlMode,

    #[schema(strings(
        display_name = "h264: Profile",
        help = "Whenever possible, attempts to use this profile. May increase compatibility with varying mobile devices. Only has an effect for h264. Doesn't affect NVENC on Windows."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub h264_profile: H264Profile,

    #[schema(strings(help = r#"CAVLC algorithm is recommended.
CABAC produces better compression but it's significantly slower and may lead to runaway latency"#))]
    #[schema(flag = "steamvr-restart")]
    pub entropy_coding: EntropyCoding,

    #[schema(strings(
        help = r#"In CBR mode, this makes sure the bitrate does not fall below the assigned value. This is mostly useful for debugging."#
    ))]
    #[schema(flag = "steamvr-restart")]
    pub filler_data: bool,

    #[schema(strings(
        display_name = "10-bit encoding",
        help = "Sets the encoder to use 10 bits per channel instead of 8, if the client has no preference. Does not work on Linux with Nvidia"
    ))]
    #[schema(flag = "steamvr-restart")]
    pub use_10bit: Option<bool>,

    #[schema(strings(
        display_name = "Encoding Gamma",
        help = "To prioritize darker pixels at the expense of potentially additional banding in midtones, set to 2.2. To allow the encoder to decide priority on its own, set to 1.0."
    ))]
    #[schema(flag = "steamvr-restart")]
    pub encoding_gamma: Option<f32>,

    #[schema(strings(display_name = "HDR"))]
    #[schema(flag = "steamvr-restart")]
    pub hdr: HDRConfig,

    #[schema(strings(display_name = "NVENC"))]
    #[schema(flag = "steamvr-restart")]
    pub nvenc: NvencConfig,

    #[cfg_attr(not(target_os = "windows"), schema(flag = "hidden"))]
    #[schema(strings(display_name = "AMF"))]
    #[schema(flag = "steamvr-restart")]
    pub amf: AmfConfig,

    #[schema(strings(display_name = "Software (CPU) encoding"))]
    pub software: SoftwareEncodingConfig,
}
