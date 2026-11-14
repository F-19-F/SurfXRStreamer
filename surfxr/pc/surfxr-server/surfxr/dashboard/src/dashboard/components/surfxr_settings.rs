//! Present existing schema controls at their original JSON paths, without migrating settings.
use super::{NestingInfo, SettingControl};
use surfxr_packets::PathValuePair;
use surfxr_session::settings_schema::{SchemaEntry, SchemaNode};
use eframe::egui::{self, Frame, Grid, RichText, Ui};
use serde_json::Value;

pub struct Group {
    pub title: &'static str,
    pub description: &'static str,
    parent: Vec<String>,
    control: SettingControl,
}

pub struct SurfXrSettings {
    selected: usize,
    groups: Vec<Vec<Group>>,
}

// Switch/optional contents are real path components in the saved session.
pub(super) fn take_entry(node: &mut SchemaNode, path: &[&str]) -> SchemaEntry<SchemaNode> {
    if path[0] == "content" {
        match node {
            SchemaNode::Switch { content, .. } | SchemaNode::Optional { content, .. } => {
                return take_entry(content, &path[1..]);
            }
            _ => panic!("Expected schema content at {path:?}"),
        }
    }
    let SchemaNode::Section { entries, .. } = node else {
        panic!("Expected schema section at {path:?}");
    };
    let index = entries
        .iter()
        .position(|entry| entry.name == path[0])
        .unwrap();
    if path.len() == 1 {
        entries.remove(index)
    } else {
        take_entry(&mut entries[index].content, &path[1..])
    }
}

impl Group {
    pub(super) fn take(
        schema: &mut SchemaNode,
        title: &'static str,
        description: &'static str,
        parent: &[&str],
        names: &[&str],
    ) -> Self {
        let entries = names
            .iter()
            .map(|name| {
                let mut path = parent.to_vec();
                path.push(name);
                let mut entry = take_entry(schema, &path);
                let label = match (parent.last().copied(), *name) {
                    (Some("headset"), "emulation_mode") => Some("Headset model"),
                    (Some("content"), "emulation_mode") => Some("Controller model"),
                    (Some("frame_desktop"), "enabled") => Some("Allow desktop streaming"),
                    (Some("frame_desktop"), "allow_input") => Some("Keyboard & mouse input"),
                    (Some("frame_desktop"), "allow_audio") => Some("Desktop audio"),
                    (Some("frame_desktop"), "require_system_service") => {
                        Some("Require administrator service")
                    }
                    (Some("frame_desktop"), "framerate") => Some("Desktop frame rate"),
                    (Some("frame_desktop"), "bitrate_mbps") => Some("Bitrate per monitor"),
                    (Some("frame_desktop"), "monitor_count") => Some("Monitors to stream"),
                    (Some("steamvr_launcher"), "enable_surfxr_on_manual_steamvr_start") => {
                        Some("Load SurfXR on manual SteamVR start")
                    }
                    (Some("steamvr_launcher"), "open_close_steamvr_with_dashboard") => {
                        Some("Start / stop SteamVR with Dashboard")
                    }
                    _ => None,
                };
                if let Some(label) = label {
                    entry.strings.insert("display_name".into(), label.into());
                }
                if parent.last() == Some(&"frame_desktop") {
                    if let SchemaNode::Number { suffix, .. } = &mut entry.content {
                        if *name == "framerate" {
                            *suffix = Some(" Hz".into());
                        }
                        if *name == "bitrate_mbps" {
                            *suffix = Some(" Mbps".into());
                        }
                    }
                }
                if parent.last() == Some(&"nvenc") && *name == "multi_pass" {
                    if let SchemaNode::Choice { variants, .. } = &mut entry.content {
                        for variant in variants {
                            let label = match variant.name.as_str() {
                                "Disabled" => "Single pass (lower latency)",
                                "QuarterResolution" => "Two passes (quarter resolution)",
                                "FullResolution" => "Two passes (full resolution, quality)",
                                _ => continue,
                            };
                            variant.strings.insert("display_name".into(), label.into());
                        }
                    }
                }
                entry
            })
            .collect();
        Self {
            title,
            description,
            parent: parent.iter().map(|part| (*part).into()).collect(),
            control: SettingControl::new(
                NestingInfo {
                    path: std::iter::once("session_settings")
                        .chain(parent.iter().copied())
                        .map(Into::into)
                        .collect(),
                    indentation_level: 0,
                },
                SchemaNode::Section {
                    entries,
                    gui_collapsible: false,
                },
            ),
        }
    }

    pub(super) fn ui(&mut self, ui: &mut Ui, settings: &mut Value) -> Option<PathValuePair> {
        let mut fragment = settings;
        for part in &self.parent {
            fragment = fragment.get_mut(part)?;
        }
        Frame::group(ui.style())
            .fill(egui::Color32::from_rgb(40, 40, 40))
            .inner_margin(16)
            .show(ui, |ui| {
                ui.set_width((ui.available_width() - 2.0).max(0.0));
                ui.label(RichText::new(crate::i18n::tr(self.title)).strong().size(17.0));
                ui.label(RichText::new(crate::i18n::tr(self.description)).weak());
                Grid::new(self.title)
                    .num_columns(2)
                    .spacing([20.0, 12.0])
                    .min_col_width(240.0)
                    .show(ui, |ui| self.control.ui(ui, fragment, false))
                    .inner
            })
            .inner
    }
}

impl SurfXrSettings {
    pub const TABS: [&'static str; 3] = [
        "Devices & input",
        "Desktop streaming",
        "Startup & connection",
    ];

