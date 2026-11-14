//! Native desktop host ownership and explicit elevation controls.
use std::{
    path::PathBuf,
    process::{Child, Command},
    sync::{Mutex, atomic::{AtomicBool, Ordering}},
};
static BUSY: AtomicBool = AtomicBool::new(false);
static ERROR: Mutex<Option<String>> = Mutex::new(None);

#[derive(Clone, Default)]
pub struct ServiceStatus {
    pub installed: bool,
    pub running: bool,
}
fn executable() -> Option<PathBuf> {
    let folder = std::env::current_exe()
        .ok()?
        .parent()?.to_owned();
    let daemon=folder.join("surfxr-daemon.exe");
    let path=if daemon.is_file(){daemon}else{folder.join("surfxr-desktop.exe")};
    path.is_file().then_some(path)
}
pub fn start_user_host() -> Option<Child> {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        if executable()?.file_name().is_some_and(|name|name=="surfxr-daemon.exe"){return None;}
        Command::new(std::env::current_exe().ok()?.parent()?.join("surfxr-desktop.exe"))
            .args(["--host", "--dashboard-managed"])
            .creation_flags(0x08000000)
            .spawn()
            .ok()
    }
    #[cfg(not(windows))]
    {
        None
    }
}
pub fn action(action: &str) {
    if BUSY.swap(true, Ordering::AcqRel) { return; }
    *ERROR.lock().unwrap() = None;
    let action = action.to_owned();
    std::thread::spawn(move || {
        #[cfg(windows)]
        if matches!(action.as_str(), "automatic" | "manual") {
            if let Err(error) = crate::desktop_startup::set_automatic(action == "automatic") {
                surfxr_common::warn!("Desktop service action did not complete: {error}");
                *ERROR.lock().unwrap() = Some(error);
            }
            BUSY.store(false, Ordering::Release);
            return;
        }
        #[cfg(windows)]
        let result = crate::service_elevation::action(&action);
        #[cfg(not(windows))]
        let result: Result<(), String> = Err("Service management requires Windows".into());
        if let Err(error) = result {
            surfxr_common::warn!("Desktop service action did not complete: {error}");
            *ERROR.lock().unwrap() = Some(error);
        } else if action == "uninstall" {
            #[cfg(windows)]
            if let Err(error) = crate::desktop_startup::set_login(false) {
                *ERROR.lock().unwrap() = Some(error);
            }
        }
        BUSY.store(false, Ordering::Release);
    });
}
pub fn status() -> ServiceStatus {
    #[cfg(windows)]
    {
        use std::{ffi::c_void, ptr};
        #[repr(C)]
        struct ServiceState {
            ty: u32,
            state: u32,
            controls: u32,
            win32: u32,
            specific: u32,
            checkpoint: u32,
            hint: u32,
        }
        #[link(name = "advapi32")]
        unsafe extern "system" {
            fn OpenSCManagerW(
                machine: *const u16,
                database: *const u16,
                access: u32,
            ) -> *mut c_void;
            fn OpenServiceW(manager: *mut c_void, name: *const u16, access: u32) -> *mut c_void;
            fn QueryServiceStatus(service: *mut c_void, status: *mut ServiceState) -> i32;
            fn CloseServiceHandle(service: *mut c_void) -> i32;
        }
        unsafe {
            let manager = OpenSCManagerW(ptr::null(), ptr::null(), 1);
            if manager.is_null() {
                return ServiceStatus::default();
            }
            let name: Vec<_> = "SurfXRDesktop\0".encode_utf16().collect();
            let service = OpenServiceW(manager, name.as_ptr(), 4);
            CloseServiceHandle(manager);
            if service.is_null() {
                return ServiceStatus::default();
            }
            let mut state = ServiceState {
                ty: 0,
                state: 0,
                controls: 0,
                win32: 0,
                specific: 0,
                checkpoint: 0,
                hint: 0,
            };
            let ok = QueryServiceStatus(service, &mut state) != 0;
            CloseServiceHandle(service);
            ServiceStatus {
                installed: true,
                running: ok && state.state == 4,
            }
        }
    }
    #[cfg(not(windows))]
    {
        ServiceStatus::default()
    }
}
pub fn ui(ui: &mut eframe::egui::Ui) {
    ui.ctx()
        .request_repaint_after(std::time::Duration::from_millis(500));
    let desktop = crate::frame_desktop::status();
    ui.label(crate::i18n::tr(&desktop.message));
    if desktop.active {
        ui.label(format!(
            "{}: {} | {}",
            crate::i18n::tr("Device"), desktop.hostname,
            crate::i18n::tr(if desktop.system { "SYSTEM permissions" } else { "User permissions" })
        ));
    }
    let service = status();
    ui.label(crate::i18n::tr(if service.running {
        "High privilege service: running"
    } else if service.installed {
        "High privilege service: stopped"
    } else {
        "High privilege service: not installed"
    }));
    ui.label(crate::i18n::tr("Desktop quality is in Quick setup > Picture & performance. Access and audio are in SurfXR features > Desktop streaming. Trust devices in Devices."));
    ui.label(crate::i18n::tr("Startup options are in Quick setup > Startup & diagnostics."));
    if let Some(error) = ERROR.lock().unwrap().as_ref() {
        ui.colored_label(eframe::egui::Color32::LIGHT_RED, crate::i18n::notification(error));
    }
    if BUSY.load(Ordering::Acquire) { ui.spinner(); }
    ui.add_enabled_ui(!BUSY.load(Ordering::Acquire), |ui| {
    ui.horizontal_wrapped(|ui| {
        if ui
            .add_enabled(
                !service.running,
                eframe::egui::Button::new(crate::i18n::tr("Install / update service")),
            )
            .clicked()
        {
            action("install");
        }
        if ui
            .add_enabled(
                service.installed && !service.running,
                eframe::egui::Button::new(crate::i18n::tr("Start service")),
            )
            .clicked()
        {
            action("start");
        }
        if ui
            .add_enabled(service.running, eframe::egui::Button::new(crate::i18n::tr("Stop service")))
            .clicked()
        {
            action("stop");
        }
        if ui
            .add_enabled(
                service.installed && !service.running,
                eframe::egui::Button::new(crate::i18n::tr("Uninstall service")),
            )
            .clicked()
        {
            action("uninstall");
        }
    });
    });
}

