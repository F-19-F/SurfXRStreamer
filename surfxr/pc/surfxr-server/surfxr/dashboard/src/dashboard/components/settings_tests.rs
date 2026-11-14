//! Drive real egui controls and apply their requests through the production session manager.
use super::*;
use surfxr_server_io::ServerSessionManager;
use eframe::egui::{
    self, Event, Modifiers, PointerButton, Pos2, RawInput, Rect, Vec2, epaint::Shape,
};

// Synchronization metadata advances independently of the settings under test.
fn configuration(session: &surfxr_session::SessionConfig) -> serde_json::Value {
    let mut value=serde_json::to_value(session).unwrap();
    value.as_object_mut().unwrap().remove("frame_settings_sync");
    value
}

struct Harness {
    context: egui::Context,
    tab: SettingsTab,
    devices: Option<crate::dashboard::components::DevicesTab>,
    session: ServerSessionManager,
    output: egui::FullOutput,
    changes: usize,
    size: Vec2,
}

impl Harness {
    fn new() -> Self {
        // UI fixtures select a language instead of inheriting the test host.
        crate::i18n::set_language(crate::i18n::Language::English);
        let context = egui::Context::default();
        crate::dashboard::appearance::apply(&context);
        let session = ServerSessionManager::new(None);
        let mut tab = SettingsTab::new();
        tab.update_session(&session.session().session_settings);
        Self {
            context,
            tab,
            devices: None,
            session,
            output: Default::default(),
            changes: 0,
            size: egui::vec2(900.0, 720.0),
        }
    }

    fn frame(&mut self, events: Vec<Event>) {
        let mut requests = vec![];
        self.output = self.context.run_ui(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, self.size)),
                events,
                ..Default::default()
            },
            |ui| {
                egui::CentralPanel::default().show(ui, |ui| {
                    requests.extend(if let Some(devices) = &mut self.devices {
                        devices.ui(ui, true)
                    } else { self.tab.ui(ui) });
                });
            },
        );
        for request in requests {
            if let ServerRequest::SetSessionValues(values) = request {
                self.changes += values.len();
                self.session.set_session_values(values).unwrap();
                self.tab
                    .update_session(&self.session.session().session_settings);
            }
        }
    }

    fn settle(&mut self) {
        for _ in 0..3 {
            self.frame(vec![]);
        }
    }

    fn click(&mut self, text: &str) {
        self.settle();
        let pos = self.position(text);
        self.click_pos(pos);
    }

    fn click_pos(&mut self, pos: Pos2) {
        self.frame(vec![
            Event::PointerMoved(pos),
            Event::PointerButton {
                pos,
                button: PointerButton::Primary,
                pressed: true,
                modifiers: Modifiers::NONE,
            },
        ]);
        self.frame(vec![Event::PointerButton {
            pos,
            button: PointerButton::Primary,
            pressed: false,
            modifiers: Modifiers::NONE,
        }]);
        self.settle();
    }

    fn click_in_row(&mut self, row: &str, button: &str) {
        self.settle();
        let label = self.position(row);
        let pos = self
            .output
            .shapes
            .iter()
            .find_map(|shape| {
                if let Shape::Text(text) = &shape.shape {
                    let center = text.pos + text.galley.size() / 2.0;
                    if text.galley.text() == button
                        && (center.y - label.y).abs() < 8.0
                        && shape.clip_rect.contains(center)
                    {
                        return Some(center);
                    }
                }
                None
            })
            .unwrap_or_else(|| panic!("{button} not visible in {row}"));
        self.click_pos(pos);
    }

    fn toggle(&mut self, row: &str) {
        self.settle();
        let label = self.position(row);
        let pos = self
            .output
            .shapes
            .iter()
            .find_map(|shape| {
                if let Shape::Circle(circle) = &shape.shape {
                    if (circle.center.y - label.y).abs() < 8.0 && circle.center.x > label.x {
                        return Some(circle.center);
                    }
                }
                None
            })
            .expect("Visible toggle in row");
        self.click_pos(pos);
    }

    fn position(&self, text: &str) -> Pos2 {
        let pos = self
            .output
            .shapes
            .iter()
            .find_map(|shape| {
                if let Shape::Text(label) = &shape.shape {
                    let center = label.pos + label.galley.size() / 2.0;
                    if label.galley.text() == text && shape.clip_rect.contains(center) {
                        return Some(center);
                    }
                }
                None
            })
            .unwrap_or_else(|| panic!("Visible text not found: {text}"));
        pos
    }
}

