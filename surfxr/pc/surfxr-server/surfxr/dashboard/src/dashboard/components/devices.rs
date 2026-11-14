use crate::dashboard::ServerRequest;
use surfxr_common::ConnectionState;
use surfxr_gui_common::theme::{self, log_colors};
use surfxr_packets::ClientConnectionsAction;
use surfxr_session::{ClientConnectionConfig, SessionConfig};
use eframe::{
    egui::{self, Frame, Grid, Layout, TextEdit, Ui, Window},
    emath::{Align, Align2},
    epaint::Color32,
};

struct EditPopupState {
    new_devices: bool,
    hostname: String,
    ips: Vec<String>,
}

pub struct DevicesTab {
    new_devices: Option<Vec<(String, ClientConnectionConfig)>>,
    trusted_devices: Option<Vec<(String, ClientConnectionConfig)>>,
    edit_popup_state: Option<EditPopupState>,
    startup_hint_dismissed: bool,
    persistent_trust: bool,
}

impl DevicesTab {
    pub fn new() -> Self {
        Self {
            new_devices: None,
            trusted_devices: None,
            edit_popup_state: None,
            startup_hint_dismissed: false,
            persistent_trust: false,
        }
    }

    pub fn update_client_list(&mut self, session: &SessionConfig) {
        let discovery = &session.session_settings.connection.client_discovery;
        self.persistent_trust = discovery.enabled && discovery.content.auto_trust_clients;
        let (trusted_clients, untrusted_clients) =
            session
                .client_connections
                .clone()
                .into_iter()
                .partition::<Vec<_>, _>(|(_, data)| data.trusted);

        self.trusted_devices = Some(trusted_clients);
        self.new_devices = Some(untrusted_clients);
    }

    pub fn ui(&mut self, ui: &mut Ui, connected_to_server: bool) -> Vec<ServerRequest> {
        let desktop_sessions = crate::frame_desktop::sessions::snapshot();
        self.ui_with_sessions(ui, connected_to_server, &desktop_sessions)
    }

    pub fn ui_with_sessions(&mut self, ui: &mut Ui, connected_to_server: bool,
        desktop_sessions: &[crate::frame_desktop::sessions::Session]) -> Vec<ServerRequest> {
        let mut requests = vec![];
        ui.ctx().request_repaint_after(std::time::Duration::from_millis(500));
        desktop_sessions_section(ui, desktop_sessions);

        if self.new_devices.is_none() {
            requests.push(ServerRequest::GetSession);
        }

        if connected_to_server {
            self.startup_hint_dismissed = false;
        }
        if !connected_to_server && desktop_sessions.is_empty() && !self.startup_hint_dismissed {
            Frame::group(ui.style()).inner_margin(theme::FRAME_PADDING).show(ui, |ui| {
                ui.horizontal_wrapped(|ui| {
                    ui.vertical(|ui| {
                        ui.strong(crate::i18n::tr("SteamVR is not running"));
                        ui.label(crate::i18n::tr("Desktop streaming uses its own service. Check Installation for host status."));
                        ui.label(crate::i18n::tr("For VR, connect from the headset or choose Launch SteamVR."));
                    });
                    if ui.button(crate::i18n::tr("Dismiss")).clicked() { self.startup_hint_dismissed = true; }
                });
            });
        }

        ui.vertical_centered_justified(|ui| {
            if let Some(clients) = &self.new_devices
                && let Some(request) = new_clients_section(ui, clients, self.persistent_trust)
            {
                requests.push(request);
            }

            ui.add_space(theme::FRAME_PADDING);

            if let Some(clients) = &mut self.trusted_devices
                && let Some(request) = trusted_clients_section(
                    ui,
                    clients
                        .iter()
                                                .collect::<Vec<_>>()
                        .as_slice(),
                    &mut self.edit_popup_state,
                    desktop_sessions,
                )
            {
                requests.push(request);
            }
        });

        if let Some(mut state) = self.edit_popup_state.take() {
            Window::new(crate::i18n::tr("Edit connection"))
                .anchor(Align2::CENTER_CENTER, (0.0, 0.0))
                .resizable(false)
                .collapsible(false)
                .show(ui.ctx(), |ui| {
                    ui.add_space(5.0);

                    ui.columns(2, |ui| {
                        ui[0].horizontal(|ui| {
                            ui.add_space(5.0);
                            ui.label(crate::i18n::tr("Hostname:"));
                        });
                        ui[1].add_enabled(
                            state.new_devices,
                            TextEdit::singleline(&mut state.hostname),
                        );

                        ui[0].horizontal(|ui| {
                            ui.add_space(5.0);
                            ui.label(crate::i18n::tr("IP Addresses:"));
                        });
                        for address in &mut state.ips {
                            ui[1].text_edit_singleline(address);
                        }
                        if ui[1].button(crate::i18n::tr("Add new")).clicked() {
                            state.ips.push("192.168.X.X".into());
                        }
                    });

                    ui.columns(2, |ui| {
                        if ui[0].button(crate::i18n::tr("Cancel")).clicked() {
                            return;
                        }

                        if ui[1].button(crate::i18n::tr("Save")).clicked() {
                            let manual_ips =
                                state.ips.iter().filter_map(|s| s.parse().ok()).collect();

                            if state.new_devices {
                                requests.push(ServerRequest::UpdateClientList {
                                    hostname: state.hostname,
                                    action: ClientConnectionsAction::AddIfMissing {
                                        trusted: true,
                                        manual_ips,
                                    },
                                });
                            } else {
                                requests.push(ServerRequest::UpdateClientList {
                                    hostname: state.hostname,
                                    action: ClientConnectionsAction::SetManualIps(manual_ips),
                                });
                            }
                        } else {
                            self.edit_popup_state = Some(state);
                        }
                    })
                });
        }

        requests
    }
}

