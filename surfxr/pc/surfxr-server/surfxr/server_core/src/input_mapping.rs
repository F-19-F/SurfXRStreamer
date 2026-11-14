use surfxr_common::*;
use surfxr_packets::{ButtonEntry, ButtonValue};
use surfxr_session::{
    AutomaticButtonMappingConfig, BinaryToScalarStates, ButtonBindingTarget, ButtonMappingType,
    ControllersEmulationMode, HysteresisThreshold, Range,
};
use std::collections::{HashMap, HashSet};

pub fn registered_button_set(
    controllers_emulation_mode: &ControllersEmulationMode,
) -> HashSet<u64> {
    match &controllers_emulation_mode {
        ControllersEmulationMode::SteamFrame => FRAME_CONTROLLER_PROFILE.button_set.clone(),
        ControllersEmulationMode::RiftSTouch
        | ControllersEmulationMode::Quest1Touch
        | ControllersEmulationMode::Quest2Touch
        | ControllersEmulationMode::Quest3Plus
        | ControllersEmulationMode::QuestPro => CONTROLLER_PROFILE_INFO
            .get(&QUEST_CONTROLLER_PROFILE_ID)
            .unwrap()
            .button_set
            .clone(),
        ControllersEmulationMode::Pico4 => CONTROLLER_PROFILE_INFO
            .get(&PICO4_CONTROLLER_PROFILE_ID)
            .unwrap()
            .button_set
            .clone(),
        ControllersEmulationMode::PSVR2Sense => CONTROLLER_PROFILE_INFO
            .get(&PSVR2_CONTROLLER_PROFILE_ID)
            .unwrap()
            .button_set
            .clone(),
        ControllersEmulationMode::ValveIndex => CONTROLLER_PROFILE_INFO
            .get(&INDEX_CONTROLLER_PROFILE_ID)
            .unwrap()
            .button_set
            .clone(),
        ControllersEmulationMode::ViveWand => CONTROLLER_PROFILE_INFO
            .get(&VIVE_CONTROLLER_PROFILE_ID)
            .unwrap()
            .button_set
            .clone(),
        ControllersEmulationMode::ViveTracker => HashSet::new(),
        ControllersEmulationMode::Custom { button_set, .. } => button_set
            .iter()
            .map(|b| surfxr_common::hash_string(b))
            .collect(),
    }
}

pub struct BindingTarget {
    destination: u64,
    mapping_type: ButtonMappingType,
    binary_conditions: Vec<u64>,
}

// Inputs relative to the same physical button
#[derive(Clone, Copy)]
pub struct ButtonInputs {
    click: Option<u64>,
    touch: Option<u64>,
    value: Option<u64>,
    force: Option<u64>,
}

fn click(click: u64) -> ButtonInputs {
    ButtonInputs {
        click: Some(click),
        touch: None,
        value: None,
        force: None,
    }
}

fn ct(set: &HashSet<u64>, click: u64, touch: u64) -> ButtonInputs {
    ButtonInputs {
        click: Some(click),
        touch: set.contains(&touch).then_some(touch),
        value: None,
        force: None,
    }
}

fn value(value: u64) -> ButtonInputs {
    ButtonInputs {
        click: None,
        touch: None,
        value: Some(value),
        force: None,
    }
}

fn ctv(set: &HashSet<u64>, click: u64, touch: u64, value: u64) -> ButtonInputs {
    ButtonInputs {
        click: set.contains(&click).then_some(click),
        touch: set.contains(&touch).then_some(touch),
        value: set.contains(&value).then_some(value),
        force: None,
    }
}

fn ctvf(set: &HashSet<u64>, click: u64, touch: u64, value: u64, force: u64) -> ButtonInputs {
    ButtonInputs {
        click: set.contains(&click).then_some(click),
        touch: set.contains(&touch).then_some(touch),
        value: set.contains(&value).then_some(value),
        force: set.contains(&force).then_some(force),
    }
}

fn passthrough(target: u64) -> BindingTarget {
    BindingTarget {
        destination: target,
        mapping_type: ButtonMappingType::Passthrough,
        binary_conditions: vec![],
    }
}

fn binary_to_scalar(target: u64, map: BinaryToScalarStates) -> BindingTarget {
    BindingTarget {
        destination: target,
        mapping_type: ButtonMappingType::BinaryToScalar(map),
        binary_conditions: vec![],
    }
}

fn hysteresis_threshold(target: u64, map: HysteresisThreshold) -> BindingTarget {
    BindingTarget {
        destination: target,
        mapping_type: ButtonMappingType::HysteresisThreshold(map),
        binary_conditions: vec![],
    }
}

fn remap(target: u64, map: Range) -> BindingTarget {
    BindingTarget {
        destination: target,
        mapping_type: ButtonMappingType::Remap(map),
        binary_conditions: vec![],
    }
}

