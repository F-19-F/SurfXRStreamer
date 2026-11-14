mod appearance;
mod components;
mod exit_confirmation;
#[cfg(not(target_arch = "wasm32"))]
pub mod preview;

use self::components::{
    DevicesTab, LogsTab, NotificationBar, SettingsTab, SetupWizard, SetupWizardRequest,
};
use crate::{
    DataSources,
    dashboard::components::{StatisticsTab},
};
use surfxr_common::{
    LogEntry,
    parking_lot::{Condvar, Mutex},
};
use surfxr_events::EventType;
use surfxr_gui_common::theme;
use surfxr_packets::{ClientConnectionsAction, PathValuePair};
use surfxr_session::SessionConfig;
use eframe::egui::{
    self, Align, CentralPanel, Direction, Frame, Layout, Margin, Panel, RichText, Ui,
};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, path::PathBuf, sync::Arc};

#[derive(Serialize, Deserialize, Debug)]
pub enum ServerRequest {
    Log(LogEntry),
    GetSession,
    UpdateSession(Box<SessionConfig>),
    SetSessionValues(Vec<PathValuePair>),
    UpdateClientList {
        hostname: String,
        action: ClientConnectionsAction,
    },
    InsertIdr,
    StartRecording,
    StopRecording,
    AddFirewallRules,
    RemoveFirewallRules,
    GetDriverList,
    RegisterSurfxrDriver,
    UnregisterDriver(PathBuf),
    RestartSteamvr,
    ShutdownSteamvr,
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Tab {
    Devices,
    Statistics,
    Settings,
    #[cfg(not(target_arch = "wasm32"))]
    Installation,
    Logs,
    About,
}

pub struct Dashboard {
    data_sources: DataSources,
    #[cfg(windows)]
    tray: Option<crate::tray::Tray>,
    exiting: bool,
    exit_confirmation: exit_confirmation::ExitConfirmation,
    shutdown_requested: bool,
    just_opened: bool,
    server_restarting: Arc<Mutex<bool>>,
    server_restarting_condvar: Arc<Condvar>,
    selected_tab: Tab,
    tab_labels: BTreeMap<Tab, &'static str>,
    connections_tab: DevicesTab,
    statistics_tab: StatisticsTab,
    settings_tab: SettingsTab,
    #[cfg(not(target_arch = "wasm32"))]
    installation_tab: components::InstallationTab,
    logs_tab: LogsTab,
    about_tab: components::AboutTab,
    notification_bar: NotificationBar,
    setup_wizard: SetupWizard,
    setup_wizard_open: bool,
    session: Option<SessionConfig>,
}

impl Dashboard {
    pub fn new(creation_context: &eframe::CreationContext<'_>, data_sources: DataSources) -> Self {
        appearance::apply(&creation_context.egui_ctx);
        #[cfg(not(target_arch = "wasm32"))]
        crate::i18n::load(&crate::get_filesystem_layout().session().with_file_name("dashboard-ui.json"));

        data_sources.request(ServerRequest::GetSession);

        Self {
            data_sources,
            #[cfg(windows)]
            tray: None,
            exiting: false,
            exit_confirmation: Default::default(),
            shutdown_requested: false,
            just_opened: true,
            server_restarting: Arc::new(Mutex::new(false)),
            server_restarting_condvar: Arc::new(Condvar::new()),
            selected_tab: Tab::Settings,
            tab_labels: [
                (Tab::Devices, "Devices"),
                (Tab::Statistics, "Performance"),
                (Tab::Settings, "Settings"),
                #[cfg(not(target_arch = "wasm32"))]
                (Tab::Installation, "Installation"),
                (Tab::Logs, "Logs"),
                (Tab::About, "About"),
            ]
            .into_iter()
            .collect(),
            connections_tab: DevicesTab::new(),
            statistics_tab: StatisticsTab::new(),
            settings_tab: SettingsTab::new(),
            #[cfg(not(target_arch = "wasm32"))]
            installation_tab: components::InstallationTab::new(),
            logs_tab: LogsTab::new(),
            about_tab: Default::default(),
            notification_bar: NotificationBar::new(),
            setup_wizard: SetupWizard::new(),
            setup_wizard_open: false,
            session: None,
        }
    }

    #[cfg(windows)]
    pub fn with_tray(mut self, tray: Option<crate::tray::Tray>, ctx: &egui::Context) -> Self {
        if let Some(tray) = &tray { tray.attach(ctx); }
        self.tray = tray;
        self
    }