fn new_clients_section(
    ui: &mut Ui,
    clients: &[(String, ClientConnectionConfig)],
    persistent_trust: bool,
) -> Option<ServerRequest> {
    let mut request = None;

    Frame::group(ui.style())
        .inner_margin(theme::FRAME_PADDING)
        .fill(theme::SECTION_BG)
        .show(ui, |ui| {
            ui.vertical_centered_justified(|ui| {
                ui.horizontal(|ui| {
                    ui.add_space(theme::FRAME_TEXT_SPACING);
                    ui.heading(crate::i18n::tr("New Wireless Devices"));

                    // Extend to the right
                    ui.with_layout(Layout::right_to_left(Align::Center), |_| ());
                });
            });
            ui.with_layout(Layout::top_down(Align::Min), |ui| {
                let mut pairing = crate::pairing::enabled();
                if pairing { ui.ctx().request_repaint_after(std::time::Duration::from_millis(250)); }
                if ui.add_enabled(!persistent_trust || pairing, egui::Checkbox::new(
                    &mut pairing, crate::i18n::tr("One-time pairing"))).changed() {
                    crate::pairing::set_enabled(pairing);
                    if pairing { request = Some(ServerRequest::GetSession); }
                }
                ui.add(egui::Label::new(crate::i18n::tr(if persistent_trust {
                    "Turn off Auto trust clients in Advanced > Connection to use one-time pairing."
                } else {
                    "Trust the next new device during this Dashboard session. Turns off after it connects. Paired devices stay trusted."
                })).wrap());
                ui.add_space(8.0);
            });
            for (hostname, _) in clients {
                Frame::group(ui.style())
                    .fill(theme::DARKER_BG)
                    .inner_margin(egui::vec2(15.0, 12.0))
                    .show(ui, |ui| {
                        Grid::new(format!("{hostname}-new-clients"))
                            .num_columns(2)
                            .spacing(egui::vec2(8.0, 8.0))
                            .show(ui, |ui| {
                                ui.horizontal(|ui| {
                                    ui.label(hostname);
                                });
                                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                    if ui.button(crate::i18n::tr("Trust")).clicked() {
                                        request = Some(ServerRequest::UpdateClientList {
                                            hostname: hostname.clone(),
                                            action: ClientConnectionsAction::Trust,
                                        });
                                    };
                                });
                                ui.end_row();
                            });
                    });
            }
        });

    request
}

