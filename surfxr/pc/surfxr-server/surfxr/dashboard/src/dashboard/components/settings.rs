use super::{
    NestingInfo, SettingControl,
    presets::{PresetControl, builtin_schema},
};
use crate::dashboard::ServerRequest;
use surfxr_gui_common::DisplayString;
use surfxr_session::{SessionSettings, Settings};
use eframe::egui::{Grid, RichText, ScrollArea, Ui};
#[cfg(target_arch = "wasm32")]
use instant::Instant;
use serde_json as json;
use settings_schema::SchemaNode;
use std::time::Duration;
#[cfg(not(target_arch = "wasm32"))]
use std::time::Instant;

const DATA_UPDATE_INTERVAL: Duration = Duration::from_secs(1);
const MIN_COLUMN_SIZE: f32 = 240.0;

#[cfg(all(test, not(target_arch = "wasm32")))]
#[path = "settings_tests.rs"]
mod tests;

struct TopLevelEntry {
    id: DisplayString,
    control: SettingControl,
}

pub struct SettingsTab {
    selected_top_tab_id: String,
    page: usize,
    quick_page: usize,
    extended: super::surfxr_settings::SurfXrSettings,
    gaze: super::gaze_settings::GazeSettings,
    bitrate: super::surfxr_settings::Group,
    encoder_analysis: PresetControl,
    desktop_picture: super::surfxr_settings::Group,
    logging: super::surfxr_settings::Group,
    search: String,
    pub prefer_pc_mouse: bool,
    resolution_preset: PresetControl,
    framerate_preset: PresetControl,
    encoder_preset: PresetControl,
    foveation_preset: PresetControl,
    codec_preset: PresetControl,
    game_audio_preset: PresetControl,
    microphone_preset: PresetControl,
    hand_tracking_interaction_preset: PresetControl,
    eye_face_tracking_preset: PresetControl,
    top_level_entries: Vec<TopLevelEntry>,
    session_settings_json: Option<json::Value>,
    last_update_instant: Instant,
}

impl SettingsTab {
    pub fn new() -> Self {
        let nesting_info = NestingInfo {
            path: vec!["session_settings".into()],
            indentation_level: 0,
        };
        let mut schema = Settings::schema(surfxr_session::session_settings_default());
        super::foveation_policy::update_guidance(&mut schema);

        let desktop_picture = super::surfxr_settings::Group::take(
            &mut schema, "Desktop picture",
            "Applied on the next desktop connection. Bitrate is per monitor; unavailable monitors do not overwrite your saved count.",
            &["extra", "frame_desktop"], &["framerate", "bitrate_mbps", "monitor_count"],
        );
        let extended = super::surfxr_settings::SurfXrSettings::new(&mut schema);
        let logging = super::surfxr_settings::Group::take(&mut schema,
            "Logging", "Warnings by default. Debug enables detailed diagnostic logs. Changes apply immediately.",
            &["extra", "logging"], &["log_level", "show_raw_events", "log_to_disk"]);
        let gaze = super::gaze_settings::GazeSettings::new(&mut schema);
        let bitrate = super::surfxr_settings::Group::take(
            &mut schema, "VR bitrate",
            "Higher bitrate improves detail but needs more wireless bandwidth. Constant keeps your target; Adaptive follows connection capacity.",
            &["video", "bitrate"], &["mode"],
        );
        super::surfxr_settings::take_entry(&mut schema, &["video", "encoder_config", "nvenc", "multi_pass"]);
        // Hide upstream-client controls only in the Dashboard projection. The full
        // session schema stays compatible; the original client implementation is removed.
        for path in [
            &["video", "passthrough"][..],
            &["video", "max_buffering_frames"],
            &["video", "buffering_history_weight"],
            &["video", "force_software_decoder"],
            &["video", "mediacodec_extra_options"],
            &["video", "clientside_foveation"],
            &["video", "clientside_post_processing"],
            &["video", "upscaling"],
            &["headset", "performance_level"],
            &["connection", "stream_protocol"],
            &["connection", "wired_client_type"],
            &["connection", "wired_client_autolaunch"],
            &["connection", "client_buffer_config"],
            &["extra", "logging", "client_log_report_level"],
            &["extra", "logging", "debug_groups"],
            &["extra", "new_version_popup"],
        ] {
            super::surfxr_settings::take_entry(&mut schema, path);
        }

        // Top level node must be a section
        let SchemaNode::Section { entries, .. } = schema else {
            unreachable!();
        };

        let top_level_entries = entries
            .into_iter()
            .map(|entry| {
                let id = entry.name;
                let display = super::get_display_name(&id, &entry.strings);

                let mut nesting_info = nesting_info.clone();
                nesting_info.path.push(id.clone().into());

                TopLevelEntry {
                    id: DisplayString { id, display },
                    control: SettingControl::new(nesting_info, entry.content),
                }
            })
            .collect();

        Self {
            selected_top_tab_id: "video".into(),
            page: 0,
            quick_page: 0,
            extended,
            gaze,
            bitrate,
            encoder_analysis: PresetControl::new(builtin_schema::multipass_schema()),
            desktop_picture,
            logging,
            search: String::new(),
            prefer_pc_mouse: false,
            resolution_preset: PresetControl::new(builtin_schema::resolution_schema()),
            framerate_preset: PresetControl::new(builtin_schema::framerate_schema()),
            encoder_preset: PresetControl::new(builtin_schema::encoder_preset_schema()),
            foveation_preset: PresetControl::new(builtin_schema::foveation_preset_schema()),
            codec_preset: PresetControl::new(builtin_schema::codec_preset_schema()),
            game_audio_preset: PresetControl::new(builtin_schema::game_audio_schema()),
            microphone_preset: PresetControl::new(builtin_schema::microphone_schema()),
            hand_tracking_interaction_preset: PresetControl::new(
                builtin_schema::hand_tracking_interaction_schema(),
            ),
            eye_face_tracking_preset: PresetControl::new(builtin_schema::eye_face_tracking_schema()),
            top_level_entries,
            session_settings_json: None,
            last_update_instant: Instant::now(),
        }
    }

