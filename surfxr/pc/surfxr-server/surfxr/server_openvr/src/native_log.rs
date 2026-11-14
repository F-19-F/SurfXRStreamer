//! Native driver logging/hash callbacks retained from the upstream bridge.
use surfxr_common::{log, parking_lot::Mutex};
use std::{collections::HashMap, ffi::{CStr, c_char}, sync::LazyLock, time::{Duration, Instant}};

// The native driver is responsible of interpreting values and calling functions using
// device/input/output identifiers obtained using this function
pub unsafe extern "C" fn surfxr_path_to_id(path_string: *const c_char) -> u64 {
    surfxr_common::hash_string(unsafe { CStr::from_ptr(path_string) }.to_str().unwrap())
}

pub unsafe extern "C" fn surfxr_error(string_ptr: *const c_char) {
    surfxr_common::show_e(unsafe { CStr::from_ptr(string_ptr) }.to_string_lossy());
}

pub unsafe fn log(level: log::Level, string_ptr: *const c_char) {
    log::log!(
        level,
        "{}",
        unsafe { CStr::from_ptr(string_ptr) }.to_string_lossy()
    );
}

pub unsafe extern "C" fn surfxr_warn(string_ptr: *const c_char) {
    unsafe { log(log::Level::Warn, string_ptr) };
}

pub unsafe extern "C" fn surfxr_info(string_ptr: *const c_char) {
    unsafe { log(log::Level::Info, string_ptr) };
}

pub unsafe extern "C" fn surfxr_dbg_server_impl(_string_ptr: *const c_char) {
    surfxr_common::dbg_server_impl!(
        "{}",
        unsafe { CStr::from_ptr(_string_ptr) }.to_string_lossy()
    );
}

pub unsafe extern "C" fn surfxr_dbg_encoder(_string_ptr: *const c_char) {
    surfxr_common::dbg_encoder!(
        "{}",
        unsafe { CStr::from_ptr(_string_ptr) }.to_string_lossy()
    );
}

// Should not be used in production
pub unsafe extern "C" fn surfxr_log_periodically(tag_ptr: *const c_char, message_ptr: *const c_char) {
    const INTERVAL: Duration = Duration::from_secs(1);
    static LATEST_TAG_TIMESTAMPS: LazyLock<Mutex<HashMap<String, Instant>>> =
        LazyLock::new(|| Mutex::new(HashMap::new()));

    let tag = unsafe { CStr::from_ptr(tag_ptr) }.to_string_lossy();
    let message = unsafe { CStr::from_ptr(message_ptr) }.to_string_lossy();

    let mut timestamps_ref = LATEST_TAG_TIMESTAMPS.lock();
    let old_timestamp = timestamps_ref
        .entry(tag.to_string())
        .or_insert_with(Instant::now);
    if *old_timestamp + INTERVAL < Instant::now() {
        *old_timestamp += INTERVAL;

        log::warn!("{}: {}", tag, message);
    }
}