// Map two buttons with eterogeneous inputs
fn map_button_pair_automatic(
    source: ButtonInputs,
    destination: ButtonInputs,
    config: &AutomaticButtonMappingConfig,
) -> impl Iterator<Item = (u64, Vec<BindingTarget>)> {
    let click_to_value = BinaryToScalarStates { off: 0.0, on: 1.0 };

    let mut entries = vec![];
    if let Some(source_click) = source.click {
        let mut targets = vec![];

        if let Some(destination_click) = destination.click {
            targets.push(passthrough(destination_click));
        }
        if source.touch.is_none()
            && let Some(destination_touch) = destination.touch
        {
            targets.push(passthrough(destination_touch));
        }

        if source.value.is_none()
            && let Some(destination_value) = destination.value
        {
            targets.push(binary_to_scalar(destination_value, click_to_value));
        }

        entries.push((source_click, targets));
    }
    if let Some(source_touch) = source.touch {
        let mut targets = vec![];
        if let Some(destination_touch) = destination.touch {
            targets.push(passthrough(destination_touch));
        }
        entries.push((source_touch, targets));
    }
    if let Some(source_value) = source.value {
        let mut targets = vec![];
        let mut remap_for_touch = false;
        let mut remap_for_force = false;

        if source.click.is_none()
            && let Some(destination_click) = destination.click
        {
            targets.push(hysteresis_threshold(
                destination_click,
                config.click_threshold,
            ));
        }
        if source.touch.is_none()
            && let Some(destination_touch) = destination.touch
        {
            targets.push(hysteresis_threshold(
                destination_touch,
                config.touch_threshold,
            ));
            remap_for_touch = true;
        }
        if source.force.is_none()
            && let Some(destination_force) = destination.force
        {
            targets.push(remap(
                destination_force,
                Range {
                    min: config.force_threshold,
                    max: 1.0,
                },
            ));
            remap_for_force = true;
        }

        if let Some(destination_value) = destination.value {
            if !remap_for_touch && !remap_for_force {
                targets.push(passthrough(destination_value));
            } else {
                let low = if remap_for_touch {
                    config.touch_threshold.value
                } else {
                    0.0
                };
                let high = if remap_for_force {
                    config.force_threshold
                } else {
                    1.0
                };
                targets.push(remap(
                    destination_value,
                    Range {
                        min: low,
                        max: high,
                    },
                ));
            }
        }

        entries.push((source_value, targets));
    }

    entries.into_iter()
}

#[path = "automatic_mapping.rs"]
mod automatic_mapping;
use automatic_mapping::automatic_bindings;
pub struct ButtonMappingManager {
    mappings: HashMap<u64, Vec<BindingTarget>>,
    binary_source_states: HashMap<u64, bool>,
    hysteresis_states: HashMap<u64, HashMap<u64, bool>>,
}

impl ButtonMappingManager {
    pub fn new_automatic(
        source: &HashSet<u64>,
        controllers_emulation_mode: &ControllersEmulationMode,
        button_mapping_config: &AutomaticButtonMappingConfig,
    ) -> Self {
        let button_set = registered_button_set(controllers_emulation_mode);
        let mappings = if matches!(controllers_emulation_mode, ControllersEmulationMode::SteamFrame) {
            source.intersection(&button_set).map(|id| (*id, vec![passthrough(*id)])).collect()
        } else {
            automatic_bindings(source, &button_set, button_mapping_config)
        };
        Self {
            mappings,
            binary_source_states: HashMap::new(),
            hysteresis_states: HashMap::new(),
        }
    }

    pub fn new_manual(mappings: &[(String, Vec<ButtonBindingTarget>)]) -> Self {
        let mappings = mappings
            .iter()
            .map(|(key, value)| {
                (
                    surfxr_common::hash_string(key),
                    value
                        .iter()
                        .map(|b| BindingTarget {
                            destination: surfxr_common::hash_string(&b.destination),
                            mapping_type: b.mapping_type.clone(),
                            binary_conditions: b
                                .binary_conditions
                                .iter()
                                .map(|c| surfxr_common::hash_string(c))
                                .collect(),
                        })
                        .collect(),
                )
            })
            .collect();

        Self {
            mappings,
            binary_source_states: HashMap::new(),
            hysteresis_states: HashMap::new(),
        }
    }

    // Apply any button changes that are mapped to this specific button
    pub fn map_button(&mut self, source_button: &ButtonEntry) -> Vec<ButtonEntry> {
        if let ButtonValue::Binary(value) = source_button.value {
            let val_ref = self
                .binary_source_states
                .entry(source_button.path_id)
                .or_default();

            if value == *val_ref {
                return vec![];
            }

            // NB: Update value
            *val_ref = value;
        }

        let mut destination_buttons = vec![];

        if let Some(mappings) = self.mappings.get(&source_button.path_id) {
            'mapping: for mapping in mappings {
                let destination_value = match (&mapping.mapping_type, source_button.value) {
                    (ButtonMappingType::Passthrough, value) => value,
                    (
                        ButtonMappingType::HysteresisThreshold(threshold),
                        ButtonValue::Scalar(value),
                    ) => {
                        let state = self
                            .hysteresis_states
                            .entry(source_button.path_id)
                            .or_default()
                            .entry(mapping.destination)
                            .or_default();

                        if *state && value < threshold.value - threshold.deviation {
                            *state = false;
                        } else if !*state && value > threshold.value + threshold.deviation {
                            *state = true;
                        } else {
                            // No change needed
                            continue;
                        }

                        ButtonValue::Binary(*state)
                    }
                    (ButtonMappingType::BinaryToScalar(levels), ButtonValue::Binary(value)) => {
                        if value {
                            ButtonValue::Scalar(levels.on)
                        } else {
                            ButtonValue::Scalar(levels.off)
                        }
                    }
                    (ButtonMappingType::Remap(range), ButtonValue::Scalar(value)) => {
                        let value = (value - range.min) / (range.max - range.min);
                        ButtonValue::Scalar(value.clamp(0.0, 1.0))
                    }
                    _ => {
                        error!("Failed to map button!");
                        continue;
                    }
                };

                for source_id in &mapping.binary_conditions {
                    if !self
                        .binary_source_states
                        .get(source_id)
                        .copied()
                        .unwrap_or(false)
                    {
                        continue 'mapping;
                    }
                }

                destination_buttons.push(ButtonEntry {
                    path_id: mapping.destination,
                    value: destination_value,
                });
            }
        } else {
            let button_name = BUTTON_INFO
                .get(&source_button.path_id)
                .map_or("Unknown", |info| info.path);
            info!("Received button not mapped: {button_name}");
        }

        destination_buttons
    }
}
