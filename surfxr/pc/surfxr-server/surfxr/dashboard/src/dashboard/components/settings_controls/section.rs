use super::{INDENTATION_STEP, NestingInfo, SettingControl, collapsible, notice, search};
use surfxr_gui_common::{
    DisplayString,
    theme::{
        OK_GREEN,
        log_colors::{INFO_LIGHT, WARNING_LIGHT},
    },
};
use surfxr_packets::PathValuePair;
use surfxr_session::settings_schema::{SchemaEntry, SchemaNode};
use eframe::egui::{RichText, Ui};
use serde_json as json;

struct Entry {
    id: DisplayString,
    help: Option<String>,
    notice: Option<String>,
    hidden: bool,
    steamvr_restart_flag: bool,
    real_time_flag: bool,
    control: SettingControl,
    search_text: String,
    own_search_text: String,
}

pub struct Control {
    nesting_info: NestingInfo,
    entries: Vec<Entry>,
    gui_collapsible: bool,
}

impl Control {
    pub fn new(
        mut nesting_info: NestingInfo,
        schema_entries: Vec<SchemaEntry<SchemaNode>>,
        gui_collapsible: bool,
    ) -> Self {
        nesting_info.indentation_level += 1;

        let entries = schema_entries
            .into_iter()
            .map(|entry| {
                let id = entry.name;
                let display = super::get_display_name(&id, &entry.strings);
                let help = entry.strings.get("help").cloned();
                let notice = entry.strings.get("notice").cloned();
                let hidden = entry.flags.contains("hidden");
                let steamvr_restart_flag = entry.flags.contains("steamvr-restart");
                let real_time_flag = entry.flags.contains("real-time");
                let own_search_text = format!(
                    "{} {}",
                    display,
                    entry
                        .strings
                        .values()
                        .cloned()
                        .collect::<Vec<_>>()
                        .join(" ")
                )
                .to_lowercase();
                let search_text = format!(
                    "{} {}",
                    own_search_text,
                    search::schema_text(&entry.content)
                );

                let mut nesting_info = nesting_info.clone();
                nesting_info.path.push(id.clone().into());

                Entry {
                    id: DisplayString { id, display },
                    help,
                    notice,
                    hidden,
                    steamvr_restart_flag,
                    real_time_flag,
                    search_text,
                    own_search_text,
                    control: SettingControl::new(nesting_info, entry.content),
                }
            })
            .collect();

        Self {
            nesting_info,
            entries,
            gui_collapsible,
        }
    }

    pub fn ui(
        &mut self,
        ui: &mut Ui,
        session_fragment: &mut json::Value,
        allow_inline: bool,
    ) -> Option<PathValuePair> {
        let query = search::query(ui);
        let entries_count = self
            .entries
            .iter()
            .filter(|entry| !entry.hidden && search::matches(&entry.search_text, &query))
            .count();

        let mut request = None;

        let collapsed = if self.gui_collapsible && query.is_empty() {
            super::grid_flow_inline(ui, allow_inline);

            let collapsed = collapsible::collapsible_button(
                ui,
                &self.nesting_info,
                session_fragment,
                &mut request,
            );

            if !collapsed {
                ui.end_row();
            }

            collapsed
        } else {
            if allow_inline {
                ui.end_row();
            }

            false
        };

        if !collapsed {
            if entries_count == 0 {
                ui.label(crate::i18n::tr("No matching settings in this category."));
            }
            for (i, entry) in self
                .entries
                .iter_mut()
                .filter(|entry| !entry.hidden && search::matches(&entry.search_text, &query))
                .enumerate()
            {
                ui.horizontal(|ui| {
                    ui.add_space(INDENTATION_STEP * self.nesting_info.indentation_level as f32);
                    let label_res = ui.label(crate::i18n::tr(&entry.id.display));
                    if cfg!(debug_assertions) {
                        label_res.on_hover_text_at_pointer(&*entry.id);
                    }

                    if let Some(string) = &entry.help {
                        ui.colored_label(INFO_LIGHT, RichText::new(crate::i18n::tr("?")).small())
                            .on_hover_text_at_pointer(crate::i18n::tr(string));
                    }
                    if entry.steamvr_restart_flag {
                        ui.colored_label(WARNING_LIGHT, RichText::new(crate::i18n::tr("Restart")).small())
                            .on_hover_text_at_pointer(
                                crate::i18n::tr("Changing this setting will make SteamVR restart! Please save your in-game progress first"),
                            );
                    }
                    if entry.real_time_flag {
                        // Text badges remain readable with all supported fonts.
                        ui.colored_label(OK_GREEN, RichText::new(crate::i18n::tr("Live")).small())
                            .on_hover_text_at_pointer(
                                crate::i18n::tr("This setting can be changed in real-time during streaming!"),
                            );
                    }
                    if let Some(string) = &entry.notice {
                        ui.push_id(&entry.id.id, |ui| notice::notice(ui, string));
                    }
                });

                if search::matches(&entry.own_search_text, &query) {
                    search::set(ui, "");
                }
                request = entry
                    .control
                    .ui(ui, &mut session_fragment[&entry.id.id], true)
                    .or(request);
                search::set(ui, &query);

                if i != entries_count - 1 {
                    ui.end_row();
                }
            }
        }

        request
    }
}
