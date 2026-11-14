use super::schema::{
    HigherOrderChoiceOption, HigherOrderChoiceSchema, PresetModifier, PresetSchemaNode,
};
pub use super::video::{encoder_preset_schema, foveation_preset_schema, multipass_schema};
use crate::dashboard::components::presets::schema::PresetModifierOperation;
use settings_schema::ChoiceControlType;
use std::{
    collections::{HashMap, HashSet},
    str::FromStr,
};

pub(super) fn string_modifier(target_path: &str, value: &str) -> PresetModifier {
    PresetModifier {
        target_path: target_path.into(),
        operation: PresetModifierOperation::Assign(serde_json::Value::String(value.into())),
    }
}
pub(super) fn num_modifier(target_path: &str, value: &str) -> PresetModifier {
    PresetModifier {
        target_path: target_path.into(),
        operation: PresetModifierOperation::Assign(serde_json::Value::Number(
            serde_json::Number::from_str(value).unwrap(),
        )),
    }
}
pub(super) fn bool_modifier(target_path: &str, value: bool) -> PresetModifier {
    PresetModifier {
        target_path: target_path.into(),
        operation: PresetModifierOperation::Assign(serde_json::Value::Bool(value)),
    }
}

pub fn resolution_schema() -> PresetSchemaNode {
    // Reconstructed per-eye tiers from Frame preferences/remote_settings.cpp.
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Resolution".into(),
        strings: [(
            "help".into(),
            "SurfXR per-eye output size. Sets both the SteamVR render target and streaming resolution. Dynamic Foveated Streaming packs its base and focus regions separately. Higher tiers need more GPU and decoder capacity.".into(),
        )]
        .into_iter()
        .collect(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: [
            ("Potato", 1344, 1440),
            ("Low", 1728, 1824),
            ("Medium", 2112, 2304),
            ("High", 2496, 2688),
            ("Ultra", 2688, 2880),
            ("Godlike (SurfXR default)", 3072, 3264),
            ("Monster", 3648, 4032),
            ("Rampage", 4224, 4608),
            ("Unstoppable", 4992, 5376),
        ]
        .into_iter()
        .map(|(name, width, height)| HigherOrderChoiceOption {
            display_name: format!("{name} - {width} x {height} / eye"),
            modifiers: [
                string_modifier(
                    "session_settings.video.transcoding_view_resolution.variant",
                    "Absolute",
                ),
                num_modifier(
                    "session_settings.video.transcoding_view_resolution.Absolute.width",
                    &width.to_string(),
                ),
                bool_modifier(
                    "session_settings.video.transcoding_view_resolution.Absolute.height.set",
                    true,
                ),
                num_modifier(
                    "session_settings.video.transcoding_view_resolution.Absolute.height.content",
                    &height.to_string(),
                ),
                string_modifier(
                    "session_settings.video.emulated_headset_view_resolution.variant",
                    "Absolute",
                ),
                num_modifier(
                    "session_settings.video.emulated_headset_view_resolution.Absolute.width",
                    &width.to_string(),
                ),
                bool_modifier(
                    "session_settings.video.emulated_headset_view_resolution.Absolute.height.set",
                    true,
                ),
                num_modifier(
                    "session_settings.video.emulated_headset_view_resolution.Absolute.height.content",
                    &height.to_string(),
                ),
            ]
            .into_iter()
            .collect(),
            content: None,
        })
        .collect(),
        default_option_display_name: "Godlike (SurfXR default) - 3072 x 3264 / eye".into(),
        gui: ChoiceControlType::Dropdown,
    })
}