    fn poll_events(&mut self) {
        let mut requests = vec![];
        #[cfg(not(target_arch = "wasm32"))]
        requests.extend(self.installation_tab.poll());
        while let Some(event) = self.data_sources.poll_event() {
            self.logs_tab.push_event(event.inner.clone());

            match event.inner.event_type {
                EventType::Log(log_event) => {
                    self.notification_bar
                        .push_notification(log_event, event.from_dashboard);
                }
                EventType::GraphStatistics(graph_statistics) => self
                    .statistics_tab
                    .update_graph_statistics(graph_statistics),
                EventType::StatisticsSummary(statistics) => {
                    self.statistics_tab.update_statistics(statistics)
                }
                EventType::Session(session) => {
                    if self.session.as_ref().is_some_and(|old|
                        old.frame_settings_sync.revision > session.frame_settings_sync.revision) { continue; }
                    if let Some(hostname) = crate::pairing::observe(&session) {
                        requests.push(ServerRequest::UpdateClientList {
                            hostname, action: ClientConnectionsAction::Trust,
                        });
                    }
                    #[cfg(not(target_arch = "wasm32"))]
                    self.installation_tab.update_session(&session);
                    let settings = session.to_settings();

                    self.connections_tab.update_client_list(&session);
                    self.settings_tab.update_session(&session.session_settings);
                    self.settings_tab.prefer_pc_mouse=session.frame_settings_sync.prefer_pc_mouse;
                    self.logs_tab.update_settings(&settings);
                    self.notification_bar.update_settings(&settings);
                    if self.just_opened {
                        if settings.extra.open_setup_wizard {
                            self.setup_wizard_open = true;
                        }

                        self.just_opened = false;
                    }

                    self.session = Some(*session);
                }
                EventType::ServerRequestsSelfRestart => self.restart_steamvr(&mut requests),
                #[cfg(not(target_arch = "wasm32"))]
                EventType::DriversList(list) => self.installation_tab.update_drivers(list),
                EventType::Adb(_) | EventType::NewVersionFound { .. }
                | EventType::DebugGroup { .. }
                | EventType::Tracking(_)
                | EventType::Buttons(_)
                | EventType::Haptics(_) => (),
            }
        }

        for request in requests { self.data_sources.request(request); }
    }

    fn restart_steamvr(&self, requests: &mut Vec<ServerRequest>) {
        self.restart_steamvr_mode(requests, false);
    }

