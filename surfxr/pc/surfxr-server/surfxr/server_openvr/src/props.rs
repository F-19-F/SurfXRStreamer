// Note: many properties are missing or are stubs.
// todo: fill out more properties for headset and controllers
// todo: add more emulation modes

#[path = "frame_controller_props.rs"]
mod frame_controller;
#[path = "frame_headset_props.rs"]
mod frame_headset;
#[path = "device_props.rs"]
mod device;
#[path = "device_icons.rs"]
mod device_icons;
pub use device::set_device_openvr_props;

use crate::{
    FfiOpenvrProperty, FfiOpenvrPropertyType_Bool, FfiOpenvrPropertyType_Double,
    FfiOpenvrPropertyType_Float, FfiOpenvrPropertyType_Int32, FfiOpenvrPropertyType_String,
    FfiOpenvrPropertyType_Uint64, FfiOpenvrPropertyType_Vector3, FfiOpenvrPropertyValue,
};
use surfxr_common::{
    BODY_CHEST_ID, BODY_HIPS_ID, BODY_LEFT_ELBOW_ID, BODY_LEFT_FOOT_ID, BODY_LEFT_KNEE_ID,
    BODY_RIGHT_ELBOW_ID, BODY_RIGHT_FOOT_ID, BODY_RIGHT_KNEE_ID, DEVICE_ID_TO_PATH, HAND_LEFT_ID,
    HAND_RIGHT_ID, HAND_TRACKER_LEFT_ID, HAND_TRACKER_RIGHT_ID, HEAD_ID, debug,
    settings_schema::Switch, warn,
};
use surfxr_session::{
    ControllersEmulationMode, HeadsetEmulationMode, OpenvrPropKey, OpenvrPropType, OpenvrProperty,
};
use std::{
    ffi::{CString, c_char, c_void},
    ptr,
};

pub fn set_openvr_prop(instance_ptr: Option<*mut c_void>, device_id: u64, prop: OpenvrProperty) {
    let key = prop.key as u32;
    let ty = surfxr_session::openvr_prop_key_to_type(prop.key);
    let value = prop.value;

    let device_name = DEVICE_ID_TO_PATH.get(&device_id).unwrap_or(&"Unknown");

    let (type_, maybe_value) = match ty {
        OpenvrPropType::Bool => (
            FfiOpenvrPropertyType_Bool,
            value
                .parse::<bool>()
                .ok()
                .map(|bool_| FfiOpenvrPropertyValue {
                    bool_: bool_.into(),
                }),
        ),
        OpenvrPropType::Float => (
            FfiOpenvrPropertyType_Float,
            value
                .parse::<f32>()
                .ok()
                .map(|float_| FfiOpenvrPropertyValue { float_ }),
        ),
        OpenvrPropType::Int32 => (
            FfiOpenvrPropertyType_Int32,
            value
                .parse::<i32>()
                .ok()
                .map(|int32| FfiOpenvrPropertyValue { int32 }),
        ),
        OpenvrPropType::Uint64 => (
            FfiOpenvrPropertyType_Uint64,
            value
                .parse::<u64>()
                .ok()
                .map(|uint64| FfiOpenvrPropertyValue { uint64 }),
        ),
        OpenvrPropType::Vector3 => (
            FfiOpenvrPropertyType_Vector3,
            serde_json::from_str::<[f32; 3]>(&value)
                .ok()
                .map(|vector3| FfiOpenvrPropertyValue { vector3 }),
        ),
        OpenvrPropType::Double => (
            FfiOpenvrPropertyType_Double,
            value
                .parse::<f64>()
                .ok()
                .map(|double_| FfiOpenvrPropertyValue { double_ }),
        ),
        OpenvrPropType::String => {
            let c_string = CString::new(value.clone()).unwrap();
            let mut string = [0; 256];

            unsafe {
                ptr::copy_nonoverlapping(
                    c_string.as_ptr(),
                    string.as_mut_ptr(),
                    c_string.as_bytes_with_nul().len(),
                );
            }

            (
                FfiOpenvrPropertyType_String,
                Some(FfiOpenvrPropertyValue { string }),
            )
        }
    };

    let Some(ffi_value) = maybe_value else {
        warn!(
            "Failed to parse {device_name} value for OpenVR property: {:?}={value}",
            prop.key
        );

        return;
    };

    debug!("Setting {device_name} OpenVR prop: {:?}={value}", prop.key);

    let ffi_prop = FfiOpenvrProperty {
        key,
        type_,
        value: ffi_value,
    };

    if let Some(instance_ptr) = instance_ptr {
        unsafe { crate::SetOpenvrProperty(instance_ptr, ffi_prop) }
    } else {
        unsafe { crate::SetOpenvrPropByDeviceID(device_id, ffi_prop) }
    }
}

