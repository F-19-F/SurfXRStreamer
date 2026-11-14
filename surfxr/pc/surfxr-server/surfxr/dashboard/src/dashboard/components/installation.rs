use crate::dashboard::ServerRequest;
use surfxr_gui_common::theme;
use eframe::egui::{Frame, Grid, RichText, ScrollArea, Ui};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

const DRIVER_UPDATE_INTERVAL: Duration = Duration::from_secs(1);

pub enum InstallationTabRequest {
    OpenSetupWizard,
    ServerRequest(ServerRequest),
}

pub struct InstallationTab {
    frame: crate::frame_installer::FrameInstaller,
    drivers: Vec<PathBuf>,
    last_update_instant: Instant,
    drivers_loaded: bool,
    driver_status: (bool, bool),
    driver_check: Option<std::sync::mpsc::Receiver<(bool, bool)>>,
}

impl InstallationTab {
    pub fn new() -> Self {
        Self {
            frame: crate::frame_installer::FrameInstaller::new(),
            drivers: vec![],
            last_update_instant: Instant::now(),
            drivers_loaded: false,
            driver_status: (false, false),
            driver_check: None,
        }
    }

    pub fn poll(&mut self) -> Vec<ServerRequest> {
        if let Some(receiver) = &self.driver_check {
            if let Ok(status) = receiver.try_recv() {
                self.driver_status = status;
                self.drivers_loaded = true;
                self.driver_check = None;
            }
        }
        self.frame.poll()
    }
    pub fn busy(&self) -> bool { self.frame.busy() || self.driver_check.is_some() }
    pub fn update_session(&mut self, session: &surfxr_session::SessionConfig) { self.frame.update_session(session); }

    pub fn update_drivers(&mut self, list: Vec<PathBuf>) {
        if self.driver_check.is_none() {
            let root = crate::get_filesystem_layout().openvr_driver_root_dir;
            let paths = list.clone();
            let (send, receive) = std::sync::mpsc::channel();
            self.driver_check = Some(receive);
            // An offline third-party/network driver path must never block UI painting.
            std::thread::spawn(move || {
                let registered = paths.contains(&root) || paths.iter().any(|path| same_driver(path, &root));
                let binary = root.join(if cfg!(windows) { "bin/win64/driver_surfxr_server.dll" } else { "bin/linux64/driver_surfxr_server.so" });
                let _ = send.send((registered, binary.is_file()));
            });
        }
        self.drivers = list;
    }

    pub fn ui(&mut self, ui: &mut Ui) -> Vec<InstallationTabRequest> {
        let mut requests = vec![];

        let now = Instant::now();
        if now > self.last_update_instant + DRIVER_UPDATE_INTERVAL {
            requests.push(InstallationTabRequest::ServerRequest(
                ServerRequest::GetDriverList,
            ));

            self.last_update_instant = now;
        }

        ScrollArea::vertical().show(ui, |ui| {
        self.frame.ui(ui);
        ui.vertical_centered_justified(|ui| {
            if ui.button(crate::i18n::tr("Run setup wizard")).clicked() {
                requests.push(InstallationTabRequest::OpenSetupWizard);
            }
            ui.columns(2, |ui| {
                if ui[0].button(crate::i18n::tr("Add firewall rules")).clicked() {
                    requests.push(InstallationTabRequest::ServerRequest(
                        ServerRequest::AddFirewallRules,
                    ));
                }
                if ui[1].button(crate::i18n::tr("Remove firewall rules")).clicked() {
                    requests.push(InstallationTabRequest::ServerRequest(
                        ServerRequest::RemoveFirewallRules,
                    ));
                }
            });

            Frame::group(ui.style())
                .fill(theme::SECTION_BG)
                .inner_margin(theme::FRAME_PADDING)
                .show(ui, |ui| {
                    ui.vertical_centered_justified(|ui| {
                        ui.label(RichText::new(crate::i18n::tr("SurfXR SteamVR driver")).size(18.0));
                    });
                    let root = crate::get_filesystem_layout().openvr_driver_root_dir;
                    let (registered, available) = self.driver_status;
                    ui.label(crate::i18n::tr(if !self.drivers_loaded { "Checking driver registration..." }
                        else if registered && available { "Current Dashboard driver is registered and available." }
                        else if registered { "Current Dashboard driver is registered, but its binary is missing." }
                        else { "Current Dashboard driver is not registered. Register this installation below." }));
                    ui.label(root.display().to_string());
                    ui.collapsing(crate::i18n::tr("All registered SteamVR drivers"), |ui| {
                    Grid::new(0).num_columns(2).show(ui, |ui| {
                        for driver_path in &self.drivers {
                            if ui.button(crate::i18n::tr("Remove")).clicked() {
                                requests.push(InstallationTabRequest::ServerRequest(
                                    ServerRequest::UnregisterDriver(driver_path.clone()),
                                ));
                            }

                            ScrollArea::new([true, false])
                                .auto_shrink([false, false])
                                .id_salt(driver_path)
                                .show(ui, |ui| {
                                    ui.label(driver_path.to_string_lossy());
                                });
                            ui.end_row();
                        }
                    });
                    });

                    if ui.button(crate::i18n::tr("Register SURFXR driver")).clicked() {
                        requests.push(InstallationTabRequest::ServerRequest(
                            ServerRequest::RegisterSurfxrDriver,
                        ));
                    }
                });
        });

        ui.add_space(12.0);
        ui.heading(crate::i18n::tr("Desktop streaming service"));
        crate::desktop_service::ui(ui);
        });
        requests
    }
}