#[test]
fn gaze_and_fixed_presets_switch_exclusively_and_focus_reset_preserves_other_fields() {
    let mut app = Harness::new();
    app.size = egui::vec2(1100.0, 1800.0);
    app.click("Light");
    assert!(!app.session.settings().video.frame_gaze.enabled);
    assert_eq!(app.session.settings().video.foveated_encoding.as_option().unwrap().center_size, [0.8, 0.8]);
    app.toggle("Enable Dynamic Foveated Streaming");
    assert!(app.session.settings().video.frame_gaze.enabled);
    assert!(app.session.settings().video.foveated_encoding.as_option().is_none());
    app.session.set_session_values(vec![surfxr_packets::PathValuePair {
        path: surfxr_packets::parse_path("session_settings.video.frame_gaze.focus_width_percent").unwrap(),
        value: serde_json::json!(33),
    }]).unwrap();
    app.tab.update_session(&app.session.session().session_settings);
    let mut expected = configuration(app.session.session());
    expected["session_settings"]["video"]["frame_gaze"]["focus_width_percent"] = serde_json::json!(25);
    app.click_in_row("Focus width", "Reset");
    assert_eq!(configuration(app.session.session()), expected);
    app.click("High");
    assert!(!app.session.settings().video.frame_gaze.enabled);
    assert!(app.session.settings().video.foveated_encoding.as_option().is_some());
}

#[test]
fn encoder_custom_preserves_values_and_default_restores_tuning() {
    let mut app = Harness::new();
    app.session.set_session_values(vec![
        surfxr_packets::PathValuePair { path: surfxr_packets::parse_path("session_settings.video.encoder_config.nvenc.multi_pass.variant").unwrap(), value: "FullResolution".into() },
        surfxr_packets::PathValuePair { path: surfxr_packets::parse_path("session_settings.video.encoder_config.nvenc.quality_preset.variant").unwrap(), value: "P5".into() },
    ]).unwrap();
    app.tab.update_session(&app.session.session().session_settings);
    let mut expected=configuration(app.session.session());
    app.click_in_row("Encoder preset", "Custom");
    assert_eq!(configuration(app.session.session()),expected);
    assert_eq!(app.changes,0);
    app.click_in_row("Encoder preset", "SurfXR default");
    let nvenc=&mut expected["session_settings"]["video"]["encoder_config"]["nvenc"];
    nvenc["multi_pass"]["variant"]="Disabled".into();
    nvenc["quality_preset"]["variant"]="P1".into();
    assert_eq!(configuration(app.session.session()),expected);
}

#[test]
fn surfxr_foveation_button_and_reset_disable_only_upstream_encoding() {
    let mut app = Harness::new();
    app.session
        .set_session_values(vec![surfxr_packets::PathValuePair {
            path: surfxr_packets::parse_path("session_settings.video.frame_gaze.enabled").unwrap(),
            value: serde_json::json!(false),
        }])
        .unwrap();
    app.tab
        .update_session(&app.session.session().session_settings);
    app.click("Light");
    let mut expected = configuration(app.session.session());
    expected["session_settings"]["video"]["foveated_encoding"]["enabled"] =
        serde_json::json!(false);
    app.click_in_row("Foveation preset", "SurfXR default");
    assert_eq!(
        configuration(app.session.session()),
        expected
    );
    app.click("Light");
    app.click_in_row("Foveation preset", "Reset");
    assert_eq!(
        configuration(app.session.session()),
        expected
    );
}