pub fn framerate_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Preferred framerate".into(),
        strings: HashMap::new(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: [60, 72, 80, 90, 120, 144]
            .into_iter()
            .map(|framerate| HigherOrderChoiceOption {
                display_name: format!("{framerate}Hz"),
                modifiers: [num_modifier(
                    "session_settings.video.preferred_fps",
                    &format!("{:?}", framerate as f32),
                )]
                .into_iter()
                .collect(),
                content: None,
            })
            .collect(),
        default_option_display_name: "120Hz".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

pub fn codec_preset_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Codec preset".into(),
        strings: [
            (
            "notice".into(),
            "AV1 encoding is only supported on RDNA3, Ada Lovelace, Intel ARC or newer GPUs (AMD RX 7xxx+ , NVIDIA RTX 40xx+, Intel ARC)
and on headsets that have XR2 Gen 2 onboard (Quest 3, Pico 4 Ultra).\n
H264 encoding is currently NOT supported on Intel ARC GPUs on Windows."
                .into(),
            ),
        ]
        .into_iter()
        .collect(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: [("H264", "H264"), ("HEVC", "Hevc"), ("AV1", "AV1")]
            .into_iter()
            .map(|(key, val_codec)| HigherOrderChoiceOption {
                display_name: key.into(),
                modifiers: [string_modifier(
                    "session_settings.video.preferred_codec.variant",
                    val_codec,
                )]
                .into_iter()
                .collect(),
                content: None,
            })
            .collect(),
        default_option_display_name: "HEVC".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

#[cfg(target_os = "linux")]
pub fn game_audio_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Headset speaker".into(),
        strings: HashMap::new(),
        flags: HashSet::new(),
        options: [
            HigherOrderChoiceOption {
                display_name: "Disabled".into(),
                modifiers: vec![bool_modifier(
                    "session_settings.audio.game_audio.enabled",
                    false,
                )],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "Enabled".into(),
                modifiers: vec![bool_modifier(
                    "session_settings.audio.game_audio.enabled",
                    true,
                )],
                content: None,
            },
        ]
        .into_iter()
        .collect(),
        default_option_display_name: "Enabled".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

#[cfg(target_os = "linux")]
pub fn microphone_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Headset microphone".into(),
        strings: HashMap::new(),
        flags: HashSet::new(),
        options: [
            HigherOrderChoiceOption {
                display_name: "Disabled".into(),
                modifiers: vec![bool_modifier(
                    "session_settings.audio.microphone.enabled",
                    false,
                )],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "Enabled".into(),
                modifiers: vec![bool_modifier(
                    "session_settings.audio.microphone.enabled",
                    true,
                )],
                content: None,
            },
        ]
        .into_iter()
        .collect(),
        default_option_display_name: "Enabled".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

#[cfg(not(target_os = "linux"))]
pub fn game_audio_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Headset speaker".into(),
        strings: [(
            "notice".into(),
            "You can change the default audio device from the system taskbar tray (bottom right)"
                .into(),
        )]
        .into_iter()
        .collect(),
        flags: HashSet::new(),
        options: vec![
            HigherOrderChoiceOption {
                display_name: "Disabled".into(),
                modifiers: vec![bool_modifier(
                    "session_settings.audio.game_audio.enabled",
                    false,
                )],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "System Default".to_owned(),
                modifiers: vec![
                    bool_modifier("session_settings.audio.game_audio.enabled", true),
                    bool_modifier(
                        "session_settings.audio.game_audio.content.device.set",
                        false,
                    ),
                ],
                content: None,
            },
        ]
        .into_iter()
        .collect(),
        default_option_display_name: "System Default".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

#[cfg(not(target_os = "linux"))]
pub fn microphone_schema() -> PresetSchemaNode {
    let mut microhone_options = vec![HigherOrderChoiceOption {
        display_name: "Disabled".to_owned(),
        modifiers: vec![bool_modifier(
            "session_settings.audio.microphone.enabled",
            false,
        )],
        content: None,
    }];

    if cfg!(windows) {
        for (key, display_name) in [
            ("SteamStreaming", "Steam Streaming Microphone"),
            ("Automatic", "Automatic"),
            ("VAC", "Virtual Audio Cable"),
            ("VBCable", "VB Cable"),
            ("VoiceMeeter", "VoiceMeeter"),
            ("VoiceMeeterAux", "VoiceMeeter Aux"),
            ("VoiceMeeterVaio3", "VoiceMeeter VAIO3"),
        ] {
            microhone_options.push(HigherOrderChoiceOption {
                display_name: display_name.into(),
                modifiers: vec![
                    bool_modifier("session_settings.audio.microphone.enabled", true),
                    string_modifier(
                        "session_settings.audio.microphone.content.devices.variant",
                        key,
                    ),
                ],
                content: None,
            })
        }
    }

    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Headset microphone".into(),
        strings: HashMap::new(),
        flags: HashSet::new(),
        options: microhone_options.into_iter().collect(),
        default_option_display_name: if cfg!(windows) {
            "Steam Streaming Microphone".into()
        } else {
            "Disabled".into()
        },
        gui: ChoiceControlType::Dropdown,
    })
}

