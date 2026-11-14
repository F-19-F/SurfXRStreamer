//! SurfXR-owned endpoint defaults. Third-party destination ports are separate contracts.
pub const OSC_LOCAL: u16 = 19942;
pub const VR_CONTROL: u16 = 19943;
pub const VR_MEDIA: u16 = 19944;
pub const VR_PRIORITY: u16 = 19945;
pub const DASHBOARD: u16 = 19946;
pub const GAZE_OSC: u16 = 19947;
pub const DESKTOP_CONTROL: u16 = 19950;
pub const DESKTOP_MEDIA: u16 = 19951;
pub const DESKTOP_PRIORITY: u16 = 19952;
pub const WEB: u16 = 18082;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn native_and_rust_defaults_are_identical_and_do_not_overlap_legacy_listeners() {
        let header = include_str!("../../../../../shared/platform/include/surfxr/ports.hpp");
        let defaults = [("osc_local", OSC_LOCAL), ("vr_control", VR_CONTROL), ("vr_media", VR_MEDIA),
            ("vr_priority", VR_PRIORITY), ("dashboard", DASHBOARD), ("gaze_osc", GAZE_OSC),
            ("desktop_control", DESKTOP_CONTROL), ("desktop_media", DESKTOP_MEDIA),
            ("desktop_priority", DESKTOP_PRIORITY), ("web", WEB)];
        let old = [9942, 9943, 9944, 9945, 9946, 9950, 9951, 9952, 8082];
        let mut unique = std::collections::HashSet::new();
        for (name, port) in defaults {
            assert!(header.contains(&format!("unsigned {name} = {port};")), "native {name}");
            assert!(!old.contains(&port));
            assert!(unique.insert(port), "duplicate listener {name}");
        }
    }
}
