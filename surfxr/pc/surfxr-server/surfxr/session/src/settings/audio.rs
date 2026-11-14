use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(gui = "button_group")]
pub enum CustomAudioDeviceConfig {
    #[schema(strings(display_name = "By name (substring)"))]
    NameSubstring(String),
    #[schema(strings(display_name = "By index"))]
    Index(usize),
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct AudioBufferingConfig {
    #[schema(strings(display_name = "Average buffering"))]
    #[schema(gui(slider(min = 0, max = 200)), suffix = "ms")]
    pub average_buffering_ms: u64,

    #[schema(strings(display_name = "Batch size"))]
    #[schema(gui(slider(min = 1, max = 20)), suffix = "ms")]
    pub batch_ms: u64,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct GameAudioConfig {
    #[cfg_attr(target_os = "linux", schema(flag = "hidden"))]
    pub device: Option<CustomAudioDeviceConfig>,

    #[cfg_attr(target_os = "linux", schema(flag = "hidden"))]
    #[schema(strings(display_name = "Mute desktop audio when streaming"))]
    pub mute_when_streaming: bool,

    pub buffering: AudioBufferingConfig,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum MicrophoneDevicesConfig {
    #[schema(strings(display_name = "Steam Streaming Microphone"))]
    SteamStreaming,
    Automatic,
    #[schema(strings(display_name = "Virtual Audio Cable"))]
    VAC,
    #[schema(strings(display_name = "VB Cable"))]
    VBCable,
    #[schema(strings(display_name = "VoiceMeeter"))]
    VoiceMeeter,
    #[schema(strings(display_name = "VoiceMeeter Aux"))]
    VoiceMeeterAux,
    #[schema(strings(display_name = "VoiceMeeter VAIO3"))]
    VoiceMeeterVaio3,
    Custom {
        #[schema(strings(display_name = "Playback endpoint", help = "SURFXR sends headset audio into this virtual playback device. This is not your PC speakers."))]
        sink: CustomAudioDeviceConfig,
        #[schema(strings(display_name = "Recording endpoint", help = "Games receive headset audio from this virtual recording device. Select it as the microphone in the game or voice app."))]
        source: CustomAudioDeviceConfig,
    },
}

// Note: sample rate is a free parameter for microphone, because both server and client supports
// resampling. In contrary, for game audio, the server does not support resampling.
#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
#[schema(collapsible)]
pub struct MicrophoneConfig {
    #[cfg_attr(target_os = "linux", schema(flag = "hidden"))]
    #[schema(strings(display_name = "PC microphone device", help = "Steam Streaming Microphone is the Windows default. Choose another installed virtual audio device only if needed; Custom lets you specify its playback and recording endpoints."))]
    pub devices: MicrophoneDevicesConfig,

    pub buffering: AudioBufferingConfig,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct AudioConfig {
    #[schema(strings(display_name = "Headset speaker"))]
    pub game_audio: Switch<GameAudioConfig>,

    #[cfg_attr(
        windows,
        schema(strings(
            display_name = "Headset microphone",
            help = "Send headset microphone audio to Steam Streaming Microphone on your PC. Select Steam Streaming Microphone as the input in your game or voice app. Device selection and buffering are available below."
        ))
    )]
    #[cfg_attr(not(windows), schema(strings(display_name = "Headset microphone")))]
    pub microphone: Switch<MicrophoneConfig>,
}