fn trusted_clients_section(
    ui: &mut Ui,
    clients: &[&(String, ClientConnectionConfig)],
    edit_popup_state: &mut Option<EditPopupState>,
    desktop_sessions: &[crate::frame_desktop::sessions::Session],
) -> Option<ServerRequest> {
    let mut request = None;

    Frame::group(ui.style())
        .fill(theme::SECTION_BG)
        .inner_margin(theme::FRAME_PADDING)
        .show(ui, |ui| {
            Grid::new(0).num_columns(2).show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.add_space(theme::FRAME_TEXT_SPACING);
                    ui.heading(crate::i18n::tr("Trusted Wireless Devices"));
                });

                ui.with_layout(Layout::right_to_left(eframe::emath::Align::Center), |ui| {
                    if ui.button(crate::i18n::tr("Add device manually")).clicked() {
                        *edit_popup_state = Some(EditPopupState {
                            hostname: "XXXX.client.local.".into(),
                            new_devices: true,
                            ips: Vec::new(),
                        });
                    }
                });
            });

            for (hostname, data) in clients {
                let desktop = desktop_sessions.iter().find(|s| &s.hostname == hostname);
                Frame::group(ui.style())
                    .fill(theme::DARKER_BG)
                    .inner_margin(egui::vec2(15.0, 12.0))
                    .show(ui, |ui| {
                        Grid::new(format!("{hostname}-clients"))
                            .num_columns(2)
                            .spacing(egui::vec2(8.0, 8.0))
                            .show(ui, |ui| {
                                ui.label(&data.display_name);
                                ui.horizontal(|ui| {
                                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                        if let Some(session) = desktop {
                                            desktop_connection_label(ui, session);
                                        } else { connection_label(ui, &data.connection_state); }
                                    });
                                });

                                ui.end_row();

                                ui.label(format!(
                                    "{hostname}: {}",
                                    desktop.map(|s| s.ip).or(data.current_ip)
                                        .map_or_else(|| crate::i18n::tr("Unknown IP"), |ip| ip.to_string()),
                                ));
                                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                    if ui.button(crate::i18n::tr("Remove")).clicked() {
                                        for session in desktop_sessions.iter().filter(|s| &s.hostname == hostname) {
                                            crate::frame_desktop::sessions::disconnect(&session.id);
                                        }
                                        request = Some(ServerRequest::UpdateClientList {
                                            hostname: hostname.clone(),
                                            action: ClientConnectionsAction::RemoveEntry,
                                        });
                                    }
                                    if ui.button(crate::i18n::tr("Edit")).clicked() {
                                        *edit_popup_state = Some(EditPopupState {
                                            new_devices: false,
                                            hostname: hostname.to_owned(),
                                            ips: data
                                                .manual_ips
                                                .iter()
                                                .map(|addr| addr.to_string())
                                                .collect::<Vec<String>>(),
                                        });
                                    }
                                });
                            });
                    });
            }
        });

    request
}

fn connection_label(ui: &mut Ui, connection_state: &ConnectionState) {
    match connection_state {
        ConnectionState::Disconnected => ui.colored_label(Color32::GRAY, crate::i18n::tr("Disconnected")),
        ConnectionState::Connecting => ui.colored_label(log_colors::WARNING_LIGHT, crate::i18n::tr("Connecting")),
        ConnectionState::Connected => ui.colored_label(theme::OK_GREEN, crate::i18n::tr("Connected")),
        ConnectionState::Streaming => ui.colored_label(theme::OK_GREEN, crate::i18n::tr("Streaming")),
        ConnectionState::Disconnecting => {
            ui.colored_label(log_colors::WARNING_LIGHT, crate::i18n::tr("Disconnecting"))
        }
    };
}

fn desktop_connection_label(ui: &mut Ui, session: &crate::frame_desktop::sessions::Session) {
    ui.colored_label(if session.disconnecting { log_colors::WARNING_LIGHT } else { theme::OK_GREEN },
        crate::i18n::tr(if session.disconnecting { "Disconnecting" } else { session.mode }));
}

fn desktop_sessions_section(ui: &mut Ui, sessions: &[crate::frame_desktop::sessions::Session]) {
    Frame::group(ui.style()).inner_margin(theme::FRAME_PADDING).show(ui, |ui| {
        ui.heading(crate::i18n::tr("Desktop / unified connections"));
        if sessions.is_empty() {
            ui.label(crate::i18n::tr("No desktop or unified device connected"));
            ui.label(crate::i18n::tr(&crate::frame_desktop::status().message));
        }
        for session in sessions {
            ui.push_id(session.id, |ui| {
                ui.horizontal_wrapped(|ui| {
                    ui.strong(&session.hostname);
                    ui.label(session.ip.to_string());
                    desktop_connection_label(ui, session);
                    ui.label(crate::i18n::tr(if session.system { "SYSTEM permissions" } else { "User permissions" }));
                    if ui.add_enabled(!session.disconnecting,
                        egui::Button::new(crate::i18n::tr("Disconnect session"))).clicked() {
                        crate::frame_desktop::sessions::disconnect(&session.id);
                    }
                });
            });
        }
        ui.small(crate::i18n::tr("Disconnect ends this connection and keeps pairing. Reconnect from the headset when needed."));
        ui.small(crate::i18n::tr("Closing the window keeps streaming in the tray. Fully exiting disconnects all sessions."));
    });
    ui.add_space(theme::FRAME_PADDING);
}

#[cfg(test)]
#[path = "devices_tests.rs"]
mod tests;
