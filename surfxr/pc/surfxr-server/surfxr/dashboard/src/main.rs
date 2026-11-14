// hide console window on Windows in release
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod dashboard;
mod i18n;
mod pairing;
#[cfg(windows)]
mod tray;
#[cfg(windows)]
mod desktop_startup;
#[cfg(windows)]
mod service_elevation;

#[cfg(not(target_arch = "wasm32"))]
mod data_sources;
#[cfg(target_arch = "wasm32")]
mod data_sources_wasm;
#[cfg(not(target_arch = "wasm32"))]
mod desktop_service;
#[cfg(not(target_arch = "wasm32"))]
mod frame_installer;
#[cfg(not(target_arch = "wasm32"))]
mod frame_desktop;
#[cfg(not(target_arch = "wasm32"))]
mod frame_start;
#[cfg(not(target_arch = "wasm32"))]
#[cfg(target_os = "linux")]
mod linux_checks;
#[cfg(not(target_arch = "wasm32"))]
mod logging_backend;
#[cfg(not(target_arch = "wasm32"))]
mod steamvr_launcher;

#[cfg(not(target_arch = "wasm32"))]
use data_sources::DataSources;
#[cfg(target_arch = "wasm32")]
use data_sources_wasm::DataSources;

use surfxr_filesystem as afs;
use dashboard::Dashboard;

fn get_filesystem_layout() -> afs::Layout {
    afs::filesystem_layout_from_dashboard_exe(&std::env::current_exe().unwrap()).unwrap()
}

#[cfg(not(target_arch = "wasm32"))]
fn main() {
    #[cfg(windows)]
    if std::env::args().any(|arg| arg == "--service-worker") {
        std::process::exit(if service_elevation::run().is_ok() { 0 } else { 1 });
    }
    #[cfg(windows)]
    if std::env::args().any(|arg| arg == "--steamvr-ui-probe") {
        std::process::exit(steamvr_launcher::ui_probe::run());
    }
    // Explicit maintenance entry reuses exactly the Dashboard lifecycle owner.
    // Probe mode above is read-only; this mode deliberately restarts the runtime.
    if std::env::args().any(|arg| arg == "--steamvr-restart") {
        let result = steamvr_launcher::LAUNCHER.lock().restart(
            std::env::args().any(|arg| arg == "--with-steam"))
            .and_then(|()| steamvr_launcher::wait_for_driver());
        if let Err(error) = &result { eprintln!("{error:#}"); }
        std::process::exit(if result.is_ok() { 0 } else { 1 });
    }
    // Must precede process cleanup, session loading, logging and service ownership.
    if std::env::args().any(|arg| arg == "--ui-preview") {
        dashboard::preview::run();
        return;
    }
    use surfxr_common::SURFXR_VERSION;
    #[cfg(not(windows))]
    use surfxr_common::info;
    use eframe::{
        NativeOptions,
        egui::{IconData, ViewportBuilder},
    };
    use ico::IconDir;
    use std::{env, fs};
    #[cfg(not(windows))]
    use std::ffi::OsStr;
    use std::{io::Cursor, sync::mpsc};

    #[cfg(windows)]
    i18n::load(&get_filesystem_layout().session().with_file_name("dashboard-ui.json"));
    #[cfg(windows)]
    let (_instance, tray) = match tray::Tray::start()
        .expect("Cannot acquire the SurfXR Dashboard instance guard") {
        tray::Startup::Existing => return,
        tray::Startup::New(instance, tray) => (instance, tray),
    };
    let show_panel = { #[cfg(windows)] { tray.is_none() || std::env::args().any(|a| a == "--show-settings") }
        #[cfg(not(windows))] { true } };

    let (server_events_sender, server_events_receiver) = mpsc::channel();
    let lifecycle = logging_backend::Lifecycle::start();
    logging_backend::init_logging(server_events_sender.clone());

    // Kill any other dashboard instance
    #[cfg(not(windows))]
    let self_path = std::env::current_exe().unwrap().canonicalize().unwrap();
    #[cfg(not(windows))]
    for proc in sysinfo::System::new_all().processes_by_name(OsStr::new(&afs::dashboard_fname())) {
        // According to implementation notes, on linux the returned path can be empty due to
        // privileges, so canonicalize can fail
        if let Some(other_path) = proc.exe().and_then(|path| path.canonicalize().ok())
            && other_path != self_path
        {
            info!(
                "Killing other dashboard process with path {}",
                other_path.display()
            );
            proc.kill();
        }
    }

    #[cfg(target_os = "linux")]
    linux_checks::audio_check();

    data_sources::clean_session();

    if data_sources::get_read_only_local_session()
        .settings()
        .extra
        .steamvr_launcher
        .open_close_steamvr_with_dashboard
    {
        steamvr_launcher::LAUNCHER.lock().launch_steamvr()
    }

    let ico = IconDir::read(Cursor::new(include_bytes!("../resources/dashboard.ico"))).unwrap();
    let image = ico.entries().first().unwrap().decode().unwrap();

    // Workaround for the steam deck
    if fs::read_to_string("/sys/devices/virtual/dmi/id/board_vendor")
        .map(|vendor| vendor.trim() == "Valve")
        .unwrap_or(false)
    {
        unsafe { env::set_var("WINIT_X11_SCALE_FACTOR", "1") };
    }

    let native_result = eframe::run_native(
        &format!("SurfXR (streamer v{})", *SURFXR_VERSION),
        NativeOptions {
            viewport: ViewportBuilder::default()
                .with_app_id("surfxr.dashboard")
                .with_visible(show_panel)
                .with_active(show_panel)
                .with_inner_size((1120.0, 760.0))
                .with_min_inner_size((820.0, 560.0))
                .with_icon(IconData {
                    rgba: image.rgba_data().to_owned(),
                    width: image.width(),
                    height: image.height(),
                }),
            centered: true,
            ..Default::default()
        },
        {
            Box::new(move |creation_context| {
                #[cfg(windows)]
                let mut tray = tray;
                #[cfg(windows)]
                if let Some(tray) = &mut tray {
                    tray.bind_panel(creation_context, show_panel)?;
                }
                let data_source = DataSources::new(
                    creation_context.egui_ctx.clone(),
                    server_events_sender,
                    server_events_receiver,
                );

                let dashboard = Dashboard::new(creation_context, data_source);
                #[cfg(windows)]
                let dashboard = dashboard.with_tray(tray, &creation_context.egui_ctx);
                Ok(Box::new(dashboard))
            })
        },
    );
    lifecycle.native_result(native_result.is_ok());
    #[cfg(windows)]
    service_elevation::shutdown();
    native_result.unwrap();
}

#[cfg(target_arch = "wasm32")]
fn main() {
    console_error_panic_hook::set_once();
    wasm_logger::init(wasm_logger::Config::default());

    wasm_bindgen_futures::spawn_local(async {
        eframe::WebRunner::new()
            .start("dashboard_canvas", eframe::WebOptions::default(), {
                Box::new(move |creation_context| {
                    let context = creation_context.egui_ctx.clone();
                    Box::new(Dashboard::new(creation_context, DataSources::new(context)))
                })
            })
            .await
            .ok();
    });
}
