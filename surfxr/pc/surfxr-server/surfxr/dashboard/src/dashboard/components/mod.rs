mod about;
mod devices;
mod surfxr_settings;
mod gaze_settings;
mod foveation_policy;
mod logs;
mod notifications;
mod settings;
mod settings_controls;
mod setup_wizard;
mod statistics;

#[cfg(not(target_arch = "wasm32"))]
mod installation;

pub use about::*;
pub use devices::*;
pub use logs::*;
pub use notifications::*;
pub use settings::*;
pub use settings_controls::*;
pub use setup_wizard::*;
pub use statistics::*;

#[cfg(not(target_arch = "wasm32"))]
pub use installation::*;

#[cfg(not(target_arch = "wasm32"))]
mod updates;
