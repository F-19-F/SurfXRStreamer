use crate::data_sources::PolledEvent;
use surfxr_common::{LogEntry, LogSeverity, log::LevelFilter, parking_lot::Mutex};
use surfxr_events::{Event, EventType};
use std::{
    fs::{self, OpenOptions},
    io::Write,
    panic,
    path::PathBuf,
    sync::{Arc, mpsc},
};

// Lifecycle records must survive a GUI panic even when Windows supplies no
// console. Keep two bounded files and exclude session data and panic payloads.
const LIFECYCLE_LIMIT: u64 = 64 * 1024;
pub struct Lifecycle {
    path: Arc<Mutex<PathBuf>>,
}
fn lifecycle_record(path: &Mutex<PathBuf>, event: &str, detail: &str) {
    let path = path.lock();
    if fs::metadata(&*path).is_ok_and(|m| m.len() >= LIFECYCLE_LIMIT) {
        let previous = path.with_extension("previous.log");
        if fs::rename(&*path, &previous).is_err() {
            // Windows rename cannot replace the previous file.
            let _ = fs::remove_file(&previous);
            let _ = fs::rename(&*path, &previous);
        }
        if fs::metadata(&*path).is_ok_and(|m| m.len() >= LIFECYCLE_LIMIT) {
            return;
        }
    }
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(&*path) {
        let _ = writeln!(
            file,
            "{} pid={} {} {}",
            chrono::Utc::now().to_rfc3339(),
            std::process::id(),
            event,
            detail
        );
        let _ = file.flush();
    }
}
impl Lifecycle {
    pub fn start() -> Self {
        let path = Arc::new(Mutex::new(
            crate::get_filesystem_layout()
                .log_dir
                .join("dashboard_lifecycle.log"),
        ));
        lifecycle_record(&path, "started", "");
        let previous = panic::take_hook();
        let panic_path = Arc::clone(&path);
        panic::set_hook(Box::new(move |info| {
            let location = info
                .location()
                .map(|p| format!("{}:{}:{}", p.file(), p.line(), p.column()))
                .unwrap_or_default();
            lifecycle_record(&panic_path, "panic", &location);
            // The existing stderr hook retains the normal Rust panic output.
            previous(info);
        }));
        Self { path }
    }
    pub fn native_result(&self, success: bool) {
        lifecycle_record(
            &self.path,
            if success {
                "native_loop_returned"
            } else {
                "native_loop_error"
            },
            "",
        );
    }
}
impl Drop for Lifecycle {
    fn drop(&mut self) {
        lifecycle_record(
            &self.path,
            if std::thread::panicking() {
                "unwinding_exit"
            } else {
                "normal_exit"
            },
            "",
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lifecycle_rotation_and_clean_exit_are_persisted_without_gui() {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let directory =
            std::env::temp_dir().join(format!("surfxr-lifecycle-{}-{unique}", std::process::id()));
        fs::create_dir(&directory).unwrap();
        let file = directory.join("dashboard_lifecycle.log");
        fs::write(&file, vec![b'x'; LIFECYCLE_LIMIT as usize]).unwrap();
        let lifecycle = Lifecycle {
            path: Arc::new(Mutex::new(file.clone())),
        };
        lifecycle.native_result(true);
        drop(lifecycle);
        let contents = fs::read_to_string(&file).unwrap();
        assert!(contents.contains("native_loop_returned") && contents.contains("normal_exit"));
        assert!(contents.len() < 1024);
        assert_eq!(
            fs::metadata(file.with_extension("previous.log"))
                .unwrap()
                .len(),
            LIFECYCLE_LIMIT
        );
        fs::remove_file(&file).unwrap();
        fs::remove_file(file.with_extension("previous.log")).unwrap();
        fs::remove_dir(directory).unwrap();
    }
}

static LEVEL: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(2);
pub fn set_level(level: LogSeverity) {
    LEVEL.store(level.into_log_level() as usize, std::sync::atomic::Ordering::Relaxed);
}
pub fn accepts(entry: &LogEntry) -> bool {
    entry.severity.into_log_level() as usize <= LEVEL.load(std::sync::atomic::Ordering::Relaxed)
}
struct Logger(env_logger::Logger);
impl surfxr_common::log::Log for Logger {
    fn enabled(&self, meta: &surfxr_common::log::Metadata) -> bool {
        meta.level() as usize <= LEVEL.load(std::sync::atomic::Ordering::Relaxed) && self.0.enabled(meta)
    }
    fn log(&self, record: &surfxr_common::log::Record) {
        if self.enabled(record.metadata()) { self.0.log(record); }
    }
    fn flush(&self) { self.0.flush(); }
}

pub fn init_logging(event_sender: mpsc::Sender<PolledEvent>) {
    let event_sender = Arc::new(Mutex::new(event_sender));

    let logger = env_logger::Builder::new()
        .filter(Some("surfxr_events"), LevelFilter::Off)
        .filter(Some("naga"), LevelFilter::Off)
        .filter(Some("ureq"), LevelFilter::Off)
        .filter(Some("wgpu_core"), LevelFilter::Off)
        .filter(Some("wgpu_hal"), LevelFilter::Off)
        .filter_level(LevelFilter::Debug)
        .format(move |f, record| {
            let timestamp = chrono::Local::now().format("%H:%M:%S.%3f").to_string();

            event_sender
                .lock()
                .send(PolledEvent {
                    inner: Event {
                        timestamp: timestamp.clone(),
                        event_type: EventType::Log(LogEntry {
                            severity: LogSeverity::from_log_level(record.level()),
                            content: format!("{}", record.args()),
                        }),
                    },
                    from_dashboard: true,
                })
                .ok();

            writeln!(
                f,
                "[{} {} {}] {}",
                timestamp,
                record.level(),
                record.module_path().unwrap_or_default(),
                record.args()
            )
        })
        .build();
    surfxr_common::log::set_boxed_logger(Box::new(Logger(logger))).unwrap();
    surfxr_common::log::set_max_level(LevelFilter::Debug);
}