    fn restart_steamvr_mode(&self, _requests: &mut Vec<ServerRequest>, with_steam: bool) {
        let mut server_restarting_lock = self.server_restarting.lock();
        if *server_restarting_lock {
            return;
        }
        *server_restarting_lock = true;
        #[cfg(target_arch = "wasm32")]
        _requests.push(ServerRequest::RestartSteamvr);

        #[cfg(not(target_arch = "wasm32"))]
        std::thread::spawn({
            let server_restarting = Arc::clone(&self.server_restarting);
            let condvar = Arc::clone(&self.server_restarting_condvar);
            move || {
                if with_steam { crate::steamvr_launcher::LAUNCHER.lock().restart_with_steam(); }
                else { crate::steamvr_launcher::LAUNCHER.lock().restart_steamvr(); }

                *server_restarting.lock() = false;
                condvar.notify_one();
            }
        });
    }
}

impl eframe::App for Dashboard {
    fn logic(&mut self, ctx: &egui::Context, _: &mut eframe::Frame) {
        self.poll_events();
        #[cfg(not(target_arch = "wasm32"))]
        if self.installation_tab.busy() { ctx.request_repaint_after(std::time::Duration::from_millis(250)); }
        #[cfg(windows)]
        if let Some(tray) = &mut self.tray {
            self.exiting |= tray.update(ctx, self.exiting);
        }
        let resident = { #[cfg(windows)] { self.tray.is_some() }
            #[cfg(not(windows))] { false } };
        if self.exiting || (!resident && ctx.input(|i| i.viewport().close_requested())) {
            if !self.shutdown_requested {
                self.shutdown_requested = true;
                crate::frame_desktop::sessions::shutdown();
                if self.session.as_ref().is_some_and(|s| s.to_settings().extra
                    .steamvr_launcher.open_close_steamvr_with_dashboard) {
                    self.data_sources.request(ServerRequest::ShutdownSteamvr);
                    crate::steamvr_launcher::LAUNCHER.lock().ensure_steamvr_shutdown().ok();
                }
            }
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
    }

    fn ui(&mut self, ui: &mut Ui, _: &mut eframe::Frame) {
        let mut requests = vec![];

        let connected_to_server = self.data_sources.server_connected();

        let server_restarting = *self.server_restarting.lock();

        self.notification_bar.ui(ui);

        if self.setup_wizard_open {
            CentralPanel::default().show(ui, |ui| {
                if let Some(request) = self.setup_wizard.ui(ui) {
                    match request {
                        SetupWizardRequest::ServerRequest(request) => {
                            requests.push(request);
                        }
                        SetupWizardRequest::Close { finished } => {
                            if finished {
                                requests.push(ServerRequest::SetSessionValues(vec![
                                    PathValuePair {
                                        path: surfxr_packets::parse_path(
                                            "session_settings.extra.open_setup_wizard",
                                        )
                                        .unwrap(),
                                        value: serde_json::Value::Bool(false),
                                    },
                                ]))
                            }

                            self.setup_wizard_open = false;
                        }
                    }
                }
            });
        } else {
            Panel::left("side_panel")
                .resizable(false)
                .frame(
                    Frame::new()
                        .fill(theme::LIGHTER_BG)
                        .inner_margin(Margin::same(7)),
                )
                .exact_size(184.0)
                .show(ui, |ui| {
                    ui.with_layout(Layout::top_down_justified(Align::Center), |ui| {
                        ui.add_space(13.0);
                        ui.heading(RichText::new(crate::i18n::tr("SurfXR")).size(24.0).strong());
                        ui.label(RichText::new(crate::i18n::tr("Desktop & VR streaming")).weak().size(12.0));
                        ui.add_space(18.0);
                        crate::i18n::settings_selector(ui);
                        egui::warn_if_debug_build(ui);
                    });

                    ui.with_layout(Layout::top_down_justified(Align::Min), |ui| {
                        for (tab, label) in &self.tab_labels {
                            ui.selectable_value(&mut self.selected_tab, *tab, crate::i18n::tr(label));
                        }
                    });

                    #[cfg(not(target_arch = "wasm32"))]
                    ui.with_layout(
                        Layout::bottom_up(Align::Center).with_cross_justify(true),
                        |ui| {
                            ui.add_space(5.0);
                            if exit_confirmation::warning_button(ui, "Exit completely").clicked() {
                                self.exit_confirmation.request(ui.ctx());
                            }

                            if server_restarting {
                                ui.add_enabled(false, egui::Button::new(crate::i18n::tr("Restart SteamVR")));
                            } else if connected_to_server {
                                if ui.button(crate::i18n::tr("Restart SteamVR")).clicked() {
                                    self.restart_steamvr(&mut requests);
                                }
                            } else if ui.button(crate::i18n::tr("Launch SteamVR")).clicked() {
                                crate::steamvr_launcher::LAUNCHER.lock().launch_steamvr();
                            }

                            #[cfg(windows)]
                            {
                                crate::steamvr_launcher::ui_health::poll();
                                if crate::steamvr_launcher::ui_health::missing() {
                                    ui.label(crate::i18n::tr("Steam VR menu unavailable"));
                                    if ui.add_enabled(!server_restarting, egui::Button::new(crate::i18n::tr("Restart with Steam"))).clicked() {
                                        self.restart_steamvr_mode(&mut requests, true);
                                    }
                                }
                            }

                            ui.horizontal(|ui| {
                                ui.add_space(4.0);
                                ui.label(RichText::new(crate::i18n::tr("SteamVR:")).size(13.0));
                                ui.add_space(-10.0);
                                ui.with_layout(
                                    Layout::centered_and_justified(Direction::LeftToRight),
                                    |ui| {
                                        ui.label(
                                            if server_restarting {
                                                RichText::new(crate::i18n::tr("SteamVR is restarting")).weak()
                                            } else if connected_to_server {
                                                RichText::new(crate::i18n::tr("Connected")).color(theme::OK_GREEN)
                                            } else {
                                                RichText::new(crate::i18n::tr("Not running")).weak()
                                            }
                                            .size(13.0),
                                        )
                                    },
                                );
                            })
                        },
                    )
                });

            CentralPanel::default()
                .frame(Frame::new().inner_margin(Margin::same(20)).fill(theme::BG))
                .show(ui, |ui| {
                    ui.with_layout(Layout::top_down_justified(Align::LEFT), |ui| {
                        ui.heading(RichText::new(crate::i18n::tr(self.tab_labels[&self.selected_tab])).size(25.0));
                        match self.selected_tab {
                            Tab::Devices => {
                                requests.extend(self.connections_tab.ui(ui, connected_to_server));
                            }
                            Tab::Statistics => {
                                if let Some(request) = self.statistics_tab.ui(ui) {
                                    requests.push(request);
                                }
                            }
                            Tab::Settings => {
                                requests.extend(self.settings_tab.ui(ui));
                            }
                            #[cfg(not(target_arch = "wasm32"))]
                            Tab::Installation => {
                                for request in self.installation_tab.ui(ui) {
                                    match request {
                                        components::InstallationTabRequest::OpenSetupWizard => {
                                            self.setup_wizard_open = true
                                        }
                                        components::InstallationTabRequest::ServerRequest(
                                            request,
                                        ) => {
                                            requests.push(request);
                                        }
                                    }
                                }
                            }
                            Tab::Logs => self.logs_tab.ui(ui),
                            Tab::About => self.about_tab.ui(ui),
                        }
                    })
                });
        }

        for request in requests {
            self.data_sources.request(request);
        }
        let close_steamvr = self.session.as_ref().is_some_and(|s| s.to_settings()
            .extra.steamvr_launcher.open_close_steamvr_with_dashboard);
        self.exiting |= self.exit_confirmation.ui(ui.ctx(), close_steamvr);
    }
}
