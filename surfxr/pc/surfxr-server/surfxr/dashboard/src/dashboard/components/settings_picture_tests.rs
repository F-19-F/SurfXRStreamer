//! Exercise the moved controls through the real UI and session writer.
use super::*;

#[test]
fn multipass_is_explicit_in_quick_picture_and_preserves_other_values() {
    let mut app=Harness::new();app.size=egui::vec2(1200.0,2200.0);
    let mut expected=configuration(app.session.session());
    app.settle();app.position("Custom");app.position("Multi pass");
    for removed in ["Speed","Balanced","Quality"] {
        assert!(!app.output.shapes.iter().any(|s| matches!(&s.shape,Shape::Text(t) if t.galley.text()==removed)));
    }
    assert_eq!(app.changes,0);
    app.click("Two passes (full resolution, quality)");
    expected["session_settings"]["video"]["encoder_config"]["nvenc"]["multi_pass"]["variant"]="FullResolution".into();
    assert_eq!(configuration(app.session.session()),expected);
    assert_eq!(app.changes,1);
    app.tab=SettingsTab::new();app.tab.update_session(&app.session.session().session_settings);
    app.settle();app.position("Two passes (full resolution, quality)");
    app.tab.preview_page(4);app.tab.search="multi pass".into();app.settle();
    assert!(!app.output.shapes.iter().any(|s| matches!(&s.shape,Shape::Text(t) if t.galley.text()=="Multi pass")));
}

#[test]
fn typed_bitrate_commits_on_enter_and_focus_loss() {
    let mut app=Harness::new();
    app.size=egui::vec2(1100.0,1200.0);
    app.session.set_session_values(vec![surfxr_packets::PathValuePair {
        path: surfxr_packets::parse_path("session_settings.video.bitrate.mode.ConstantMbps").unwrap(),
        value: 200.into(),
    }]).unwrap();
    app.tab.update_session(&app.session.session().session_settings);
    app.click("200");
    app.frame(vec![Event::Text("300".into())]);
    app.frame(vec![Event::Key {key:egui::Key::Enter,physical_key:None,pressed:true,repeat:false,modifiers:Modifiers::NONE}]);
    app.frame(vec![Event::Key {key:egui::Key::Enter,physical_key:None,pressed:false,repeat:false,modifiers:Modifiers::NONE}]);
    app.settle();
    assert!(matches!(app.session.settings().video.bitrate.mode,surfxr_session::BitrateMode::ConstantMbps(300)));
    app.click("300");
    app.frame(vec![Event::Text("450".into())]);
    app.click("VR bitrate");
    app.settle();
    assert!(matches!(app.session.settings().video.bitrate.mode,surfxr_session::BitrateMode::ConstantMbps(450)));
    assert_eq!(app.changes,2);
    app.tab=SettingsTab::new();
    app.tab.update_session(&app.session.session().session_settings);
    app.settle();app.position("450");
}

#[test]
fn quick_picture_codec_and_bitrate_keep_paths_and_defaults() {
    let mut app = Harness::new();
    app.size = egui::vec2(1100.0, 1200.0);
    let defaults = configuration(app.session.session());
    assert_eq!(defaults["session_settings"]["video"]["preferred_codec"]["variant"], "Hevc");
    app.click_in_row("Codec preset", "H264");
    app.click_in_row("Codec preset", "Reset");
    assert_eq!(configuration(app.session.session()), defaults);
    app.position("VR bitrate");
    app.position("Enable Dynamic Foveated Streaming");
    app.click("Adaptive");
    assert!(matches!(app.session.settings().video.bitrate.mode, surfxr_session::BitrateMode::Adaptive { .. }));
    app.click("Constant");
    assert_eq!(configuration(app.session.session()), defaults);
    app.click("Sound");
    app.position("Headset speaker");
    app.click("Picture & performance");
    app.position("VR bitrate");
}

#[test]
fn legacy_conflict_is_explicit_and_resolved_only_by_a_user_edit() {
    let mut app = Harness::new();
    app.size = egui::vec2(1100.0, 2200.0);
    app.session.set_session_values(vec![surfxr_packets::PathValuePair {
        path: surfxr_packets::parse_path("session_settings.video.foveated_encoding.enabled").unwrap(),
        value: true.into(),
    }]).unwrap();
    app.tab.update_session(&app.session.session().session_settings);
    app.settle();
    assert_eq!(app.changes, 0);
    app.click("Use Dynamic Foveated Streaming");
    assert!(app.session.settings().video.frame_gaze.enabled);
    assert!(app.session.settings().video.foveated_encoding.as_option().is_none());
}

#[test]
fn advanced_fixed_switch_also_disables_gaze() {
    let mut app = Harness::new();
    app.tab.preview_page(4);
    app.tab.search = "foveated encoding".into();
    app.toggle("Foveated encoding");
    assert!(!app.session.settings().video.frame_gaze.enabled);
    assert!(app.session.settings().video.foveated_encoding.as_option().is_some());
}

#[test]
fn desktop_picture_is_in_quick_setup_only() {
    let mut app = Harness::new();
    app.size = egui::vec2(1100.0, 2200.0);
    app.settle();
    app.position("Desktop frame rate");
    app.position("Bitrate per monitor");
    app.position("Monitors to stream");
    app.tab.open_desktop();
    app.settle();
    assert!(!app.output.shapes.iter().any(|shape| matches!(
        &shape.shape, Shape::Text(text) if text.galley.text() == "Desktop frame rate"
    )));
    app.position("Keyboard & mouse input");
    assert_eq!(app.changes, 0);
}

#[test]
fn desktop_pointer_priority_uses_the_accepted_session_writer() {
    let mut app=Harness::new();
    let original=configuration(app.session.session());
    app.tab.open_desktop();app.settle();
    assert_eq!(app.changes,0);
    app.click("PC mouse");
    assert!(app.session.session().frame_settings_sync.prefer_pc_mouse);
    assert_eq!(configuration(app.session.session()),original);
    app.click("VR controller");
    assert!(!app.session.session().frame_settings_sync.prefer_pc_mouse);
    assert_eq!(app.changes,2);
}