#[test]
fn browsing_all_pages_does_not_write_settings_and_chinese_has_glyphs() {
    let mut app = Harness::new();
    let before = configuration(app.session.session());
    for size in [egui::vec2(610.0, 460.0), egui::vec2(900.0, 720.0)] {
        app.size = size;
        for page in 0..11 {
            app.tab.preview_page(page);
            app.settle();
        }
    }
    assert_eq!(app.changes, 0);
    assert_eq!(configuration(app.session.session()), before);
    #[cfg(windows)]
    app.context.fonts_mut(|fonts| {
        for family in [egui::FontFamily::Proportional, egui::FontFamily::Monospace] {
            assert!(fonts.has_glyphs(&egui::FontId::new(14.0, family), "中文设备桌面串流控制器"));
        }
    });
}

#[test]
fn surfxr_identity_controls_write_the_original_session_paths() {
    let mut app = Harness::new();
    let defaults = configuration(app.session.session());
    assert_eq!(defaults["session_settings"]["headset"]["emulation_mode"]["variant"], "SteamFrame");
    assert_eq!(defaults["session_settings"]["headset"]["controllers"]["content"]["emulation_mode"]["variant"], "SteamFrame");
    assert_eq!(defaults["session_settings"]["connection"]["stream_protocol"]["variant"], "Udp");
    app.tab.preview_page(1);
    app.click("Steam Frame");
    app.click("Quest 2");
    app.click_in_row("Headset model", "Reset");
    app.click("Steam Frame Controllers");
    app.click("Quest 2 Touch");
    app.click_in_row("Controller model", "Reset");
    let saved = configuration(app.session.session());
    assert_eq!(
        saved["session_settings"]["headset"]["emulation_mode"]["variant"],
        "SteamFrame"
    );
    assert_eq!(
        saved["session_settings"]["headset"]["controllers"]["content"]["emulation_mode"]["variant"],
        "SteamFrame"
    );
    assert_eq!(app.changes, 4);
}

#[test]
#[cfg(windows)]
fn microphone_is_in_advanced_audio_and_steam_preset_restores_the_route() {
    let mut app = Harness::new();
    app.settle();
    assert!(!app.output.shapes.iter().any(|shape| matches!(
        &shape.shape, Shape::Text(text) if text.galley.text() == "Headset microphone"
    )));
    app.position("Godlike (SurfXR default) - 3072 x 3264 / eye");
    app.tab.preview_page(5);
    app.settle();
    app.position("Steam Streaming Microphone");
    let before = configuration(app.session.session());
    app.click("Steam Streaming Microphone");
    app.click("Disabled");
    assert!(app.session.settings().audio.microphone.as_option().is_none());
    app.click_in_row("Headset microphone", "Reset");
    assert_eq!(configuration(app.session.session()), before);
    assert!(matches!(app.session.settings().audio.microphone.as_option().unwrap().devices,
        surfxr_session::MicrophoneDevicesConfig::SteamStreaming));
}

#[test]
fn custom_resolution_can_select_last_preset_without_wrong_index_or_panic() {
    let mut app = Harness::new();
    app.session
        .set_session_values(vec![surfxr_packets::PathValuePair {
            path: surfxr_packets::parse_path(
                "session_settings.video.transcoding_view_resolution.Absolute.width",
            )
            .unwrap(),
            value: serde_json::json!(2222),
        }])
        .unwrap();
    app.tab
        .update_session(&app.session.session().session_settings);
    app.click("Preset not applied");
    let pos = app.position("Medium - 2112 x 2304 / eye");
    app.frame(vec![
        Event::PointerMoved(pos),
        Event::MouseWheel {
            unit: egui::MouseWheelUnit::Point,
            delta: egui::vec2(0.0, -600.0),
            modifiers: Modifiers::NONE,
            phase: egui::TouchPhase::Move,
        },
    ]);
    for _ in 0..20 {
        app.frame(vec![]);
    }
    app.click("Unstoppable - 4992 x 5376 / eye");
    let saved = configuration(app.session.session());
    assert_eq!(
        saved["session_settings"]["video"]["transcoding_view_resolution"]["Absolute"]["width"],
        4992
    );
    assert_eq!(
        saved["session_settings"]["video"]["emulated_headset_view_resolution"]["Absolute"]["width"],
        4992
    );
    for key in ["transcoding_view_resolution", "emulated_headset_view_resolution"] {
        assert_eq!(saved["session_settings"]["video"][key]["Absolute"]["height"]["set"], true);
        assert_eq!(saved["session_settings"]["video"][key]["Absolute"]["height"]["content"], 5376);
    }
}

