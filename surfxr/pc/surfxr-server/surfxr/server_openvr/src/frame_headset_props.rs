use surfxr_session::OpenvrPropKey;

pub(super) fn set_icons(set: impl Fn(OpenvrPropKey, &str)) {
    use OpenvrPropKey::*;
    // Frame has off/searching/ready/error/info assets, but no standby or
    // *_alert variants. Missing paths make SteamVR display a generic HMD.
    for (key, suffix) in [
        (NamedIconPathDeviceOffString, "off.png"),
        (NamedIconPathDeviceSearchingString, "searching.gif"),
        (NamedIconPathDeviceSearchingAlertString, "error.png"),
        (NamedIconPathDeviceReadyString, "ready.png"),
        (NamedIconPathDeviceReadyAlertString, "info.png"),
        (NamedIconPathDeviceNotReadyString, "error.png"),
        (NamedIconPathDeviceAlertLowString, "ready_low.png"),
        (NamedIconPathDeviceStandbyString, "off.png"),
        (NamedIconPathDeviceStandbyAlertString, "info.png"),
    ] {
        set(key, &format!("{{vrlink}}/icons/headset_steamframe_{suffix}"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    #[test]
    fn all_frame_hmd_states_use_available_frame_assets() {
        use OpenvrPropKey::*;
        let properties = RefCell::new(Vec::new());
        set_icons(|key, value| properties.borrow_mut().push((key, value.to_owned())));
        let properties = properties.into_inner();
        assert_eq!(properties.len(), 9);
        for (key, suffix) in [
            (NamedIconPathDeviceOffString, "off.png"),
            (NamedIconPathDeviceStandbyString, "off.png"),
            (NamedIconPathDeviceNotReadyString, "error.png"),
            (NamedIconPathDeviceSearchingAlertString, "error.png"),
            (NamedIconPathDeviceReadyAlertString, "info.png"),
            (NamedIconPathDeviceStandbyAlertString, "info.png"),
            (NamedIconPathDeviceReadyString, "ready.png"),
            (NamedIconPathDeviceSearchingString, "searching.gif"),
            (NamedIconPathDeviceAlertLowString, "ready_low.png"),
        ] {
            assert!(properties.iter().any(|(actual, path)| *actual as u32 == key as u32 &&
                path == &format!("{{vrlink}}/icons/headset_steamframe_{suffix}")));
        }
    }
}
