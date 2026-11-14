//! Gate at driver Init, never at HmdDriverFactory (Steam also probes factories).
use crate::FACTORY_INIT_DATA;
use std::sync::OnceLock;

#[unsafe(export_name = "ShouldInitializeRuntime")]
pub extern "C" fn should_initialize_runtime() -> bool {
    static ALLOWED: OnceLock<bool> = OnceLock::new();
    *ALLOWED.get_or_init(|| {
        FACTORY_INIT_DATA.lock().as_ref().is_some_and(|data| {
            surfxr_server_io::driver_start_allowed(&data.filesystem_layout.session())
        })
    })
}
