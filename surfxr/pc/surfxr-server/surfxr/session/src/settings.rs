use surfxr_common::{
    SURFXR_VERSION, DebugGroupsConfig, DebugGroupsConfigDefault, LogSeverity, LogSeverityDefault,
    LogSeverityDefaultVariant,
};
#[path = "frame_desktop.rs"]
mod frame_desktop;
use frame_desktop::{FrameDesktopConfig, FrameDesktopConfigDefault};
#[path = "frame_gaze.rs"]
mod frame_gaze;
use frame_gaze::{FrameGazeConfig, FrameGazeConfigDefault};
use surfxr_system_info::{ClientFlavor, ClientFlavorDefault, ClientFlavorDefaultVariant};
use bytemuck::{Pod, Zeroable};
use serde::{Deserialize, Serialize};
use settings_schema::{
    ArrayDefault, DictionaryDefault, OptionalDefault, SettingsSchema, Switch, SwitchDefault,
    VectorDefault,
};

include!(concat!(env!("OUT_DIR"), "/openvr_property_keys.rs"));

pub enum OpenvrPropType {
    Bool,
    Float,
    Int32,
    Uint64,
    Vector3,
    Double,
    String,
}

#[path = "settings/encoding.rs"]
mod encoding;
pub use encoding::*;
#[path = "settings/video.rs"]
mod video;
pub use video::*;
#[path = "settings/audio.rs"]
mod audio;
pub use audio::*;
#[path = "settings/tracking.rs"]
mod tracking;
pub use tracking::*;
#[path = "settings/controllers.rs"]
mod controllers;
pub use controllers::*;
#[path = "settings/connection.rs"]
mod connection;
pub use connection::*;
#[path = "settings/extra.rs"]
mod extra;
pub use extra::*;

pub fn session_settings_default() -> SettingsDefault {
    SettingsDefault {
        video: default_video::make(),
        audio: default_audio::make(),
        headset: default_headset::make(),
        connection: default_connection::make(),
        extra: default_extra::make(),
    }
}
#[path = "settings/default_video.rs"]
mod default_video;
#[path = "settings/default_audio.rs"]
mod default_audio;
#[path = "settings/default_headset.rs"]
mod default_headset;
#[path = "settings/default_connection.rs"]
mod default_connection;
#[path = "settings/default_extra.rs"]
mod default_extra;