fn same_driver(left: &std::path::Path, right: &std::path::Path) -> bool {
    match (left.canonicalize(), right.canonicalize()) {
        (Ok(left), Ok(right)) => if cfg!(windows) {
            left.to_string_lossy().eq_ignore_ascii_case(&right.to_string_lossy())
        } else { left == right },
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use eframe::egui::{self, Event, Modifiers, PointerButton, RawInput, Rect, Pos2, epaint::Shape};
    #[test]
    fn other_drivers_are_hidden_until_the_secondary_list_is_opened() {
        crate::i18n::set_language(crate::i18n::Language::English);
        let context = egui::Context::default();
        let mut tab = InstallationTab::new();
        tab.drivers = vec![PathBuf::from("third-party-driver-fixture")];
        tab.drivers_loaded = true;
        let mut frame = |events| context.run_ui(RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, egui::vec2(1200.0, 2400.0))),
            events, ..Default::default()
        }, |ui| { egui::CentralPanel::default().show(ui, |ui| { tab.ui(ui); }); });
        frame(vec![]);
        let output = frame(vec![]);
        let texts: Vec<_> = output.shapes.iter().filter_map(|shape| match &shape.shape {
            Shape::Text(text) => Some(text), _ => None,
        }).collect();
        assert!(!texts.iter().any(|text| text.galley.text() == "third-party-driver-fixture"));
        let header = texts.iter().find(|text| text.galley.text() == "All registered SteamVR drivers").unwrap();
        let pos = header.pos + header.galley.size() / 2.0;
        for pressed in [true, false] {
            frame(vec![Event::PointerMoved(pos), Event::PointerButton {
                pos, button: PointerButton::Primary, pressed, modifiers: Modifiers::NONE,
            }]);
        }
        let mut found = false;
        for _ in 0..20 {
            found |= frame(vec![]).shapes.iter().any(|shape| matches!(&shape.shape,
                Shape::Text(text) if text.galley.text() == "third-party-driver-fixture"));
        }
        assert!(found);
    }
    #[test]
    fn registration_compares_the_actual_installation_directory() {
        let root = std::env::temp_dir().join(format!("surfxr-driver-path-{:032x}", rand::random::<u128>()));
        std::fs::create_dir_all(root.join("current")).unwrap();
        std::fs::create_dir(root.join("other")).unwrap();
        assert!(same_driver(&root.join("current"), &root.join("other/../current")));
        assert!(!same_driver(&root.join("current"), &root.join("other")));
        assert!(!same_driver(&root.join("current"), &root.join("missing")));
        for dir in ["current", "other"] { std::fs::remove_dir(root.join(dir)).unwrap(); }
        std::fs::remove_dir(root).unwrap();
    }
}
