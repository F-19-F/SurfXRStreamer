#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
fn main() {
    #[cfg(windows)]
    std::process::exit(surfxr_server_openvr::frame_worker::run());
    #[cfg(not(windows))]
    std::process::exit(2);
}
