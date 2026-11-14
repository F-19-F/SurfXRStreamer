#[cfg(target_os = "linux")]
mod linux_steamvr;
mod runtime_probe;
#[cfg(windows)]
mod shutdown;
#[cfg(windows)]
pub(crate) mod ui_probe;
#[cfg(windows)]
pub(crate) mod ui_health;
#[cfg(windows)]
mod windows_steamvr;

use crate::data_sources;
use surfxr_common::{
    anyhow::{Context, Result},
    debug, error,
    glam::bool,
    parking_lot::Mutex,
    warn,
};
use surfxr_filesystem::{self as afs};
use serde_json::{self, json};
use std::{
    sync::atomic::{AtomicBool, Ordering},
    ffi::OsStr,
    fs,
    marker::PhantomData,
    process::Command,
    thread,
};
use sysinfo::System;
#[cfg(not(windows))]
use {std::time::{Duration, Instant}, sysinfo::ProcessesToUpdate};

static RESTARTING: AtomicBool = AtomicBool::new(false);

#[cfg(not(windows))]
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(10);

// Dropping a Child does not reap it, and a zombie's pid still passes
// kill(pid, 0), which vrmonitor's single-instance check runs on the last holder
// of its semaphore. An unreaped child blocks every later launch until this
// process exits.
pub(super) fn spawn_and_reap(command: &mut Command) -> Result<()> {
    let mut child = command.spawn()?;

    thread::spawn(move || {
        child.wait().ok();
    });

    Ok(())
}
const DRIVER_KEY: &str = "driver_surfxr_server";
const BLOCKED_KEY: &str = "blocked_by_safe_mode";

pub fn is_steamvr_running() -> bool {
    steamvr_identity().is_some()
}
pub(super) fn steamvr_identity()->Option<(u32,u64)>{
    System::new_all()
        .processes_by_name(OsStr::new(&afs::exec_fname("vrserver")))
        .next().map(|process|(process.pid().as_u32(),process.start_time()))
}
pub(crate) fn frame_runtime_evidence(port:u16)->crate::frame_start::RuntimeEvidence {
    crate::frame_start::RuntimeEvidence {
        // The matching SURFXR driver's web owner is the readiness proof. A PID
        // is only evidence that an in-progress launch exited, never readiness.
        driver_ready:runtime_probe::surfxr_driver_running(port),process:steamvr_identity(),
        pending:RESTARTING.load(Ordering::Acquire) || surfxr_server_io::driver_start_pending(&crate::get_filesystem_layout().session()),
    }
}

#[cfg(not(windows))]
pub fn maybe_kill_steamvr() {
    let mut system = System::new_all();

    #[allow(unused_variables)]
    for process in system.processes_by_name(OsStr::new(&afs::exec_fname("vrmonitor"))) {
        debug!("Killing vrmonitor");

        #[cfg(target_os = "linux")]
        linux_steamvr::terminate_process(process);
        #[cfg(windows)]
        windows_steamvr::kill_process(process.pid().as_u32());

        thread::sleep(Duration::from_secs(1));
    }

    system.refresh_processes(ProcessesToUpdate::All, true);

    #[allow(unused_variables)]
    for process in system.processes_by_name(OsStr::new(&afs::exec_fname("vrserver"))) {
        debug!("Killing vrserver");

        #[cfg(target_os = "linux")]
        linux_steamvr::terminate_process(process);
        #[cfg(windows)]
        windows_steamvr::kill_process(process.pid().as_u32());

        thread::sleep(Duration::from_secs(1));
    }
}

fn unblock_surfxr_driver() -> Result<()> {
    if !cfg!(target_os = "linux") {
        return Ok(());
    }

    let path = surfxr_server_io::steamvr_settings_file_path()?;
    let text = fs::read_to_string(&path).with_context(|| format!("Failed to read {path:?}"))?;
    let new_text = unblock_surfxr_driver_within_vrsettings(text.as_str())
        .with_context(|| "Failed to rewrite .vrsettings.")?;
    fs::write(&path, new_text)
        .with_context(|| "Failed to write .vrsettings back after changing it.")?;
    Ok(())
}

