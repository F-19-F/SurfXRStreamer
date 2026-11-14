use super::*;
use egui::{Context, Pos2, RawInput, Rect, epaint::Shape};

#[test]
fn desktop_connection_is_visible_without_steamvr_and_does_not_overwrite_vr_state() {
    crate::i18n::set_language(crate::i18n::Language::English);
    let ctx = Context::default();
    let mut config = SessionConfig::default();
    config.client_connections.insert("frame-test".into(), ClientConnectionConfig {
        trusted: true, display_name: "Test Frame".into(),
        current_ip: None, manual_ips: Default::default(), connection_state: ConnectionState::Disconnected,
    });
    let session = crate::frame_desktop::sessions::Session {
        id: [5; 32], hostname: "frame-test".into(), ip: "192.0.2.10".parse().unwrap(),
        system: true, mode: "Desktop streaming", disconnecting: false,
    };
    let mut devices = DevicesTab::new(); devices.update_client_list(&config);
    for connected in [true, false] {
        let mut labels = vec![];
        for _ in 0..3 {
            let output = ctx.run_ui(RawInput { screen_rect: Some(Rect::from_min_size(Pos2::ZERO, egui::vec2(1000.0, 900.0))),
                ..Default::default() }, |ui| {
                let sessions = if connected { std::slice::from_ref(&session) } else { &[] };
                assert!(devices.ui_with_sessions(ui, false, sessions).is_empty());
            });
            labels = output.shapes.iter().filter_map(|shape| match &shape.shape {
                Shape::Text(text) => Some(text.galley.text().to_string()), _ => None,
            }).collect::<Vec<_>>();
        }
        assert_eq!(labels.iter().any(|s| s == "Desktop streaming"), connected);
        assert_eq!(labels.iter().any(|s| s == "Disconnect session"), connected);
        assert_eq!(labels.iter().any(|s| s == "Disconnected"), !connected);
        assert_eq!(labels.iter().any(|s| s == "SteamVR is not running"), !connected);
        if connected { assert!(labels.iter().any(|s| s == "frame-test: 192.0.2.10")); }
    }
    assert_eq!(config.client_connections["frame-test"].connection_state, ConnectionState::Disconnected);
}
