use surfxr_session::OpenvrPropKey;

pub(super) fn set_properties(left: bool, set: impl Fn(OpenvrPropKey, &str)) {
    use OpenvrPropKey::*;
    // Report the actual wireless controller identity to SteamVR.
    set(DeviceIsWirelessBool, "true");
    let hand = if left { "left" } else { "right" };
    // SteamVR's Frame assets use "seaching" (sic), info/error instead of
    // searching_alert/ready_alert, and have no dedicated standby artwork.
    for (key, suffix) in [
        (NamedIconPathDeviceOffString, "off.png"),
        (NamedIconPathDeviceSearchingString, "seaching.gif"),
        (NamedIconPathDeviceSearchingAlertString, "error.png"),
        (NamedIconPathDeviceReadyString, "ready.png"),
        (NamedIconPathDeviceReadyAlertString, "info.png"),
        (NamedIconPathDeviceNotReadyString, "error.png"),
        (NamedIconPathDeviceAlertLowString, "ready_low.png"),
        (NamedIconPathDeviceStandbyString, "off.png"),
        (NamedIconPathDeviceStandbyAlertString, "info.png"),
    ] {
        set(key, &format!("{{frame_controller}}/icons/{hand}_frame_controller_{suffix}"));
    }
}
