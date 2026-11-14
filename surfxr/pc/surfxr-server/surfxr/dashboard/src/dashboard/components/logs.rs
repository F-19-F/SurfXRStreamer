use surfxr_common::LogSeverity;
use surfxr_events::{Event, EventType};
use surfxr_gui_common::theme::log_colors;
use surfxr_session::{RawEventsConfig, Settings};
use eframe::{
    egui::{Grid, OpenUrl, OutputCommand, RichText, ScrollArea, Ui},
    epaint::Color32,
};
use settings_schema::Switch;
use std::collections::VecDeque;

struct Entry {
    color: Color32,
    timestamp: String,
    ty: String,
    message: String,
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn normal_logs_hide_details_but_keep_warnings_and_debug_opt_in() {
        let mut tab = LogsTab::new();
        for severity in [LogSeverity::Debug, LogSeverity::Info, LogSeverity::Warning, LogSeverity::Error] {
            tab.push_event(Event { timestamp: String::new(), event_type: EventType::Log(surfxr_common::LogEntry {
                severity, content: format!("{severity:?}"),
            }) });
        }
        assert_eq!(tab.entries.len(), 2);
        assert_eq!(tab.entries[0].message, "Warning");
        tab.level = LogSeverity::Debug;
        tab.push_event(Event { timestamp: String::new(), event_type: EventType::Log(surfxr_common::LogEntry {
            severity: LogSeverity::Debug, content: "details".into(),
        }) });
        assert_eq!(tab.entries.len(), 3);
    }
}

pub struct LogsTab {
    raw_events_config: Switch<RawEventsConfig>,
    entries: VecDeque<Entry>,
    log_limit: usize,
    level: LogSeverity,
}

impl LogsTab {
    pub fn new() -> Self {
        Self {
            raw_events_config: Switch::Disabled,
            level: LogSeverity::Warning,
            entries: VecDeque::new(),
            log_limit: 1000,
        }
    }

    pub fn update_settings(&mut self, settings: &Settings) {
        self.raw_events_config = settings.extra.logging.show_raw_events.clone();
        self.level = settings.extra.logging.log_level;
        #[cfg(not(target_arch = "wasm32"))]
        crate::logging_backend::set_level(self.level);
    }

    pub fn push_event(&mut self, event: Event) {
        if let EventType::Log(entry) = &event.event_type {
            if entry.severity.into_log_level() > self.level.into_log_level() { return; }
        } else if self.level != LogSeverity::Debug { return; }
        let color = if let EventType::Log(entry) = &event.event_type {
            Some(match entry.severity {
                LogSeverity::Error => log_colors::ERROR_LIGHT,
                LogSeverity::Warning => log_colors::WARNING_LIGHT,
                LogSeverity::Info => log_colors::INFO_LIGHT,
                LogSeverity::Debug => log_colors::DEBUG_LIGHT,
            })
        } else if let Switch::Enabled(config) = &self.raw_events_config {
            (!config.hide_spammy_events
                || !matches!(
                    event.event_type,
                    EventType::StatisticsSummary(_)
                        | EventType::GraphStatistics(_)
                        | EventType::Tracking(_)
                ))
            .then_some(log_colors::EVENT_LIGHT)
        } else {
            None
        };

        if let Some(color) = color {
            self.entries.push_back(Entry {
                color,
                timestamp: event.timestamp.clone(),
                ty: event.event_type_string(),
                message: event.message(),
            });

            if self.entries.len() > self.log_limit {
                self.entries.pop_front();
            }
        }
    }

    pub fn ui(&mut self, ui: &mut Ui) {
        ui.label(crate::i18n::tr("Log level is in Quick setup > Startup & diagnostics. Detailed events require Debug and Show raw events."));
        ui.horizontal(|ui| {
            if ui.button(crate::i18n::tr("Copy all")).clicked() {
                ui.output_mut(|out| {
                    out.commands
                        .push(OutputCommand::CopyText(self.entries.iter().fold(
                            String::new(),
                            |acc, entry| {
                                format!(
                                    "{}{} [{}] {}\n",
                                    acc, entry.timestamp, entry.ty, entry.message
                                )
                            },
                        )));
                })
            }
            if ui.button(crate::i18n::tr("Open logs directory")).clicked() {
                let log_dir = crate::get_filesystem_layout().log_dir;
                ui.ctx().open_url(OpenUrl::same_tab(format!(
                    "file://{}",
                    log_dir.to_string_lossy()
                )));
            }
            if ui.button(crate::i18n::tr("Clear all")).clicked() {
                self.entries.clear();
            }
        });

        ScrollArea::both()
            .stick_to_bottom(true)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                Grid::new(0)
                    .spacing((10.0, 2.0))
                    .num_columns(3)
                    .striped(true)
                    .show(ui, |ui| {
                        for entry in &self.entries {
                            ui.colored_label(
                                entry.color,
                                RichText::new(&entry.timestamp).size(12.0),
                            );
                            ui.colored_label(entry.color, RichText::new(&entry.ty).size(12.0));
                            ui.colored_label(entry.color, RichText::new(&entry.message).size(12.0));

                            ui.end_row();
                        }
                    });
            });
    }
}