#[test]
fn advanced_search_finds_nested_controls_without_changing_saved_collapse_state() {
    let mut app = Harness::new();
    app.tab.preview_page(4);
    app.tab.search = "quality preset".into();
    let before = configuration(app.session.session());
    app.settle();
    app.position("Quality preset");
    assert_eq!(app.changes, 0);
    assert_eq!(configuration(app.session.session()), before);
    app.tab.search = "unmatched setting xyz".into();
    app.settle();
    app.position("No matching settings in this category.");
    app.click("Clear");
    assert!(app.tab.search.is_empty());
    app.position("Bitrate");
}

#[test]
fn upstream_client_controls_are_absent_even_from_advanced_search() {
    let mut app = Harness::new();
    let before = configuration(app.session.session());
    for (page, search) in [
        (4, "client-side"), (4, "mediacodec"), (4, "software decoder"),
        (4, "passthrough"), (4, "maximum buffering"),
        (6, "performance level"), (7, "stream protocol"),
        (7, "wired client"), (7, "client buffer"), (8, "client log report"),
    ] {
        app.tab.preview_page(page);
        app.tab.search = search.into();
        app.settle();
        app.position("No matching settings in this category.");
    }
    assert_eq!(app.changes, 0);
    assert_eq!(configuration(app.session.session()), before);
}

#[test]
fn desktop_controls_change_only_the_bound_field_and_reset_through_existing_schema() {
    let mut app = Harness::new();
    app.tab.open_desktop();
    app.settle();
    let before = configuration(app.session.session());
    let label = app.position("Keyboard & mouse input");
    let switch = app
        .output
        .shapes
        .iter()
        .find_map(|shape| {
            if let Shape::Circle(circle) = &shape.shape {
                if (circle.center.y - label.y).abs() < 8.0 && circle.center.x > label.x {
                    return Some(circle.center);
                }
            }
            None
        })
        .expect("Input toggle next to its label");
    app.click_pos(switch);
    let mut expected = before;
    expected["session_settings"]["extra"]["frame_desktop"]["allow_input"] =
        serde_json::json!(false);
    assert_eq!(
        configuration(app.session.session()),
        expected
    );
    app.session
        .set_session_values(vec![surfxr_packets::PathValuePair {
            path: surfxr_packets::parse_path("session_settings.extra.frame_desktop.framerate")
                .unwrap(),
            value: serde_json::json!(90),
        }])
        .unwrap();
    app.tab
        .update_session(&app.session.session().session_settings);
    app.tab.preview_page(0);
    app.size = egui::vec2(1100.0, 2200.0);
    app.settle();
    let label = app.position("Desktop frame rate");
    let reset = app
        .output
        .shapes
        .iter()
        .find_map(|shape| {
            if let Shape::Text(text) = &shape.shape {
                let pos = text.pos + text.galley.size() / 2.0;
                if text.galley.text() == "Reset" && (pos.y - label.y).abs() < 8.0 {
                    return Some(pos);
                }
            }
            None
        })
        .unwrap();
    app.click_pos(reset);
    assert_eq!(app.session.settings().extra.frame_desktop.framerate, 60);
    assert!(!app.session.settings().extra.frame_desktop.allow_input);
    assert_eq!(app.changes, 2);
}

#[path = "settings_language_tests.rs"]
mod language_tests;

#[path = "settings_picture_tests.rs"]
mod picture_tests;

#[path = "settings_diagnostics_tests.rs"]
mod diagnostics_tests;