    pub fn update_session(&mut self, session_settings: &SessionSettings) {
        let settings_json = json::to_value(session_settings).unwrap();

        self.resolution_preset
            .update_session_settings(&settings_json);
        self.framerate_preset
            .update_session_settings(&settings_json);
        self.encoder_preset.update_session_settings(&settings_json);
        self.encoder_analysis.update_session_settings(&settings_json);
        self.foveation_preset
            .update_session_settings(&settings_json);
        self.codec_preset.update_session_settings(&settings_json);
        self.game_audio_preset
            .update_session_settings(&settings_json);
        self.microphone_preset
            .update_session_settings(&settings_json);
        self.hand_tracking_interaction_preset
            .update_session_settings(&settings_json);
        self.eye_face_tracking_preset
            .update_session_settings(&settings_json);

        self.session_settings_json = Some(settings_json);
    }

    pub fn open_desktop(&mut self) {
        self.page = 1;
        self.extended.open_desktop();
    }

    #[cfg(not(target_arch = "wasm32"))]
    pub(crate) fn preview_page(&mut self, index: usize) {
        if index >= 9 {
            self.page = 0;
            self.quick_page = index - 8;
            return;
        }
        self.quick_page = 0;
        self.page = if index == 0 {
            0
        } else if index < 4 {
            1
        } else {
            2
        };
        if (1..4).contains(&index) {
            self.extended.select_page(index - 1);
        }
        if index >= 4 {
            self.selected_top_tab_id = self.top_level_entries[index - 4].id.id.clone();
        }
    }

