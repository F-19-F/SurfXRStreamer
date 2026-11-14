use surfxr_common::{LogEntry, LogSeverity};
use surfxr_gui_common::theme::{self, log_colors};
use surfxr_session::Settings;
use eframe::egui::{self, Frame, Label, Layout, Panel, RichText, TextWrapMode, Ui};
#[cfg(target_arch = "wasm32")]
use instant::Instant;
#[cfg(not(target_arch = "wasm32"))]
use std::time::Instant;
use std::{collections::VecDeque, time::Duration};

const TIMEOUT: Duration = Duration::from_secs(8);
const TIP: &str = "SurfXR features contains device identities, desktop streaming and SteamVR startup. Advanced keeps detailed SURFXR tuning.";

pub struct NotificationBar {
    message: Option<LogEntry>,
    receive_instant: Instant,
    min_notification_level: LogSeverity,
    show_tip: bool,
    tip_dismissed: bool,
    dismissed: VecDeque<(LogSeverity, String)>,
    expanded: bool,
}

impl NotificationBar {
    pub fn new() -> Self {
        Self {
            message: None,
            receive_instant: Instant::now(),
            min_notification_level: LogSeverity::Debug,
            show_tip: false,
            tip_dismissed: false,
            dismissed: VecDeque::new(),
            expanded: false,
        }
    }

    pub fn update_settings(&mut self, settings: &Settings) {
        self.min_notification_level = settings.extra.logging.notification_level;
        if !settings.extra.logging.show_notification_tip {
            self.tip_dismissed = false;
        }
        self.show_tip = settings.extra.logging.show_notification_tip;
    }

    pub fn push_notification(&mut self, event: LogEntry, from_dashboard: bool) {
        let minimum = if from_dashboard {
            if cfg!(debug_assertions) {
                LogSeverity::Debug
            } else {
                LogSeverity::Info
            }
        } else {
            self.min_notification_level
        };
        if event.severity < minimum
            || self
                .dismissed
                .iter()
                .any(|(severity, text)| *severity == event.severity && *text == event.content)
        {
            return;
        }
        if self.receive_instant.elapsed() >= TIMEOUT
            || self
                .message
                .as_ref()
                .is_none_or(|old| event.severity >= old.severity)
        {
            self.message = Some(event);
            self.receive_instant = Instant::now();
            self.expanded = false;
        }
    }

    fn dismiss(&mut self) {
        if let Some(event) = self.message.take() {
            if self.dismissed.len() == 64 {
                self.dismissed.pop_front();
            }
            self.dismissed.push_back((event.severity, event.content));
        } else {
            self.tip_dismissed = true;
        }
        self.expanded = false;
    }

    pub fn ui(&mut self, ui: &mut Ui) {
        if self.message.is_some() && self.receive_instant.elapsed() >= TIMEOUT {
            self.message = None;
            self.expanded = false;
        }
        let (label, text, color) = if let Some(event) = &self.message {
            let (label, color) = match event.severity {
                LogSeverity::Error => ("Error", log_colors::ERROR_LIGHT),
                LogSeverity::Warning => ("Warning", log_colors::WARNING_LIGHT),
                LogSeverity::Info => ("Info", log_colors::INFO_LIGHT),
                LogSeverity::Debug => ("Details", theme::FG),
            };
            ui.ctx()
                .request_repaint_after(TIMEOUT.saturating_sub(self.receive_instant.elapsed()));
            (label, crate::i18n::notification(&event.content), color)
        } else if self.show_tip && !self.tip_dismissed {
            ("Tip", crate::i18n::tr(TIP), log_colors::INFO_LIGHT)
        } else {
            return;
        };
        let expanded = self.expanded;
        Panel::bottom("notifications")
            .exact_size(if expanded { 160.0 } else { 44.0 })
            .resizable(false)
            .frame(Frame::new().inner_margin(egui::vec2(14.0, 8.0)).fill(theme::LIGHTER_BG))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.label(RichText::new(crate::i18n::tr(label)).color(color).strong());
                    ui.with_layout(Layout::right_to_left(egui::Align::Center), |ui| {
                        if ui.small_button(crate::i18n::tr("Dismiss"))
                            .on_hover_text(crate::i18n::tr("Hide this message for this Dashboard session. New messages still appear; all events remain in Logs."))
                            .clicked() { self.dismiss(); }
                        if ui.small_button(crate::i18n::tr(if expanded { "Less" } else { "Details" })).clicked() {
                            self.expanded = !expanded;
                        }
                        if !expanded { ui.add(Label::new(&text).wrap_mode(TextWrapMode::Truncate)); }
                    });
                });
                if expanded {
                    egui::ScrollArea::vertical().max_height(108.0).show(ui, |ui| { ui.label(&text); });
                }
            });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dismissed_warning_does_not_return_but_new_errors_do() {
        let mut bar = NotificationBar::new();
        let warning = || LogEntry {
            severity: LogSeverity::Warning,
            content: "Repeated warning".into(),
        };
        bar.push_notification(warning(), true);
        assert!(bar.message.is_some());
        bar.dismiss();
        bar.push_notification(warning(), true);
        assert!(bar.message.is_none());
        bar.push_notification(
            LogEntry {
                severity: LogSeverity::Error,
                content: "New error".into(),
            },
            true,
        );
        assert!(bar.message.is_some());
    }

    #[test]
    fn dismiss_button_stays_visible_and_can_be_clicked() {
        crate::i18n::set_language(crate::i18n::Language::English);
        let context = egui::Context::default();
        let mut bar = NotificationBar::new();
        bar.push_notification(
            LogEntry {
                severity: LogSeverity::Warning,
                content: "A long warning ".repeat(80),
            },
            true,
        );
        let screen = egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(820.0, 560.0));
        let mut draw = |events| {
            context.run_ui(
                egui::RawInput {
                    screen_rect: Some(screen),
                    events,
                    ..Default::default()
                },
                |ui| {
                    bar.ui(ui);
                    egui::CentralPanel::default().show(ui, |_| {});
                },
            )
        };
        for _ in 0..3 {
            draw(vec![]);
        }
        let output = draw(vec![]);
        let pos = output
            .shapes
            .iter()
            .find_map(|shape| {
                if let egui::epaint::Shape::Text(text) = &shape.shape {
                    if text.galley.text() == "Dismiss" {
                        let rect = egui::Rect::from_min_size(text.pos, text.galley.size());
                        assert!(screen.contains_rect(rect));
                        assert!(shape.clip_rect.contains_rect(rect));
                        return Some(rect.center());
                    }
                }
                None
            })
            .expect("Visible dismissal action");
        draw(vec![
            egui::Event::PointerMoved(pos),
            egui::Event::PointerButton {
                pos,
                button: egui::PointerButton::Primary,
                pressed: true,
                modifiers: egui::Modifiers::NONE,
            },
        ]);
        draw(vec![egui::Event::PointerButton {
            pos,
            button: egui::PointerButton::Primary,
            pressed: false,
            modifiers: egui::Modifiers::NONE,
        }]);
        assert!(bar.message.is_none());
    }
}
