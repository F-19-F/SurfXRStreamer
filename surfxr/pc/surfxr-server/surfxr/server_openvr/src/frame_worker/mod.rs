//! Frame encoder/video socket owner. It never initializes an OpenVR client.
mod child;
mod keyboard;
mod parent;
mod audio;
mod network;
use std::sync::atomic::{AtomicBool, Ordering};
static CHILD: AtomicBool = AtomicBool::new(false);
pub(crate) fn is_child() -> bool {
    CHILD.load(Ordering::Acquire)
}
pub(crate) use child::{send_video, encoder_params};

struct WorkerLogger;
static LOGGER: WorkerLogger = WorkerLogger;
impl log::Log for WorkerLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= log::Level::Info
    }
    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            let message =
                std::ffi::CString::new(record.args().to_string().replace('\0', "")).unwrap();
            let level = match record.level() {
                log::Level::Error => 0,
                log::Level::Warn => 1,
                _ => 2,
            };
            unsafe { crate::bindings::FrameWorkerRelayLog(message.as_ptr(), level) };
        }
    }
    fn flush(&self) {}
}
pub fn run() -> i32 {
    let arguments = std::env::args().collect::<Vec<_>>();
    if arguments.len() != 4 || arguments[1] != "--frame-worker" {
        return 2;
    }
    let (Ok(mapping), Ok(wake)) = (arguments[2].parse::<u64>(), arguments[3].parse::<u64>()) else {
        return 2;
    };
    if mapping == 0 || wake == 0 {
        return 2;
    }
    CHILD.store(true, Ordering::Release);
    if log::set_logger(&LOGGER).is_err() {
        return 2;
    }
    log::set_max_level(log::LevelFilter::Info);
    unsafe { crate::bindings::FrameWorkerRun(mapping, wake) }
}
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonWorkerRunV6(mapping:u64,wake:u64,socket:u64)->i32{
    if mapping==0||wake==0||socket==0{return 2;}
    CHILD.store(true,Ordering::Release);
    let _=log::set_logger(&LOGGER);
    log::set_max_level(log::LevelFilter::Info);
    unsafe {crate::bindings::FrameWorkerDaemonRun(mapping,wake,socket)}
}
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonWorkerRunV7(mapping:u64,wake:u64,owner:*mut std::ffi::c_void,send:crate::bindings::FfiFrameWorkerSend)->i32{
    if mapping==0||wake==0||owner.is_null()||send.is_none(){return 2;}
    CHILD.store(true,Ordering::Release);
    let _=log::set_logger(&LOGGER);log::set_max_level(log::LevelFilter::Info);
    unsafe {crate::bindings::FrameWorkerDaemonRunDirect(mapping,wake,owner,send)}
}
// Keep the old export discoverable, but do not admit the old lease contract.
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonWorkerRunV5(_mapping:u64,_wake:u64,_socket:u64)->i32{2}