pub fn hand_tracking_interaction_schema() -> PresetSchemaNode {
    const HELP: &str = r"Disabled: hands cannot emulate buttons. Useful for using Joy-Cons or other non-native controllers.
SteamVR Input 2.0: create separate SteamVR devices for hand tracking.
SURFXR bindings: use SURFXR hand tracking button bindings. Check the wiki for help.
";

    const PREFIX: &str = "session_settings.headset.controllers.content";

    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Hand tracking interaction".into(),
        strings: [("help".into(), HELP.into())].into_iter().collect(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: [
            HigherOrderChoiceOption {
                display_name: "Disabled".into(),
                modifiers: vec![
                    bool_modifier("session_settings.headset.controllers.enabled", true),
                    bool_modifier(
                        &format!("{PREFIX}.hand_skeleton.content.steamvr_input_2_0"),
                        false,
                    ),
                    bool_modifier(
                        &format!("{PREFIX}.hand_tracking_interaction.enabled"),
                        false,
                    ),
                ],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "SteamVR Input 2.0".into(),
                modifiers: vec![
                    bool_modifier("session_settings.headset.controllers.enabled", true),
                    bool_modifier(&format!("{PREFIX}.hand_skeleton.enabled"), true),
                    bool_modifier(
                        &format!("{PREFIX}.hand_skeleton.content.steamvr_input_2_0"),
                        true,
                    ),
                    bool_modifier(
                        &format!("{PREFIX}.hand_tracking_interaction.enabled"),
                        false,
                    ),
                ],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "SURFXR bindings".into(),
                modifiers: vec![
                    bool_modifier("session_settings.headset.controllers.enabled", true),
                    bool_modifier(
                        &format!("{PREFIX}.hand_skeleton.content.steamvr_input_2_0"),
                        false,
                    ),
                    bool_modifier(&format!("{PREFIX}.hand_tracking_interaction.enabled"), true),
                ],
                content: None,
            },
        ]
        .into_iter()
        .collect(),
        default_option_display_name: "SteamVR Input 2.0".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

pub fn eye_face_tracking_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Eye and face tracking".into(),
        strings: HashMap::new(),
        flags: HashSet::new(),
        options: [
            HigherOrderChoiceOption {
                display_name: "Disabled".into(),
                modifiers: vec![bool_modifier(
                    "session_settings.headset.face_tracking.enabled",
                    false,
                )],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "VRChat Eye OSC".into(),
                modifiers: vec![
                    bool_modifier("session_settings.headset.face_tracking.enabled", true),
                    bool_modifier(
                        "session_settings.headset.face_tracking.content.sink.social_presence.enabled",
                        true,
                    ),
                    string_modifier(
                        "session_settings.headset.face_tracking.content.sink.social_presence.content.variant",
                        "VrchatEyeOsc",
                    ),
                ],
                content: None,
            },
            HigherOrderChoiceOption {
                display_name: "VRCFaceTracking".into(),
                modifiers: vec![
                    bool_modifier("session_settings.headset.face_tracking.enabled", true),
                    bool_modifier(
                        "session_settings.headset.face_tracking.content.sink.social_presence.enabled",
                        true,
                    ),
                    string_modifier(
                        "session_settings.headset.face_tracking.content.sink.social_presence.content.variant",
                        "VrcFaceTracking",
                    ),
                ],
                content: None,
            },
        ]
        .into_iter()
        .collect(),
        default_option_display_name: "Disabled".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}
