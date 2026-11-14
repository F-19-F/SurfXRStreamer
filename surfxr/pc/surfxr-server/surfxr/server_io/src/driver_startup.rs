//! Manual SteamVR starts are opt-in; explicit SURFXR launches grant one bounded ticket.
use surfxr_common::anyhow::Result;
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

const TICKET_LIFETIME_SECONDS: u64 = 120;
static SERIAL: AtomicU64 = AtomicU64::new(0);

fn now_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn ticket_path(session_path: &Path) -> PathBuf {
    session_path.with_file_name("steamvr-launch-request")
}

fn scratch_path(session_path: &Path) -> PathBuf {
    session_path.with_file_name(format!(
        "steamvr-launch-{}-{}.tmp",
        std::process::id(),
        SERIAL.fetch_add(1, Ordering::Relaxed)
    ))
}

pub fn authorize_next_driver_start(session_path: &Path) -> Result<()> {
    let pending = scratch_path(session_path);
    let result = (|| {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&pending)?;
        write!(file, "SURFXR_START_1\n{}\n", now_seconds())?;
        file.sync_all()?;
        // Windows rename does not replace an old, expired or failed launch ticket.
        cancel_driver_start(session_path);
        fs::rename(&pending, ticket_path(session_path))?;
        Ok(())
    })();
    fs::remove_file(pending).ok();
    result
}

pub fn cancel_driver_start(session_path: &Path) {
    fs::remove_file(ticket_path(session_path)).ok();
    fs::remove_file(claim_path(session_path)).ok();
}

fn claim_path(session_path: &Path) -> PathBuf {
    session_path.with_file_name("steamvr-launch-request.claim")
}

fn valid_ticket(path: &Path, now: u64) -> bool {
    let Ok(file) = fs::File::open(path) else {
        return false;
    };
    let mut bytes = String::new();
    if file.take(65).read_to_string(&mut bytes).is_err() || bytes.len() > 64 {
        return false;
    }
    let mut lines = bytes.lines();
    if lines.next() != Some("SURFXR_START_1") {
        return false;
    }
    let Some(created) = lines.next().and_then(|n| n.parse::<u64>().ok()) else {
        return false;
    };
    lines.next().is_none()
        && now
            .checked_sub(created)
            .is_some_and(|age| age < TICKET_LIFETIME_SECONDS)
}

pub fn driver_start_pending(session_path: &Path) -> bool {
    valid_ticket(&ticket_path(session_path), now_seconds())
}

fn consume_ticket(session_path: &Path, now: u64) -> bool {
    // A create-new claim serializes readers on Windows too. Factory probes do not consume it.
    let claim = claim_path(session_path);
    let Ok(lock) = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&claim)
    else {
        return false;
    };
    let ticket = ticket_path(session_path);
    let valid = valid_ticket(&ticket, now);
    let removed = fs::remove_file(ticket).is_ok();
    drop(lock);
    fs::remove_file(claim).ok();
    valid && removed
}

pub fn driver_start_allowed(session_path: &Path) -> bool {
    let explicit_launch = consume_ticket(session_path, now_seconds());
    // Read only. A driver declining initialization must not migrate settings or start a server.
    let manual = fs::read_to_string(session_path)
        .ok()
        .and_then(|text| serde_json::from_str::<serde_json::Value>(&text).ok())
        .is_some_and(|session| {
            session["session_settings"]["extra"]["steamvr_launcher"]
            ["enable_surfxr_on_manual_steamvr_start"].as_bool() == Some(true)
        });
    explicit_launch || manual
}

