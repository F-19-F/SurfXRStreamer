//! Offline view of the production settings controls. Never starts DataSources or a runtime.
use super::{
    ServerRequest, appearance,
    components::{DevicesTab, NotificationBar, SettingsTab},
};
use surfxr_common::{LogEntry, LogSeverity};
use surfxr_server_io::ServerSessionManager;
use eframe::egui::{self, CentralPanel, Frame, Panel, RichText};
#[path = "preview_capture.rs"]
mod preview_capture;

struct Preview {
    settings: SettingsTab,
    devices: DevicesTab,
    show_devices: bool,
    show_about: bool,
    about: super::components::AboutTab,
    show_installation: bool,
    installation: super::components::InstallationTab,
    notifications: NotificationBar,
    session: ServerSessionManager,
    last_change: String,
    capture: Option<preview_capture::Capture>,
}

impl Preview {
    fn new(context: &eframe::CreationContext<'_>) -> Self {
        appearance::apply(&context.egui_ctx);
        if std::env::args().any(|arg| arg == "--language=zh-CN") {
            crate::i18n::set_language(crate::i18n::Language::SimplifiedChinese);
        }
        let session = ServerSessionManager::new(None);
        let mut settings = SettingsTab::new();
        settings.update_session(&session.session().session_settings);
        let mut devices = DevicesTab::new();
        devices.update_client_list(session.session());
        let mut notifications = NotificationBar::new();
        notifications.update_settings(session.settings());
        Self {
            settings,
            devices,
            show_devices: false,
            show_about: false,
            about: Default::default(),
            show_installation: false,
            installation: super::components::InstallationTab::new(),
            notifications,
            session,
            last_change: "No changes".into(),
            capture: preview_capture::Capture::from_args(),
        }
    }
}

impl eframe::App for Preview {
    fn ui(&mut self, ui: &mut egui::Ui, _: &mut eframe::Frame) {
        if let Some(capture) = &mut self.capture {
            capture.update(ui.ctx());
            self.settings.preview_page(if capture.page == 14 { 11 } else { capture.page.min(10) });
            self.show_devices = capture.page == 11;
            self.show_about = capture.page == 12;
            self.show_installation = capture.page == 13;
            self.notifications.push_notification(
                LogEntry {
                    severity: LogSeverity::Warning,
                    content: "Preview: 中文设备 / 桌面串流 / 控制器 / 144 Hz".into(),
                },
                true,
            );
        }
        self.notifications.ui(ui);
        Panel::left("preview_navigation")
            .exact_size(184.0)
            .resizable(false)
            .frame(
                Frame::new()
                    .inner_margin(12)
                    .fill(egui::Color32::from_rgb(36, 36, 36)),
            )
            .show(ui, |ui| {
                ui.heading("SurfXR");
                crate::i18n::selector(ui);
                ui.label(RichText::new(crate::i18n::tr("Offline UI preview")).weak());
                ui.separator();
                ui.label(crate::i18n::tr("Production settings controls"));
                ui.label(crate::i18n::tr("Changes stay in memory."));
                if ui.button(crate::i18n::tr("Devices")).clicked() { self.show_devices = true; }
                if ui.button(crate::i18n::tr("Settings")).clicked() { self.show_devices = false; }
                if ui.button(crate::i18n::tr("Desktop settings")).clicked() {
                    self.show_devices = false;
                    self.settings.open_desktop();
                }
                if ui.button(crate::i18n::tr("Sample warning")).clicked() {
                    self.notifications.push_notification(
                        LogEntry {
                            severity: LogSeverity::Warning,
                            content: "Preview warning: 中文设备 / 桌面串流 / 控制器 / 144 Hz"
                                .into(),
                        },
                        true,
                    );
                }
                ui.separator();
                ui.label(crate::i18n::tr("Last applied change"));
                ui.label(crate::i18n::tr(&self.last_change));
            });
        CentralPanel::default()
            .frame(
                Frame::new()
                    .inner_margin(20)
                    .fill(surfxr_gui_common::theme::BG),
            )
            .show(ui, |ui| {
                let requests = if self.show_about {
                    self.about.ui(ui); vec![]
                } else if self.show_installation {
                    self.installation.ui(ui); vec![]
                } else {
                    ui.heading(crate::i18n::tr(if self.show_devices { "Devices" } else { "Settings" }));
                    if self.show_devices {
                        self.devices.ui_with_sessions(ui, false, &[crate::frame_desktop::sessions::Session {
                            id: [0; 32], hostname: "frame-preview".into(), ip: "192.0.2.10".parse().unwrap(),
                            system: true, mode: "Desktop streaming", disconnecting: false,
                        }])
                    } else { self.settings.ui(ui) }
                };
                for request in requests {
                    if let ServerRequest::SetSessionValues(values) = request {
                        self.last_change = values
                            .iter()
                            .map(|v| {
                                format!("{} = {}", surfxr_packets::path_to_string(&v.path), v.value)
                            })
                            .collect::<Vec<_>>()
                            .join("\n");
                        match self.session.set_session_values(values) {
                            Ok(()) => self
                                .settings
                                .update_session(&self.session.session().session_settings),
                            Err(error) => self.last_change = error.to_string(),
                        }
                    }
                }
            });
    }
}

pub fn run() {
    let size = if std::env::args().any(|arg| arg == "--compact") {
        (820.0, 560.0)
    } else {
        (1120.0, 760.0)
    };
    eframe::run_native(
        "SurfXR - Offline UI preview",
        eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default()
                .with_inner_size(size)
                .with_min_inner_size((820.0, 560.0)),
            ..Default::default()
        },
        Box::new(|context| Ok(Box::new(Preview::new(context)))),
    )
    .unwrap();
}
