//! Wait for the whole runtime generation, not only vrserver. Never kill Steam.
use surfxr_common::anyhow::{Result, bail};
use std::{
    path::{Path, PathBuf},
    thread,
    time::{Duration, Instant},
};
use sysinfo::System;

fn runtime_executable(path: &Path, root: &Path) -> bool {
    // Canonical paths are used by the caller; require both runtime ownership and
    // a runtime process name. Games and third-party overlays are never targets.
    path.starts_with(root)
        && path
            .file_name()
            .and_then(|n| n.to_str())
            .is_some_and(|name| {
                [
                    "vrstartup.exe",
                    "vrserver.exe",
                    "vrmonitor.exe",
                    "vrcompositor.exe",
                    "vrdashboard.exe",
                    "vrwebhelper.exe",
                ]
                .iter()
                .any(|n| name.eq_ignore_ascii_case(n))
            })
}

fn snapshot(root: &Path) -> Vec<PathBuf> {
    System::new_all()
        .processes()
        .values()
        .filter_map(|p| {
            let executable = p.exe()?.canonicalize().ok()?;
            runtime_executable(&executable, root).then_some(executable)
        })
        .collect()
}

pub(super) fn wait_for_exit() -> Result<()> {
    let root = surfxr_server_io::steamvr_root_dir()?.canonicalize()?;
    let deadline = Instant::now() + Duration::from_secs(15);
    while !snapshot(&root).is_empty() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(200));
    }
    // Fail closed instead of forcing vrserver down while Steam's UI is still
    // attached. A user can close a stuck application without losing its state.
    wait_until_quiet(|| snapshot(&root).is_empty(), Instant::now, thread::sleep)
}

fn wait_until_quiet(
    mut empty: impl FnMut() -> bool,
    mut now: impl FnMut() -> Instant,
    mut sleep: impl FnMut(Duration),
) -> Result<()> {
    let deadline = now() + Duration::from_secs(5);
    let mut quiet_since = None;
    loop {
        let at = now();
        if empty() {
            let since = *quiet_since.get_or_insert(at);
            if at.saturating_duration_since(since) >= Duration::from_secs(1) {
                return Ok(());
            }
        } else {
            quiet_since = None;
        }
        if at >= deadline {
            bail!(
                "SteamVR processes have not finished exiting; launch cancelled. Close SteamVR and retry."
            );
        }
        sleep(Duration::from_millis(100));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    #[test]
    fn only_runtime_owned_processes_are_targets() {
        let root = Path::new("runtime");
        assert!(runtime_executable(
            Path::new("runtime/bin/vrwebhelper.exe"),
            root
        ));
        for path in [
            "runtime/bin/steamwebhelper.exe",
            "other/vrserver.exe",
            "runtime/bin/game.exe",
        ] {
            assert!(!runtime_executable(Path::new(path), root));
        }
    }
    #[test]
    fn late_children_reset_quiet_period_and_stalls_fail_closed() {
        let start = Instant::now();
        let elapsed = Cell::new(Duration::ZERO);
        let result = wait_until_quiet(
            || !(Duration::from_millis(500)..Duration::from_millis(1300)).contains(&elapsed.get()),
            || start + elapsed.get(),
            |step| elapsed.set(elapsed.get() + step),
        );
        assert!(result.is_ok());
        assert!(elapsed.get() >= Duration::from_millis(2300));
        elapsed.set(Duration::ZERO);
        assert!(
            wait_until_quiet(
                || false,
                || start + elapsed.get(),
                |d| elapsed.set(elapsed.get() + d)
            )
            .is_err()
        );
        assert_eq!(elapsed.get(), Duration::from_secs(5));
    }
}
