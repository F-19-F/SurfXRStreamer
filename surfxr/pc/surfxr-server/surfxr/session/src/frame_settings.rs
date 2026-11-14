//! SurfXR's single-writer settings protocol. Reads never become edits.
use serde::{Deserialize, Serialize};

pub const TIERS: [(u32, u32); 9] = [(1344,1440),(1728,1824),(2112,2304),
    (2496,2688),(2688,2880),(3072,3264),(3648,4032),(4224,4608),(4992,5376)];
pub const RATES: [u32; 6] = [60,72,80,90,120,144];

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct FrameSettingsSync {
    pub revision: u64,
    #[serde(default)]
    pub prefer_pc_mouse: bool,
    // Retained across Dashboard/driver ownership changes. Retrying a command
    // whose HTTP response was lost must not overwrite a subsequent PC edit.
    pub receipts: Vec<(u64, u64)>,
}
#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct FrameSettingsRequest {
    pub token: u64,
    pub sequence: u64,
    // Bit 7 opts into the pointer-policy snapshot; low bits: 5 pointer policy.
    // 0 read, 1 refresh rate, 2 tier (-1..7), 3 bitrate, 4 exit SteamVR.
    pub action: u8,
    pub value: f64,
}
impl FrameSettingsRequest {
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len()!=25 { return None; }
        Some(Self { token:u64::from_le_bytes(bytes[0..8].try_into().ok()?),
            sequence:u64::from_le_bytes(bytes[8..16].try_into().ok()?), action:bytes[16],
            value:f64::from_le_bytes(bytes[17..25].try_into().ok()?) })
    }
}
#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct FrameSettingsSnapshot {
    pub sequence: u64,
    pub revision: u64,
    #[serde(default)]
    pub prefer_pc_mouse: bool,
    pub fps: f64,
    // -2 is a custom PC resolution. It is displayed, never rounded to a tier.
    pub quality: f64,
    // Zero denotes an uncapped adaptive bitrate.
    pub bitrate: f64,
}
impl FrameSettingsSnapshot {
    pub fn bytes(&self, extended: bool) -> Vec<u8> {
        let mut out=self.sequence.to_le_bytes().to_vec();
        out.extend(self.revision.to_le_bytes());
        for value in [self.fps,self.quality,self.bitrate] { out.extend(value.to_le_bytes()); }
        if extended { out.extend((if self.prefer_pc_mouse {1.0f64} else {0.0f64}).to_le_bytes()); }
        out
    }
}