#[cfg(test)]
mod tests {
    use super::*;
    struct TestDir(PathBuf);
    impl TestDir {
        fn new() -> Self {
            let dir = std::env::temp_dir().join(format!(
                "surfxr-startup-{}-{}",
                std::process::id(),
                SERIAL.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&dir).unwrap();
            Self(dir)
        }
        fn session(&self) -> PathBuf {
            self.0.join("session.json")
        }
    }
    impl Drop for TestDir {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.0).ok();
        }
    }

    #[test]
    fn manual_default_is_off_and_old_settings_migrate_without_changing_other_values() {
        let dir = TestDir::new();
        let path = dir.session();
        let mut session = surfxr_session::SessionConfig::default();
        assert!(
            !session
                .session_settings
                .extra
                .steamvr_launcher
                .enable_surfxr_on_manual_steamvr_start
        );
        let mut old = serde_json::to_value(&session).unwrap();
        old["session_settings"]["extra"]["steamvr_launcher"]
            .as_object_mut()
            .unwrap()
            .remove("enable_surfxr_on_manual_steamvr_start");
        old["session_settings"]["extra"]["steamvr_launcher"]["direct_launch"] = true.into();
        session.merge_from_json(&old).unwrap();
        assert!(
            session
                .session_settings
                .extra
                .steamvr_launcher
                .direct_launch
        );
        assert!(
            !session
                .session_settings
                .extra
                .steamvr_launcher
                .enable_surfxr_on_manual_steamvr_start
        );
        let text = serde_json::to_string(&old).unwrap();
        fs::write(&path, &text).unwrap();
        assert!(!driver_start_allowed(&path));
        assert_eq!(fs::read_to_string(&path).unwrap(), text);
        old["session_settings"]["extra"]["steamvr_launcher"]["enable_surfxr_on_manual_steamvr_start"] =
            true.into();
        fs::write(&path, serde_json::to_string(&old).unwrap()).unwrap();
        assert!(driver_start_allowed(&path));
        assert!(driver_start_allowed(&path));
    }

    #[test]
    fn explicit_launch_is_single_use_cancelable_and_consumed_even_when_manual_is_on() {
        let dir = TestDir::new();
        let path = dir.session();
        authorize_next_driver_start(&path).unwrap();
        assert!(driver_start_pending(&path));
        assert!(driver_start_allowed(&path));
        assert!(!driver_start_allowed(&path));
        authorize_next_driver_start(&path).unwrap();
        cancel_driver_start(&path);
        assert!(!driver_start_allowed(&path));
        // A failed process may leave its claim; a new explicit launch replaces it.
        fs::write(claim_path(&path), "").unwrap();
        authorize_next_driver_start(&path).unwrap();
        let enabled = r#"{"session_settings":{"extra":{"steamvr_launcher":{"enable_surfxr_on_manual_steamvr_start":true}}}}"#;
        fs::write(&path, enabled).unwrap();
        assert!(driver_start_allowed(&path));
        fs::write(&path, "{}").unwrap();
        assert!(!driver_start_allowed(&path));
    }

    #[test]
    fn stale_future_malformed_and_oversized_tickets_do_not_enable_manual_starts() {
        let dir = TestDir::new();
        let path = dir.session();
        for text in [
            "SURFXR_START_1\n0\n".to_owned(),
            "SURFXR_START_1\n101\n".into(),
            "SURFXR_START_1\n100\nextra\n".into(),
            "X".repeat(65),
        ] {
            fs::write(ticket_path(&path), text).unwrap();
            assert!(!consume_ticket(&path, 1000));
        }
        fs::write(ticket_path(&path), "SURFXR_START_1\n1001\n").unwrap();
        assert!(!consume_ticket(&path, 1000));
        fs::write(ticket_path(&path), "SURFXR_START_1\n881\n").unwrap();
        assert!(consume_ticket(&path, 1000));
        fs::write(ticket_path(&path), "SURFXR_START_1\n880\n").unwrap();
        assert!(!consume_ticket(&path, 1000));
    }

    #[test]
    fn simultaneous_driver_initializations_cannot_reuse_the_ticket() {
        let dir = TestDir::new();
        let path = dir.session();
        authorize_next_driver_start(&path).unwrap();
        let threads: Vec<_> = (0..4)
            .map(|_| {
                let path = path.clone();
                std::thread::spawn(move || driver_start_allowed(&path))
            })
            .collect();
        assert_eq!(
            threads
                .into_iter()
                .map(|t| t.join().unwrap())
                .filter(|allowed| *allowed)
                .count(),
            1
        );
    }
}
