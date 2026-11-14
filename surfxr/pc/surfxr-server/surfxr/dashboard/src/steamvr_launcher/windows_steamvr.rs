use surfxr_common::anyhow::Result;
use std::os::windows::process::CommandExt;
use std::process::Command;

const CREATE_NO_WINDOW: u32 = 0x0800_0000;

pub fn launch_steamvr_with_steam() -> Result<()> {
    super::spawn_and_reap(
        Command::new("cmd")
            .args(["/C", "start", "steam://rungameid/250820"])
            .creation_flags(CREATE_NO_WINDOW),
    )
}

// Use the monitor's own quit URI. WM_CLOSE/taskkill can merely hide its window.
// Invoke the registered runtime directly, never an arbitrary protocol handler.
pub fn request_monitor_close() -> Result<()> {
    let expected = surfxr_server_io::steamvr_root_dir()?
        .join("bin/win64/vrmonitor.exe")
        .canonicalize()?;
    for process in sysinfo::System::new_all().processes().values() {
        if process.exe().and_then(|p| p.canonicalize().ok()).as_ref() == Some(&expected) {
            return super::spawn_and_reap(
                Command::new(&expected)
                    .arg("vrmonitor://quit")
                    .creation_flags(CREATE_NO_WINDOW),
            );
        }
    }
    Ok(())
}
