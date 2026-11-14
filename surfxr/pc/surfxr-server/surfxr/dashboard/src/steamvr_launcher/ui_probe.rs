//! Isolated OpenVR background query: no scene ownership, settings writes or launch.
//! Executed in a bounded child so a stuck runtime cannot hang the Dashboard.
use surfxr_common::anyhow::{Context, Result, bail};
use libloading::Library;
use std::os::windows::process::CommandExt;
#[allow(
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod sdk {
    include!(concat!(env!("OUT_DIR"), "/overlay_sdk.rs"));
    include!(concat!(env!("OUT_DIR"), "/overlay_interface.rs"));
}
use std::{
    ffi::c_char,
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

pub(super) fn query_child() -> Result<bool> {
    let mut child = Command::new(std::env::current_exe()?)
        .arg("--steamvr-ui-probe")
        .creation_flags(0x08000000)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if let Some(status) = child.try_wait()? {
            return match status.code() {
                Some(0) => Ok(true),
                Some(2) => Ok(false),
                _ => bail!("Steam UI probe unavailable"),
            };
        }
        if Instant::now() >= deadline {
            child.kill().ok();
            child.wait().ok();
            bail!("Steam UI probe timed out");
        }
        thread::sleep(Duration::from_millis(50));
    }
}

pub(crate) fn run() -> i32 {
    match query() {
        Ok(true) => 0,
        Ok(false) => 2,
        Err(_) => 3,
    }
}

fn query() -> Result<bool> {
    if !super::is_steamvr_running() {
        bail!("SteamVR is not running");
    }
    let path = surfxr_server_io::steamvr_root_dir()?.join("bin/win64/openvr_api.dll");
    // Interface identity, table layout and flags come from the bundled public SDK.
    unsafe {
        let library = Library::new(path)?;
        let init =
            library.get::<unsafe extern "C" fn(*mut i32, i32) -> u32>(b"VR_InitInternal\0")?;
        let shutdown = library.get::<unsafe extern "C" fn()>(b"VR_ShutdownInternal\0")?;
        let interface = library.get::<unsafe extern "C" fn(
            *const c_char,
            *mut i32,
        )
            -> *const sdk::VR_IVROverlay_FnTable>(
            b"VR_GetGenericInterface\0"
        )?;
        let mut error = 0;
        init(
            &mut error,
            sdk::EVRApplicationType_VRApplication_Background as i32,
        ); // Background must not start SteamVR (OpenVR contract).
        if error != 0 {
            bail!("OpenVR background initialization failed");
        }
        let result = (|| {
            let table = interface(sdk::INTERFACE.as_ptr().cast(), &mut error);
            if table.is_null() || error != 0 {
                bail!("OpenVR overlay interface unavailable");
            }
            let find = (*table)
                .FindOverlay
                .context("SDK FindOverlay unavailable")?;
            let flags = (*table)
                .GetOverlayFlags
                .context("SDK GetOverlayFlags unavailable")?;
            let mut handle = 0;
            let mut bits = 0;
            if find(
                c"valve.steam.gamepadui.main".as_ptr().cast_mut(),
                &mut handle,
            ) != 0
            {
                return Ok(false);
            }
            // SteamVR's own isVRGamepadUIReady checks this VisibleInDashboard flag.
            if flags(handle, &mut bits) != 0 {
                bail!("Steam UI overlay flags unavailable");
            }
            Ok(bits & sdk::VROverlayFlags_VisibleInDashboard as u32 != 0)
        })();
        shutdown();
        result.context("Steam UI query")
    }
}