// Reads and writes back steamvr.vrsettings in order to
// ensure the SURFXR driver is not blocked (safe mode).
fn unblock_surfxr_driver_within_vrsettings(text: &str) -> Result<String> {
    let mut settings = serde_json::from_str::<serde_json::Value>(text)?;
    let values = settings
        .as_object_mut()
        .with_context(|| "Failed to parse .vrsettings.")?;
    let blocked = values
        .get(DRIVER_KEY)
        .and_then(|driver| driver.get(BLOCKED_KEY))
        .and_then(|blocked| blocked.as_bool())
        .unwrap_or(false);

    if blocked {
        debug!("Unblocking SURFXR driver in SteamVR.");
        if !values.contains_key(DRIVER_KEY) {
            values.insert(DRIVER_KEY.into(), json!({}));
        }
        let driver = settings[DRIVER_KEY]
            .as_object_mut()
            .with_context(|| "Did not find SURFXR key in settings.")?;
        driver.insert(BLOCKED_KEY.into(), json!(false)); // overwrites if present
    } else {
        debug!("SURFXR is not blocked in SteamVR.");
    }

    Ok(serde_json::to_string_pretty(&settings)?)
}

pub struct Launcher {
    _phantom: PhantomData<()>,
}

impl Launcher {
    pub fn launch_steamvr(&self) {
        if let Err(error)=self.launch_steamvr_inner(false){error!("Failed to launch SteamVR: {error}");}
    }

    // Use the same saved launch policy for headset and Dashboard requests.
    // Direct launch remains an explicit escape hatch for Steam cloud-sync waits.
    pub fn launch_frame_steamvr(&self)->Result<()> {
        self.launch_steamvr_inner(false)
    }

    fn launch_steamvr_inner(&self, force_steam: bool)->Result<()> {
        // The ADB server might be left running because of an unclean termination of SteamVR.
        // Kill it unconditionally to ensure clean state, regardless of current connection mode.
        // Note: this will also kill a system-wide ADB server not started by SURFXR.

        #[cfg(target_os = "linux")]
        {
            if let Err(e) = linux_steamvr::maybe_unwrap_vrcompositor_launcher() {
                error!("Could not remove a leftover vrcompositor wrapper: {e}");
            }
            linux_steamvr::linux_hardware_checks();
        }

        let surfxr_driver_dir = crate::get_filesystem_layout().openvr_driver_root_dir;

        // Make sure to unregister any other SURFXR driver because it would cause a socket conflict
        let other_surfxr_dirs = surfxr_server_io::get_registered_drivers()
            .unwrap_or_default()
            .into_iter()
            .filter(|path| {
                path.to_string_lossy().to_lowercase().contains("surfxr") && *path != surfxr_driver_dir
            })
            .collect::<Vec<_>>();
        surfxr_server_io::driver_registration(&other_surfxr_dirs, false).ok();

        surfxr_server_io::driver_registration(&[surfxr_driver_dir], true).context("Cannot register SURFXR driver")?;

        if let Err(err) = unblock_surfxr_driver() {
            warn!("Failed to unblock SURFXR driver: {:?}", err);
        }

        let layout = crate::get_filesystem_layout();
        let session = data_sources::get_read_only_local_session();
        if is_steamvr_running() {
            if runtime_probe::surfxr_driver_running(session.settings().connection.web_server_port)
                || surfxr_server_io::driver_start_pending(&layout.session())
            {
                return Ok(());
            }
            // A direct SteamVR start can be alive with SURFXR intentionally declined.
            // Explicit SURFXR launch must replace it rather than silently do nothing.
            warn!("Restarting SteamVR to load the SURFXR virtual headset.");
            runtime_probe::request_shutdown(session.settings().connection.web_server_port, false).ok();
            #[cfg(windows)]
            windows_steamvr::request_monitor_close()?;
            self.ensure_steamvr_shutdown()?;
            if is_steamvr_running() {
                surfxr_common::anyhow::bail!("SteamVR did not stop; cannot launch the SURFXR virtual headset");
            }
        }

        #[cfg(windows)]
        shutdown::wait_for_exit()?;
        surfxr_server_io::authorize_next_driver_start(&layout.session()).context("Cannot authorize SURFXR driver initialization")?;

        debug!("SteamVR is dead. Launching...");

        let launched = if use_direct_launch(session.settings().extra.steamvr_launcher.direct_launch, force_steam) {
            let start_script = layout.server_start_script();

            if start_script.exists() {
                debug!("Running VR server start script: {}", start_script.display());

                spawn_and_reap(&mut Command::new(&start_script))
            } else if let Ok(steamvr_bin_dir) =
                surfxr_server_io::steamvr_root_dir().map(|root| root.join("bin"))
            {
                let steamvr_path = if cfg!(windows) {
                    steamvr_bin_dir.join("win64").join("vrstartup.exe")
                } else {
                    steamvr_bin_dir.join("vrmonitor.sh")
                };

                debug!("Launching SteamVR from path: {}", steamvr_path.display());

                spawn_and_reap(&mut Command::new(&steamvr_path))
            } else {
                Err(surfxr_common::anyhow::anyhow!(
                    "Failed to find SteamVR files to directly launch SteamVR"
                ))
            }
        } else {
            #[cfg(windows)]
            let result = windows_steamvr::launch_steamvr_with_steam();

            #[cfg(target_os = "linux")]
            let result = linux_steamvr::launch_steamvr_with_steam();
            result
        };
        if let Err(e) = launched {
            surfxr_server_io::cancel_driver_start(&layout.session());
            return Err(e);
        }
        Ok(())
    }

