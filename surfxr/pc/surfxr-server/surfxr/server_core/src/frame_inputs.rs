//! Extension acknowledgement is tied to the registered SteamVR controller layout.
use surfxr_session::ControllersEmulationMode;

pub fn negotiate(
    capabilities: &str,
    extensions: &str,
    mode: Option<&ControllersEmulationMode>,
) -> String {
    let supported = serde_json::from_str::<serde_json::Value>(capabilities)
        .ok()
        .is_some_and(|value| value["frame_controller_inputs"].as_u64() == Some(1));
    if supported && matches!(mode, Some(ControllersEmulationMode::SteamFrame)) {
        let mut value: serde_json::Value =
            serde_json::from_str(extensions).unwrap_or_else(|_| serde_json::json!({}));
        value["frame_controller_inputs"] = serde_json::json!(1);
        if serde_json::from_str::<serde_json::Value>(capabilities)
            .ok()
            .is_some_and(|caps| caps["frame_controller_skeletons"].as_u64() == Some(1))
        {
            value["frame_controller_skeletons"] = serde_json::json!(1);
        }
        if crate::frame_controller_state::supported(capabilities, mode) {
            value["frame_controller_state"] = serde_json::json!(1);
        }
        value.to_string()
    } else {
        extensions.to_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_common::{frame_input_paths, hash_string};
    use surfxr_packets::{ButtonEntry, ButtonValue};
    use surfxr_session::{AutomaticButtonMappingConfig, HysteresisThreshold};

    #[test]
    fn missing_controller_motion_disconnects_without_reusing_history() {
        use surfxr_common::{DeviceMotion, HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID};
        use std::time::Duration;
        let settings = surfxr_session::SessionConfig::default().to_settings();
        let mut manager = crate::tracking::TrackingManager::new(8);
        let first = Duration::from_millis(1);
        let off = Duration::from_millis(2);
        let wake = Duration::from_millis(3);
        let head = (*HEAD_ID, DeviceMotion::default());
        let left = (*HAND_LEFT_ID, DeviceMotion::default());
        let right = (*HAND_RIGHT_ID, DeviceMotion::default());
        manager.report_device_motions(&settings.headset, first, &[head, left, right]);
        assert!(manager.get_device_motion(*HAND_LEFT_ID, first).is_some());
        // This is the exact get_device_motion call used by the OpenVR event
        // owner: None becomes a null controllerMotion/deviceIsConnected=false.
        manager.report_device_motions(&settings.headset, off, &[head, right]);
        assert!(manager.get_device_motion(*HAND_LEFT_ID, off).is_none());
        assert!(manager.get_device_motion(*HEAD_ID, off).is_some());
        assert!(manager.get_device_motion(*HAND_RIGHT_ID, off).is_some());
        manager.report_device_motions(&settings.headset, wake, &[head, left, right]);
        assert!(manager.get_device_motion(*HAND_LEFT_ID, wake).is_some());
    }

    #[test]
    fn frame_components_keep_their_hand_and_release_state() {
        let paths = frame_input_paths();
        let source = paths.iter().map(|p| hash_string(p)).collect();
        let config = AutomaticButtonMappingConfig {
            click_threshold: HysteresisThreshold {
                value: 0.5,
                deviation: 0.1,
            },
            touch_threshold: HysteresisThreshold {
                value: 0.2,
                deviation: 0.1,
            },
            force_threshold: 0.5,
        };
        let mut manager = crate::input_mapping::ButtonMappingManager::new_automatic(
            &source,
            &ControllersEmulationMode::SteamFrame,
            &config,
        );
        assert_eq!(paths.len(), 48);
        for path in paths {
            let binary = path.ends_with("/click") || path.ends_with("/touch");
            for pressed in [true, false] {
                let input = ButtonEntry {
                    path_id: hash_string(&path),
                    value: if binary {
                        ButtonValue::Binary(pressed)
                    } else {
                        ButtonValue::Scalar(if pressed { 0.25 } else { 0.0 })
                    },
                };
                let output = manager.map_button(&input);
                assert_eq!(output.len(), 1, "{path}");
                assert_eq!(output[0].path_id, input.path_id, "{path}");
                match (output[0].value, input.value) {
                    (ButtonValue::Binary(a), ButtonValue::Binary(b)) => assert_eq!(a, b),
                    (ButtonValue::Scalar(a), ButtonValue::Scalar(b)) => assert_eq!(a, b),
                    _ => panic!("component type changed: {path}"),
                }
            }
        }
    }
    #[test]
    fn native_layout_requires_both_ends() {
        let capability = r#"{"frame_controller_inputs":1}"#;
        let extensions = r#"{"frame_video_haptics":1}"#;
        assert_eq!(negotiate(capability, extensions, None), extensions);
        assert_eq!(
            negotiate(
                capability,
                extensions,
                Some(&ControllersEmulationMode::ValveIndex)
            ),
            extensions
        );
        assert_eq!(
            negotiate(
                "{}",
                extensions,
                Some(&ControllersEmulationMode::SteamFrame)
            ),
            extensions
        );
        let result: serde_json::Value = serde_json::from_str(&negotiate(
            capability,
            extensions,
            Some(&ControllersEmulationMode::SteamFrame),
        ))
        .unwrap();
        assert_eq!(result["frame_controller_inputs"], 1);
        assert_eq!(result["frame_video_haptics"], 1);
        assert!(result.get("frame_controller_skeletons").is_none());
        let result: serde_json::Value = serde_json::from_str(&negotiate(
            r#"{"frame_controller_inputs":1,"frame_controller_skeletons":1}"#,
            extensions,
            Some(&ControllersEmulationMode::SteamFrame),
        ))
        .unwrap();
        assert_eq!(result["frame_controller_skeletons"], 1);
    }
}