    pub fn ui(&mut self, ui: &mut Ui) -> Vec<ServerRequest> {
        let mut requests = vec![];

        let now = Instant::now();
        if now > self.last_update_instant + DATA_UPDATE_INTERVAL {
            if self.session_settings_json.is_none() {
                requests.push(ServerRequest::GetSession);
            }

            self.last_update_instant = now;
        }

        let mut path_value_pairs = vec![];
        ui.horizontal_wrapped(|ui| {
            for (page, title) in ["Quick setup", "SurfXR features", "Advanced"]
                .iter()
                .enumerate()
            {
                ui.selectable_value(&mut self.page, page, RichText::new(crate::i18n::tr(title)).size(16.0));
            }
        });
        ui.add(eframe::egui::Label::new(RichText::new(crate::i18n::tr("Changes save automatically. Settings marked Restart will restart SteamVR; save your game first.")).weak()).wrap());
        ui.separator();
        if self.session_settings_json.is_none() {
            ui.spinner();
            ui.label(crate::i18n::tr("Loading your settings..."));
            return requests;
        }
        if self.page == 1 {
            path_value_pairs.extend(
                self.extended
                    .ui(ui, self.session_settings_json.as_mut().unwrap(), &mut self.prefer_pc_mouse),
            );
        } else if self.page == 0 {
            ui.horizontal_wrapped(|ui| {
                for (index, title) in ["Picture & performance", "Sound", "Tracking", "Startup & diagnostics"].iter().enumerate() {
                    ui.selectable_value(&mut self.quick_page, index, crate::i18n::tr(title));
                }
            });
            ScrollArea::both().id_salt(("presets_scroll", self.quick_page)).show(ui, |ui| {
                if self.quick_page == 3 {
                    #[cfg(windows)]
                    crate::desktop_service::startup_ui(ui);
                    path_value_pairs.extend(self.logging.ui(ui, self.session_settings_json.as_mut().unwrap()));
                }
                if self.quick_page==0 {ui.add(eframe::egui::Label::new(crate::i18n::tr("SurfXR syncs resolution, refresh rate and bitrate with the connected headset. The latest edit wins. Resolution or refresh changes are saved; restart SteamVR manually to apply.")).wrap());}

                for (index, (title, presets)) in [
                    ("Picture & performance", vec![
                        ("resolution", &mut self.resolution_preset, true),
                        ("framerate", &mut self.framerate_preset, true),
                        ("encoder", &mut self.encoder_preset, true),
                        ("multipass", &mut self.encoder_analysis, true),
                        ("foveation", &mut self.foveation_preset, true),
                        ("codec", &mut self.codec_preset, true),
                    ]),
                    ("Sound", vec![
                        ("speaker", &mut self.game_audio_preset, true),
                    ]),
                    ("Tracking", vec![
                        ("hand", &mut self.hand_tracking_interaction_preset, true),
                        ("eye_face", &mut self.eye_face_tracking_preset, true),
                    ]),
                ].into_iter().enumerate() {
                    if index != self.quick_page { continue; }
                    for (id, preset, enabled) in presets {
                        ui.add_enabled_ui(enabled, |ui| {
                            Grid::new(("preset", id))
                                .num_columns(2)
                                .min_col_width(MIN_COLUMN_SIZE)
                                .show(ui, |ui| path_value_pairs.extend(preset.ui(ui)));
                        });

                    }
                    if title == "Picture & performance" {
                        ui.add(eframe::egui::Label::new(RichText::new(crate::i18n::tr("Selecting fixed foveation turns off Dynamic Foveated Streaming. Enabling Dynamic Foveated Streaming turns off fixed foveation; its quality values are retained.")).weak()).wrap());
                        ui.add_space(8.0);
                        let settings = self.session_settings_json.as_mut().unwrap();
                        path_value_pairs.extend(self.bitrate.ui(ui, settings));
                        ui.add_space(8.0);
                        path_value_pairs.extend(self.gaze.ui(ui, settings));
                        ui.add_space(8.0);
                        path_value_pairs.extend(self.desktop_picture.ui(ui, settings));
                    }
                    ui.add_space(8.0);
                }
            });
        } else {
            ui.label(crate::i18n::tr("Detailed SURFXR settings. Device identities, desktop options and startup behavior are in SurfXR features."));
            ui.label(crate::i18n::tr("Frame rendering and decoder options are configured on the headset. SurfXR media uses UDP."));
            ui.horizontal_wrapped(|ui| {
                for entry in &self.top_level_entries {
                    ui.selectable_value(
                        &mut self.selected_top_tab_id,
                        entry.id.id.clone(),
                        crate::i18n::tr(&entry.id.display),
                    );
                }
            });
            ui.horizontal(|ui| {
                ui.add(
                    eframe::egui::TextEdit::singleline(&mut self.search)
                        .hint_text(crate::i18n::tr("Search this category..."))
                        .desired_width(260.0),
                );
                if !self.search.is_empty() && ui.small_button(crate::i18n::tr("Clear")).clicked() {
                    self.search.clear();
                }
            });
            super::search::set(ui, &self.search.to_lowercase());
            ScrollArea::both()
                .id_salt(format!("{}_scroll", self.selected_top_tab_id))
                .show(ui, |ui| {
                    if self.selected_top_tab_id == "audio" && self.search.is_empty() {
                        Grid::new("advanced_microphone_preset")
                            .num_columns(2)
                            .min_col_width(MIN_COLUMN_SIZE)
                            .show(ui, |ui| path_value_pairs.extend(self.microphone_preset.ui(ui)));
                        if cfg!(windows) {
                            ui.add(eframe::egui::Label::new(RichText::new(crate::i18n::tr("Steam Streaming Microphone sends your headset voice to PC apps. Select it as the microphone in your game or voice app. Choose Disabled to stop sending voice; use the device settings below for a custom audio route.")).weak()).wrap());
                        }
                        ui.separator();
                    }
                    Grid::new(format!("{}_grid", self.selected_top_tab_id))
                        .striped(true)
                        .num_columns(2)
                        .min_col_width(MIN_COLUMN_SIZE)
                        .show(ui, |ui| {
                            if let Some(session_fragment) = &mut self.session_settings_json {
                                let session_fragments_mut =
                                    session_fragment.as_object_mut().unwrap();

                                let entry = self
                                    .top_level_entries
                                    .iter_mut()
                                    .find(|entry: &&mut TopLevelEntry| {
                                        entry.id.id == self.selected_top_tab_id
                                    })
                                    .unwrap();

                                let response = entry.control.ui(
                                    ui,
                                    &mut session_fragments_mut[&entry.id.id],
                                    false,
                                );

                                if let Some(response) = response {
                                    path_value_pairs.push(response);
                                }

                                ui.end_row();
                            }
                        })
                });
            super::search::set(ui, "");
        }

        if !path_value_pairs.is_empty() {
            super::foveation_policy::make_exclusive(&mut path_value_pairs);
            requests.push(ServerRequest::SetSessionValues(path_value_pairs));
        }

        requests
    }
}