#[cfg(windows)]
pub fn startup_ui(ui: &mut eframe::egui::Ui) {
    ui.heading(crate::i18n::tr("Dashboard startup"));
    let mut login = crate::desktop_startup::login_enabled();
    if ui.add_enabled(!BUSY.load(Ordering::Acquire), eframe::egui::Checkbox::new(&mut login,
        crate::i18n::tr("Start Dashboard in the tray when I sign in"))).changed() {
        if let Err(error) = crate::desktop_startup::set_login(login) {
            *ERROR.lock().unwrap() = Some(error);
        }
    }
    let service = status();
    #[cfg(windows)]
    {
        let automatic = crate::desktop_startup::automatic();
        let login = crate::desktop_startup::login_enabled();
        let mut enabled = automatic == Some(true) && login;
        if ui.add_enabled(service.installed && automatic.is_some() && !BUSY.load(Ordering::Acquire),
            eframe::egui::Checkbox::new(&mut enabled, crate::i18n::tr("Start desktop streaming automatically at boot"))).changed() {
            action(if enabled { "automatic" } else { "manual" });
        }
        ui.label(crate::i18n::tr("Starts the service at boot and keeps Dashboard in the tray after you sign in. Requires administrator approval. Use Start service to start now."));
        if automatic == Some(true) && !login {
            ui.label(crate::i18n::tr("Service starts automatically, but Dashboard logon startup is missing. Enable the option to repair it."));
        }
    }
    if let Some(error) = ERROR.lock().unwrap().as_ref() {
        ui.colored_label(eframe::egui::Color32::LIGHT_RED, crate::i18n::notification(error));
    }
    if BUSY.load(Ordering::Acquire) { ui.spinner(); ui.ctx().request_repaint_after(std::time::Duration::from_millis(250)); }
    ui.separator();
}