fn serial_number(device_id: u64) -> String {
    let settings = surfxr_server_core::settings();

    if device_id == *HEAD_ID {
        match &settings.headset.emulation_mode {
            HeadsetEmulationMode::SteamFrame => "SURFXR_SteamFrame_HMD".into(),
            HeadsetEmulationMode::RiftS => "1WMGH000XX0000".into(),
            HeadsetEmulationMode::Quest1 => "1PASH0X0X00000".into(),
            HeadsetEmulationMode::Quest2 => "1WMHH000X00000".into(),
            HeadsetEmulationMode::QuestPro => "230YC0XXXX00XX".into(),
            HeadsetEmulationMode::Pico4 => "VRLINKHMDPICO4".into(),
            HeadsetEmulationMode::Vive => "HTCVive-001".into(),
            HeadsetEmulationMode::Custom { serial_number, .. } => serial_number.clone(),
        }
    } else if device_id == *HAND_LEFT_ID || device_id == *HAND_RIGHT_ID {
        if let Switch::Enabled(controllers) = &settings.headset.controllers {
            let serial_number = match &controllers.emulation_mode {
                ControllersEmulationMode::Quest1Touch => "1PALCXXXX00000_Controller", // 1PALCLC left, 1PALCRC right
                ControllersEmulationMode::Quest2Touch => "1WMHH000X00000_Controller",
                ControllersEmulationMode::Quest3Plus => "2G0YXX0X0000XX_Controller", // 2G0YY Left 2G0YZ Right
                ControllersEmulationMode::QuestPro => "230YXXXXXXXXXX_Controller", // 230YT left, 230YV right
                ControllersEmulationMode::SteamFrame => "SURFXR_SteamFrame_Controller",
                ControllersEmulationMode::RiftSTouch
                | ControllersEmulationMode::Pico4
                | ControllersEmulationMode::PSVR2Sense
                | ControllersEmulationMode::ValveIndex
                | ControllersEmulationMode::ViveWand
                | ControllersEmulationMode::ViveTracker => "SURFXR Remote Controller",
                ControllersEmulationMode::Custom { serial_number, .. } => serial_number,
            };

            if device_id == *HAND_LEFT_ID {
                format!("{serial_number}_Left")
            } else {
                format!("{serial_number}_Right")
            }
        } else {
            "Unknown".into()
        }
    } else if device_id == *HAND_TRACKER_LEFT_ID {
        "SURFXR_Left_Hand_Full_Skeletal".into()
    } else if device_id == *HAND_TRACKER_RIGHT_ID {
        "SURFXR_Right_Hand_Full_Skeletal".into()
    } else if device_id == *BODY_CHEST_ID {
        "SURFXR Tracker (chest)".into()
    } else if device_id == *BODY_HIPS_ID {
        "SURFXR Tracker (waist)".into()
    } else if device_id == *BODY_LEFT_ELBOW_ID {
        "SURFXR Tracker (left elbow)".into()
    } else if device_id == *BODY_RIGHT_ELBOW_ID {
        "SURFXR Tracker (right elbow)".into()
    } else if device_id == *BODY_LEFT_KNEE_ID {
        "SURFXR Tracker (left knee)".into()
    } else if device_id == *BODY_RIGHT_KNEE_ID {
        "SURFXR Tracker (right knee)".into()
    } else if device_id == *BODY_LEFT_FOOT_ID {
        "SURFXR Tracker (left foot)".into()
    } else if device_id == *BODY_RIGHT_FOOT_ID {
        "SURFXR Tracker (right foot)".into()
    } else {
        "Unknown".into()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn get_serial_number(device_id: u64, out_str: *mut c_char) -> u64 {
    let string = serial_number(device_id);

    let cstring = CString::new(string).unwrap();

    let len = cstring.to_bytes_with_nul().len();

    if !out_str.is_null() {
        unsafe { ptr::copy_nonoverlapping(cstring.as_ptr(), out_str, len) };
    }

    len as u64
}
