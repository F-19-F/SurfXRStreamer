//! Physical Steam Frame layout. No Touch-style cross-hand aliases.
use crate::{InteractionProfileInfo, hash_string};
use std::{collections::HashSet, sync::LazyLock};

pub const FRAME_CONTROLLER_PROFILE_PATH: &str =
    "/interaction_profiles/valve/frame_controller_valve";
pub static FRAME_CONTROLLER_PROFILE_ID: LazyLock<u64> =
    LazyLock::new(|| hash_string(FRAME_CONTROLLER_PROFILE_PATH));

pub fn frame_input_paths() -> Vec<String> {
    let mut paths = Vec::with_capacity(48);
    for hand in ["left", "right"] {
        let faces: &[&str] = if hand == "left" {
            &["dpad_up", "dpad_down", "dpad_left", "dpad_right", "view"]
        } else {
            &["a", "b", "x", "y", "menu"]
        };
        for button in
            faces
                .iter()
                .copied()
                .chain(["bumper", "trigger", "squeeze", "thumbstick", "system"])
        {
            for component in ["click", "touch"] {
                paths.push(format!("/user/hand/{hand}/input/{button}/{component}"));
            }
        }
        for component in [
            "trigger/value",
            "squeeze/value",
            "thumbstick/x",
            "thumbstick/y",
        ] {
            paths.push(format!("/user/hand/{hand}/input/{component}"));
        }
    }
    paths
}

pub static FRAME_CONTROLLER_PROFILE: LazyLock<InteractionProfileInfo> =
    LazyLock::new(|| InteractionProfileInfo {
        path: FRAME_CONTROLLER_PROFILE_PATH,
        button_set: frame_input_paths()
            .iter()
            .map(|path| hash_string(path))
            .collect::<HashSet<_>>(),
    });

#[cfg(test)]
mod tests {
    use super::*;
    use crate::BUTTON_INFO;

    #[test]
    fn all_physical_components_are_registered() {
        assert_eq!(FRAME_CONTROLLER_PROFILE.button_set.len(), 48);
        for path in frame_input_paths() {
            assert_eq!(BUTTON_INFO[&hash_string(&path)].path, path);
        }
        assert!(
            !FRAME_CONTROLLER_PROFILE
                .button_set
                .contains(&hash_string("/user/hand/left/input/x/click"))
        );
        assert!(
            FRAME_CONTROLLER_PROFILE
                .button_set
                .contains(&hash_string("/user/hand/right/input/x/touch"))
        );
    }
}
