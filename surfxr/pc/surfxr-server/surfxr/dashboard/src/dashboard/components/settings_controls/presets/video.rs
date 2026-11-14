//! Video presets bind to the existing session schema; opening settings never applies them.
use super::{
    builtin_schema::{bool_modifier, num_modifier, string_modifier},
    schema::{HigherOrderChoiceOption, HigherOrderChoiceSchema, PresetSchemaNode},
};
use settings_schema::ChoiceControlType;

fn surfxr_encoder_option() -> HigherOrderChoiceOption {
    const PREFIX: &str = "session_settings.video.encoder_config";
    let mut modifiers = [
        ("quality_preset", "Speed"),
        ("rate_control_mode", "Cbr"),
        ("nvenc.quality_preset", "P1"),
        ("nvenc.tuning_preset", "LowLatency"),
        ("nvenc.multi_pass", "Disabled"),
        ("nvenc.split_encode_mode", "ForceThree"),
        ("nvenc.adaptive_quantization_mode", "Spatial"),
    ]
    .into_iter()
    .map(|(path, value)| string_modifier(&format!("{PREFIX}.{path}.variant"), value))
    .collect::<Vec<_>>();
    modifiers.push(num_modifier(
        &format!("{PREFIX}.nvenc.rate_control_mode"),
        "-1",
    ));
    modifiers.push(bool_modifier(
        &format!("{PREFIX}.nvenc.enable_weighted_prediction"),
        false,
    ));
    HigherOrderChoiceOption {
        display_name: "SurfXR default".into(),
        modifiers,
        content: None,
    }
}

pub fn encoder_preset_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Encoder preset".into(),
        strings: [(
            "help".into(),
            "SurfXR default uses the project's latency-focused encoder setup: CBR; NVENC P1, \
            LowLatency, single pass, Spatial AQ and up to three split engines. \
            Weighted prediction and the NVENC rate-control override are disabled. \
            Other encoders use Speed. Splitting depends on GPU/codec support. \
            Custom preserves your settings. Adjust Multi pass below or other encoder settings in Advanced. \
            Full-resolution two-pass analysis prioritizes detail at additional encoding cost. \
            Codec, bitrate, resolution and refresh rate are selected separately."
                .into(),
        )]
        .into_iter()
        .collect(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: vec![
            surfxr_encoder_option(),
            HigherOrderChoiceOption {
                display_name: "Custom".into(),
                modifiers: vec![],
                content: None,
            },
        ],
        default_option_display_name: "SurfXR default".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

pub fn foveation_preset_schema() -> PresetSchemaNode {
    const PREFIX: &str = "session_settings.video.foveated_encoding";
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Foveation preset".into(),
        strings: [(
            "help".into(),
            "Selecting Light / Medium / High disables Dynamic Foveated Streaming and uses upstream center/edge encoding. \
            Enabling Dynamic Foveated Streaming disables fixed foveation while retaining these quality values. \
            SurfXR default disables fixed foveation. Neither mode reduces the game rendering resolution."
                .into(),
        )]
        .into_iter()
        .collect(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: std::iter::once(HigherOrderChoiceOption {
            display_name: "SurfXR default".into(),
            modifiers: vec![bool_modifier(&format!("{PREFIX}.enabled"), false)],
            content: None,
        }).chain([
            ("Light", [0.80, 0.80], [8.0, 8.0]),
            ("Medium", [0.66, 0.60], [6.0, 6.0]),
            ("High", [0.45, 0.40], [4.0, 5.0]),
        ]
        .into_iter()
        .map(|(key, center_size, edge_ratio)| {
            let [center_size_x, center_size_y] = center_size;
            let [edge_ratio_x, edge_ratio_y] = edge_ratio;

            HigherOrderChoiceOption {
                display_name: key.into(),
                modifiers: [
                    bool_modifier(&format!("{PREFIX}.enabled"), true),
                    num_modifier(
                        &format!("{PREFIX}.content.center_size.content[0]"),
                        &center_size_x.to_string(),
                    ),
                    num_modifier(
                        &format!("{PREFIX}.content.center_size.content[1]"),
                        &center_size_y.to_string(),
                    ),
                    num_modifier(
                        &format!("{PREFIX}.content.edge_ratio.content[0]"),
                        &edge_ratio_x.to_string(),
                    ),
                    num_modifier(
                        &format!("{PREFIX}.content.edge_ratio.content[1]"),
                        &edge_ratio_y.to_string(),
                    ),
                ]
                .into_iter()
                .collect(),
                content: None,
            }
        }))
        .collect(),
        default_option_display_name: "SurfXR default".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}

/// A single-setting row using the same control and session writer as encoder presets.
pub fn multipass_schema() -> PresetSchemaNode {
    PresetSchemaNode::HigherOrderChoice(HigherOrderChoiceSchema {
        name: "Multi pass".into(),
        strings: [("help".into(), "Full-resolution two-pass analysis can improve detail within your bitrate budget, with extra encoding time. No filler data is added. NVIDIA only; restart SteamVR after changing this setting.".into())].into_iter().collect(),
        flags: ["steamvr-restart".into()].into_iter().collect(),
        options: [
            ("Single pass (lower latency)", "Disabled"),
            ("Two passes (quarter resolution)", "QuarterResolution"),
            ("Two passes (full resolution, quality)", "FullResolution"),
        ].into_iter().map(|(label, value)| HigherOrderChoiceOption {
            display_name: label.into(),
            modifiers: vec![string_modifier("session_settings.video.encoder_config.nvenc.multi_pass.variant", value)],
            content: None,
        }).collect(),
        default_option_display_name: "Single pass (lower latency)".into(),
        gui: ChoiceControlType::ButtonGroup,
    })
}
