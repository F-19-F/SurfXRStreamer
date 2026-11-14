use super::*;

#[test]
fn old_logging_settings_gain_quiet_level_without_losing_preferences() {
    let mut saved = json::to_value(SessionConfig::default()).unwrap();
    let logging = saved["session_settings"]["extra"]["logging"].as_object_mut().unwrap();
    logging.remove("log_level");
    logging.insert("log_to_disk".into(), json::json!(true));
    let mut session = SessionConfig::default();
    session.merge_from_json(&saved).unwrap();
    assert_eq!(session.to_settings().extra.logging.log_level, surfxr_common::LogSeverity::Warning);
    assert!(session.to_settings().extra.logging.log_to_disk);
    saved["session_settings"]["extra"]["logging"]["log_level"] = json::json!({"variant":"Debug"});
    session.merge_from_json(&saved).unwrap();
    assert_eq!(session.to_settings().extra.logging.log_level, surfxr_common::LogSeverity::Debug);
}

#[test]
fn test_manual_session_to_settings() {
    let default = session_settings_default();
    let settings_schema = json::to_value(&default).unwrap();
    let schema = Settings::schema(default);

    let _settings =
        json::from_value::<Settings>(json_session_settings_to_settings(&settings_schema, &schema))
            .err();
}

#[test]
fn test_session_to_settings() {
    let _settings = SessionConfig::default().to_settings();
}

#[test]
fn test_session_extrapolation_trivial() {
    SessionConfig::default()
        .merge_from_json(&json::to_value(SessionConfig::default()).unwrap())
        .unwrap();
}

#[test]
fn test_session_extrapolation_diff() {
    let input_json_string = r#"{
        "session_settings": {
          "fjdshfks": false,
          "video": {
            "preferred_fps": 60.0,
            "encoder_config": {"nvenc": {"quality_preset": {"variant": "P5"}}}
          },
          "headset": {
            "gui_collapsed": false,
            "controllers": {
              "enabled": false
            }
          }
        }
      }"#;

    let mut session = SessionConfig::default();
    session
        .merge_from_json(&json::from_str(input_json_string).unwrap())
        .unwrap();

    let settings = session.to_settings();

    assert_eq!(settings.video.preferred_fps, 60.0);
    assert!(settings.video.encoder_config.nvenc.quality_preset == EncoderQualityPresetNvidia::P5);
    assert!(
        settings.video.encoder_config.nvenc.split_encode_mode == NvencSplitEncodeMode::ForceThree
    );
    assert!(settings.headset.controllers.as_option().is_none());
}
