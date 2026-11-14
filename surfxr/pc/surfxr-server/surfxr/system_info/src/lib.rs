//! Compatibility-only session enum; no Android client or probing runtime.
use surfxr_common::settings_schema::SettingsSchema;
use serde::{Deserialize, Serialize};

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub enum ClientFlavor {
    Store,
    Github,
    Custom(String),
}
