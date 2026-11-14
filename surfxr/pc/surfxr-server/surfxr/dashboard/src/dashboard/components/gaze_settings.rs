//! SurfXR gaze preferences use the original schema writer and restart markers.
use super::surfxr_settings::{Group, take_entry};
use surfxr_packets::PathValuePair;
use eframe::egui::{self, Ui};
use serde_json::Value;
use settings_schema::SchemaNode;

pub struct GazeSettings {
    toggle: Group,
    region: Group,
}

impl GazeSettings {
    pub fn new(schema: &mut SchemaNode) -> Self {
        let parent = &["video", "frame_gaze"];
        let toggle = Group::take(
            schema,
            "Dynamic Foveated Streaming",
            "Enable on both PC and Frame. Changes restart SteamVR.",
            parent,
            &["enabled"],
        );
        let region = Group::take(
            schema,
            "High-resolution focus region",
            "Size per eye. Default: 25% x 25%. Larger regions cost more.",
            parent,
            &["focus_width_percent", "focus_height_percent"],
        );
        take_entry(schema, parent);
        Self { toggle, region }
    }

    pub fn ui(&mut self, ui: &mut Ui, settings: &mut Value) -> Vec<PathValuePair> {
        let mut changes = vec![];
        if let Some(change) = self.toggle.ui(ui, settings) {
            changes.push(change);
        }
        if settings["video"]["frame_gaze"]["enabled"] == true
            && settings["video"]["foveated_encoding"]["enabled"] == true {
            ui.colored_label(egui::Color32::YELLOW, crate::i18n::tr("Saved settings enable both foveation modes. Select one mode to resolve the conflict."));
            if ui.button(crate::i18n::tr("Use Dynamic Foveated Streaming")).clicked() {
                changes.push(PathValuePair {
                    path: surfxr_packets::parse_path("session_settings.video.frame_gaze.enabled").unwrap(),
                    value: true.into(),
                });
            }
        }
        ui.add_space(10.0);
        let enabled = settings["video"]["frame_gaze"]["enabled"]
            .as_bool()
            .unwrap_or(true);
        ui.add_enabled_ui(enabled, |ui| {
            if let Some(change) = self.region.ui(ui, settings) {
                changes.push(change);
            }
        });
        ui.add_space(8.0);
        ui.collapsing(crate::i18n::tr("About Dynamic Foveated Streaming"), |ui| {
            ui.add(egui::Label::new(crate::i18n::tr("A full-resolution region follows your eyes over a lower-resolution full view. Width and height are linear percentages of one eye's image, not its area. Larger regions cover more eye movement but increase encoding and decoding work.")).wrap());
            ui.add(egui::Label::new(crate::i18n::tr("Selecting fixed foveation turns off Dynamic Foveated Streaming. Enabling Dynamic Foveated Streaming turns off fixed foveation; its quality values are retained.")).wrap());
            ui.add(egui::Label::new(crate::i18n::tr("Requires Windows, UDP, H.264/HEVC, SDR 8-bit and gamma 1. If the Frame session cannot use the gaze atlas, it uses full-frame video. Focus regions support 10-33% per axis and round up to 32 pixels. This does not change the game's rendering resolution.")).wrap());
        });
        changes
    }
}