    pub fn ensure_steamvr_shutdown(&self) -> Result<()> {
        #[cfg(windows)]
        { return shutdown::wait_for_exit(); }
        #[cfg(not(windows))]
        {
        debug!("Waiting for SteamVR to shutdown...");
        let start_time = Instant::now();
        while start_time.elapsed() < SHUTDOWN_TIMEOUT && is_steamvr_running() {
            thread::sleep(Duration::from_millis(500));
        }

        maybe_kill_steamvr();
        Ok(())
        }
    }

    pub fn restart_steamvr(&self) {
        if let Err(error) = self.restart(false) { error!("Failed to restart SteamVR: {error}"); }
    }

    pub fn restart_with_steam(&self) {
        if let Err(error) = self.restart(true) { error!("Failed to restart SteamVR: {error}"); }
    }

    pub(crate) fn restart(&self, force_steam: bool) -> Result<()> {
        RESTARTING.store(true, Ordering::Release);
        struct RestartGuard;
        impl Drop for RestartGuard {
            fn drop(&mut self) { RESTARTING.store(false, Ordering::Release); }
        }
        let _guard = RestartGuard;
        // Dispatch before waiting; the old UI used an asynchronous request queue
        // racing this worker, which could time out before shutdown was delivered.
        let port = data_sources::get_read_only_local_session().settings().connection.web_server_port;
        if runtime_probe::request_shutdown(port, true).is_err() {
            #[cfg(windows)]
            windows_steamvr::request_monitor_close()?;
        }
        self.ensure_steamvr_shutdown()?;
        self.launch_steamvr_inner(force_steam)
    }
}

// Singleton with exclusive access
pub static LAUNCHER: Mutex<Launcher> = Mutex::new(Launcher {
    _phantom: PhantomData,
});

// Maintenance mode must remain alive through driver initialization, just like
// the GUI owner. Spawning Steam is not proof that SURFXR has initialized.
pub(crate) fn wait_for_driver() -> Result<()> {
    let port = data_sources::get_read_only_local_session().settings().connection.web_server_port;
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
    while std::time::Instant::now() < deadline {
        if runtime_probe::surfxr_driver_running(port) { return Ok(()); }
        thread::sleep(std::time::Duration::from_millis(250));
    }
    surfxr_common::anyhow::bail!("SURFXR driver did not become ready. Check Steam startup and the driver log.")
}

fn use_direct_launch(saved_direct: bool, force_steam: bool) -> bool { saved_direct && !force_steam }

#[cfg(test)]
mod launch_tests {
    #[test]
    fn headset_and_dashboard_share_policy_and_recovery_overrides_only_this_launch() {
        assert!(!super::use_direct_launch(false, false));
        assert!(super::use_direct_launch(true, false));
        assert!(!super::use_direct_launch(true, true));
        assert!(!super::use_direct_launch(false, true));
    }
}
