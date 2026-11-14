use surfxr_session::OpenvrPropKey;

pub(super) fn set_generic_icons(base_path: &str, set_prop: impl Fn(OpenvrPropKey, &str)) {
    use OpenvrPropKey::*;
    set_prop(
        NamedIconPathDeviceOffString,
        format!("{base_path}_off.png").as_str(),
    );
    set_prop(
        NamedIconPathDeviceSearchingString,
        format!("{base_path}_searching.gif").as_str(),
    );
    set_prop(
        NamedIconPathDeviceSearchingAlertString,
        format!("{base_path}_searching_alert.gif").as_str(),
    );
    set_prop(
        NamedIconPathDeviceReadyString,
        format!("{base_path}_ready.png").as_str(),
    );
    set_prop(
        NamedIconPathDeviceReadyAlertString,
        format!("{base_path}_ready_alert.png").as_str(),
    );
    set_prop(
        NamedIconPathDeviceAlertLowString,
        format!("{base_path}_ready_low.png").as_str(),
    );
    set_prop(
        NamedIconPathDeviceStandbyString,
        format!("{base_path}_standby.png").as_str(),
    );
    set_prop(
        NamedIconPathDeviceStandbyAlertString,
        format!("{base_path}_standby_alert.gif").as_str(),
    );
}