    pub fn new(schema: &mut SchemaNode) -> Self {
        let devices = vec![
            Group::take(
                schema,
                "Headset identity",
                "Choose the headset that games see in SteamVR. Changes restart SteamVR.",
                &["headset"],
                &["emulation_mode"],
            ),
            Group::take(
                schema,
                "Controller identity",
                "Steam Frame Controllers exposes the full Frame button layout. Changes restart SteamVR.",
                &["headset", "controllers", "content"],
                &["emulation_mode"],
            ),
        ];
        let desktop = vec![
            Group::take(
                schema,
                "Access & sound",
                "Uses the devices trusted in Devices. Revoking access or changing permissions ends the current desktop session.",
                &["extra", "frame_desktop"],
                &[
                    "enabled",
                    "allow_input",
                    "allow_audio",
                    "require_system_service",
                ],
            ),
            Group::take(
                schema,
                "Desktop & VR continuity",
                "Tracking changes apply immediately. Media encryption applies on the next unified connection; authentication and reliable control stay encrypted.",
                &["extra", "frame_desktop"],
                &["keep_steamvr_tracking", "media_encryption"],
            ),
        ];
        // The now-empty container should not leave a duplicate heading in Advanced.
        take_entry(schema, &["extra", "frame_desktop"]);
        let startup = vec![Group::take(
            schema,
            "SteamVR startup",
            "Headset and Dashboard starts use the same policy. Leave Direct launch off for Steam's VR menu. Enable it only to bypass Steam startup or cloud-sync waits; the Steam menu may be unavailable.",
            &["extra", "steamvr_launcher"],
            &[
                "enable_surfxr_on_manual_steamvr_start",
                "open_close_steamvr_with_dashboard",
                "direct_launch",
            ],
        )];
        take_entry(schema, &["extra", "steamvr_launcher"]);
        Self {
            selected: 0,
            groups: vec![devices, desktop, startup],
        }
    }

    pub fn open_desktop(&mut self) {
        self.selected = 1;
    }

    #[cfg(not(target_arch = "wasm32"))]
    pub(crate) fn select_page(&mut self, index: usize) {
        self.selected = index;
    }

    pub fn ui(&mut self, ui: &mut Ui, settings: &mut Value, prefer_pc: &mut bool) -> Vec<PathValuePair> {
        ui.horizontal_wrapped(|ui| {
            for (index, title) in Self::TABS.iter().enumerate() {
                ui.selectable_value(&mut self.selected, index, crate::i18n::tr(title));
            }
        });
        let mut changes = vec![];
        egui::ScrollArea::both().id_salt(("surfxr_settings", self.selected)).show(ui, |ui| {
            if self.selected == 0 {
                ui.label(crate::i18n::tr("Frame input and desktop integration live here. Picture, bitrate and Dynamic Foveated Streaming are in Quick setup."));
                if settings["headset"]["controllers"]["enabled"] == false {
                    ui.label(RichText::new(crate::i18n::tr("Controller forwarding is off. Enable it in Advanced > Headset > Controllers to use the selected identity.")).color(egui::Color32::from_rgb(240, 190, 100)));
                }
            }
            if self.selected == 1 {
                ui.group(|ui| {
                    ui.strong(crate::i18n::tr("Cursor priority when both move"));
                    let before=*prefer_pc;
                    ui.horizontal(|ui| {
                        ui.selectable_value(prefer_pc,false,crate::i18n::tr("VR controller"));
                        ui.selectable_value(prefer_pc,true,crate::i18n::tr("PC mouse"));
                    });
                    ui.label(crate::i18n::tr("Only the moving device controls the cursor. This preference applies when both move and syncs with the headset immediately."));
                    if before!=*prefer_pc { changes.push(PathValuePair {
                        path:vec!["frame_settings_sync".into(),"prefer_pc_mouse".into()],
                        value:serde_json::json!(*prefer_pc),
                    }); }
                });
            }
            for group in &mut self.groups[self.selected] {
                if let Some(change) = group.ui(ui, settings) { changes.push(change); }
            }
            if self.selected == 0 {
                Frame::group(ui.style()).inner_margin(16).show(ui, |ui| {
                    ui.strong(crate::i18n::tr("Frame gaze & system buttons"));
                    ui.label(crate::i18n::tr("Enable Dynamic Foveated Streaming and adjust its focus region in Quick setup > Picture & performance. The Frame headset must also enable foveated streaming. Shared eye-tracking options remain in Advanced > Headset."));
                    ui.label(crate::i18n::tr("Full Controller Capture is controlled on the headset. Press either system button four times briefly to leave capture."));
                });
            }
        });
        changes
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_session::{SessionConfig, Settings};

    #[test]
    fn regrouped_controls_keep_original_paths_and_defaults() {
        let mut schema = Settings::schema(surfxr_session::session_settings_default());
        let groups = SurfXrSettings::new(&mut schema);
        let session = serde_json::to_value(SessionConfig::default()).unwrap();
        for group in groups.groups.iter().flatten() {
            let mut value = &session["session_settings"];
            for part in &group.parent {
                value = value.get(part).unwrap();
            }
            assert!(
                value.is_object(),
                "{} must bind to an existing object",
                group.title
            );
        }
        // Ordinary headset controls and all original options still exist after extracting identities.
        let controllers = take_entry(&mut schema, &["headset", "controllers"]);
        let SchemaNode::Switch { content, .. } = controllers.content else {
            panic!()
        };
        let SchemaNode::Section { entries, .. } = *content else {
            panic!()
        };
        assert!(!entries.iter().any(|e| e.name == "emulation_mode"));
        assert!(entries.len() > 5);
    }
}
