use crate::input_mapping::ButtonMappingManager;
use surfxr_common::{
    glam::{Vec2, Vec3},
    *,
};
use surfxr_packets::{ButtonEntry, ButtonValue};
use surfxr_session::HandTrackingInteractionConfig;
use std::{
    collections::{HashMap, HashSet},
    hash::Hash,
    sync::LazyLock,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

fn lerp_pose(a: Pose, b: Pose, fac: f32) -> Pose {
    Pose {
        orientation: a.orientation.lerp(b.orientation, fac),
        position: a.position.lerp(b.position, fac),
    }
}

pub static HAND_GESTURE_BUTTON_SET: LazyLock<HashSet<u64>> = LazyLock::new(|| {
    HashSet::from([
        *LEFT_X_CLICK_ID,
        *LEFT_X_TOUCH_ID,
        *LEFT_Y_CLICK_ID,
        *LEFT_Y_TOUCH_ID,
        *LEFT_MENU_CLICK_ID,
        *LEFT_SQUEEZE_VALUE_ID,
        *LEFT_TRIGGER_VALUE_ID,
        *LEFT_TRIGGER_TOUCH_ID,
        *LEFT_THUMBSTICK_X_ID,
        *LEFT_THUMBSTICK_Y_ID,
        *LEFT_THUMBSTICK_CLICK_ID,
        *LEFT_THUMBSTICK_TOUCH_ID,
        *RIGHT_A_CLICK_ID,
        *RIGHT_A_TOUCH_ID,
        *RIGHT_B_CLICK_ID,
        *RIGHT_B_TOUCH_ID,
        *RIGHT_SQUEEZE_VALUE_ID,
        *RIGHT_TRIGGER_VALUE_ID,
        *RIGHT_TRIGGER_TOUCH_ID,
        *RIGHT_THUMBSTICK_X_ID,
        *RIGHT_THUMBSTICK_Y_ID,
        *RIGHT_THUMBSTICK_CLICK_ID,
        *RIGHT_THUMBSTICK_TOUCH_ID,
    ])
});

#[derive(Debug, Clone)]
pub struct HandGesture {
    pub id: HandGestureId,
    pub active: bool,
    pub clicked: bool,
    pub touching: bool,
    pub value: f32,
}

pub struct GestureAction {
    last_activated: u128,
    last_deactivated: u128,
    entering: bool,
    entering_since: u128,
    exiting: bool,
    exiting_since: u128,
    active: bool,
}

#[derive(Debug, Clone, Copy, Eq, Hash, PartialEq)]
#[allow(dead_code)]
pub enum HandGestureId {
    // Pinches
    ThumbIndexPinch,
    ThumbMiddlePinch,
    ThumbRingPinch,
    ThumbLittlePinch,
    // Curls
    ThumbCurl,
    IndexCurl,
    MiddleCurl,
    RingCurl,
    LittleCurl,
    GripCurl,
    // Complex
    JoystickX,
    JoystickY,
}

pub struct HandGestureManager {
    gesture_data_left: HashMap<HandGestureId, GestureAction>,
    gesture_data_right: HashMap<HandGestureId, GestureAction>,
}

#[path = "gesture_detection.rs"]
mod gesture_detection;
fn get_click_bind_for_gesture(device_id: u64, gesture_id: HandGestureId) -> Option<u64> {
    if device_id == *HAND_LEFT_ID {
        match gesture_id {
            HandGestureId::ThumbIndexPinch => Some(*LEFT_TRIGGER_CLICK_ID),
            HandGestureId::ThumbMiddlePinch => Some(*LEFT_Y_CLICK_ID),
            HandGestureId::ThumbRingPinch => Some(*LEFT_X_CLICK_ID),
            HandGestureId::ThumbLittlePinch => Some(*LEFT_MENU_CLICK_ID),
            HandGestureId::GripCurl => Some(*LEFT_SQUEEZE_CLICK_ID),
            HandGestureId::ThumbCurl => Some(*LEFT_THUMBSTICK_CLICK_ID),
            _ => None,
        }
    } else {
        match gesture_id {
            HandGestureId::ThumbIndexPinch => Some(*RIGHT_TRIGGER_CLICK_ID),
            HandGestureId::ThumbMiddlePinch => Some(*RIGHT_B_CLICK_ID),
            HandGestureId::ThumbRingPinch => Some(*RIGHT_A_CLICK_ID),
            HandGestureId::GripCurl => Some(*RIGHT_SQUEEZE_CLICK_ID),
            HandGestureId::ThumbCurl => Some(*RIGHT_THUMBSTICK_CLICK_ID),
            _ => None,
        }
    }
}

fn get_touch_bind_for_gesture(device_id: u64, gesture_id: HandGestureId) -> Option<u64> {
    if device_id == *HAND_LEFT_ID {
        match gesture_id {
            HandGestureId::ThumbIndexPinch => Some(*LEFT_TRIGGER_TOUCH_ID),
            HandGestureId::ThumbMiddlePinch => Some(*LEFT_Y_TOUCH_ID),
            HandGestureId::ThumbRingPinch => Some(*LEFT_X_TOUCH_ID),
            HandGestureId::JoystickX | HandGestureId::JoystickY => Some(*LEFT_THUMBSTICK_TOUCH_ID),
            _ => None,
        }
    } else {
        match gesture_id {
            HandGestureId::ThumbIndexPinch => Some(*RIGHT_TRIGGER_TOUCH_ID),
            HandGestureId::ThumbMiddlePinch => Some(*RIGHT_B_TOUCH_ID),
            HandGestureId::ThumbRingPinch => Some(*RIGHT_A_TOUCH_ID),
            HandGestureId::JoystickX | HandGestureId::JoystickY => Some(*RIGHT_THUMBSTICK_TOUCH_ID),
            _ => None,
        }
    }
}

fn get_hover_bind_for_gesture(device_id: u64, gesture_id: HandGestureId) -> Option<u64> {
    if device_id == *HAND_LEFT_ID {
        match gesture_id {
            HandGestureId::ThumbIndexPinch => Some(*LEFT_TRIGGER_VALUE_ID),
            HandGestureId::GripCurl => Some(*LEFT_SQUEEZE_VALUE_ID),
            HandGestureId::JoystickX => Some(*LEFT_THUMBSTICK_X_ID),
            HandGestureId::JoystickY => Some(*LEFT_THUMBSTICK_Y_ID),
            _ => None,
        }
    } else {
        match gesture_id {
            HandGestureId::ThumbIndexPinch => Some(*RIGHT_TRIGGER_VALUE_ID),
            HandGestureId::GripCurl => Some(*RIGHT_SQUEEZE_VALUE_ID),
            HandGestureId::JoystickX => Some(*RIGHT_THUMBSTICK_X_ID),
            HandGestureId::JoystickY => Some(*RIGHT_THUMBSTICK_Y_ID),
            _ => None,
        }
    }
}

pub fn trigger_hand_gesture_actions(
    button_mapping_manager: &mut ButtonMappingManager,
    device_id: u64,
    gestures: &[HandGesture],
    only_touch: bool,
) -> Vec<ButtonEntry> {
    let mut button_entries = vec![];

    for gesture in gestures {
        // Click bind
        if !only_touch && let Some(click_bind) = get_click_bind_for_gesture(device_id, gesture.id) {
            button_entries.append(&mut button_mapping_manager.map_button(&ButtonEntry {
                path_id: click_bind,
                value: ButtonValue::Binary(gesture.active && gesture.clicked),
            }));
        }

        // Touch bind
        if let Some(touch_bind) = get_touch_bind_for_gesture(device_id, gesture.id) {
            button_entries.append(&mut button_mapping_manager.map_button(&ButtonEntry {
                path_id: touch_bind,
                value: ButtonValue::Binary(gesture.active && gesture.touching),
            }));
        }

        // Hover bind
        if !only_touch && let Some(hover_bind) = get_hover_bind_for_gesture(device_id, gesture.id) {
            button_entries.append(&mut button_mapping_manager.map_button(&ButtonEntry {
                path_id: hover_bind,
                value: ButtonValue::Scalar(if gesture.active { gesture.value } else { 0.0 }),
            }));
        }
    }

    button_entries
}
