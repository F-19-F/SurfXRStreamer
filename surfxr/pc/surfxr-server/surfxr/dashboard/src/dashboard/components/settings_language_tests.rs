//! Exercise translated production controls without changing serialized identifiers.
use super::*;
use crate::i18n::{self, Language};

struct ResetLanguage;
impl Drop for ResetLanguage {
    fn drop(&mut self) {
        i18n::set_language(Language::English);
    }
}

#[test]
fn chinese_controls_keep_preset_keys_and_language_switch_is_read_only() {
    let _reset = ResetLanguage;
    let mut app = Harness::new();
    i18n::set_language(Language::SimplifiedChinese);
    app.size = egui::vec2(1200.0, 2200.0);
    app.settle();
    app.position("快捷设置");
    assert!(!app.output.shapes.iter().any(|shape| matches!(
        &shape.shape, Shape::Text(text) if text.galley.text() == "一次性配对"
    )));
    app.position("Godlike（SurfXR 默认） - 3072 x 3264 / 眼");
    app.click_in_row("编码器预设", "自定义");
    app.click("单遍（更低延迟）");
    app.click("全分辨率两遍（画质优先）");
    let saved = configuration(app.session.session());
    assert_eq!(
        saved["session_settings"]["video"]["encoder_config"]["nvenc"]["multi_pass"]["variant"],
        "FullResolution"
    );
    let changes = app.changes;
    app.tab.preview_page(4);
    app.tab.search = "质量预设".into();
    app.settle();
    app.position("质量预设");
    app.tab.search = "quality preset".into();
    app.settle();
    app.position("质量预设");
    i18n::set_language(Language::English);
    app.settle();
    app.position("Quality preset");
    assert_eq!(app.changes, changes);
    assert_eq!(configuration(app.session.session()), saved);
}

#[test]
fn devices_pairing_switch_does_not_save_permanent_auto_trust() {
    let _serial = crate::pairing::TEST_SERIAL.lock();
    struct ResetPairing;
    impl Drop for ResetPairing {
        fn drop(&mut self) {
            crate::pairing::set_enabled(false);
        }
    }
    let _reset = ResetPairing;
    crate::pairing::set_enabled(false);
    let mut app = Harness::new();
    app.session
        .set_session_values(vec![surfxr_packets::PathValuePair {
            path: surfxr_packets::parse_path(
                "session_settings.connection.client_discovery.content.auto_trust_clients",
            )
            .unwrap(),
            value: serde_json::json!(false),
        }])
        .unwrap();
    app.tab
        .update_session(&app.session.session().session_settings);
    let before = configuration(app.session.session());
    let mut devices = crate::dashboard::components::DevicesTab::new();
    devices.update_client_list(app.session.session());
    app.devices = Some(devices);
    app.click("One-time pairing");
    assert!(crate::pairing::enabled());
    assert!(crate::pairing::claim("ui-pairing-test", false));
    assert!(!crate::pairing::claim("another-device", false));
    crate::pairing::connected("ui-pairing-test");
    app.settle();
    assert!(!crate::pairing::enabled());
    assert_eq!(app.changes, 0);
    assert_eq!(configuration(app.session.session()), before);
}

#[test]
fn settings_help_and_preset_notices_have_chinese_translations() {
    let _reset = ResetLanguage;
    i18n::set_language(Language::SimplifiedChinese);
    use crate::dashboard::components::presets::{builtin_schema, schema::PresetSchemaNode};
    fn check(value: &serde_json::Value) {
        match value {
            serde_json::Value::Object(fields) => {
                for (key, value) in fields {
                    if matches!(key.as_str(), "help" | "notice") {
                        if let Some(text) = value.as_str().filter(|text| !text.is_empty()) {
                            assert_ne!(i18n::tr(text), text, "Missing help: {text}");
                        }
                    }
                    check(value);
                }
            }
            serde_json::Value::Array(values) => values.iter().for_each(check),
            _ => (),
        }
    }
    check(
        &serde_json::to_value(surfxr_session::Settings::schema(
            surfxr_session::session_settings_default(),
        ))
        .unwrap(),
    );
    for preset in [
        builtin_schema::encoder_preset_schema(),
        builtin_schema::foveation_preset_schema(),
        builtin_schema::codec_preset_schema(),
        builtin_schema::game_audio_schema(),
        builtin_schema::hand_tracking_interaction_schema(),
    ] {
        if let PresetSchemaNode::HigherOrderChoice(schema) = preset {
            check(&serde_json::to_value(schema.strings).unwrap());
        }
    }
}
