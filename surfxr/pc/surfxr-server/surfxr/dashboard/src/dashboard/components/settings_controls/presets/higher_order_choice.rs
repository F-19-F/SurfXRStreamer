use super::schema::{HigherOrderChoiceSchema, PresetModifierOperation};
use crate::dashboard::components::{self, NestingInfo, SettingControl};
use surfxr_common::debug;
use surfxr_packets::{PathSegment, PathValuePair};
use eframe::egui::Ui;
use serde_json as json;
use settings_schema::{SchemaEntry, SchemaNode};
use std::collections::HashSet;

pub struct Control {
    name: String,
    modifiers: Vec<(String, Vec<PathValuePair>)>,
    control: SettingControl,
    preset_json: json::Value,
}

#[cfg(all(test, not(target_arch = "wasm32")))]
mod tests {
    use super::*;
    use crate::dashboard::components::presets::{builtin_schema, schema::PresetSchemaNode};
    use surfxr_server_io::ServerSessionManager;

    #[test]
    fn specific_encoder_match_wins_independent_of_option_order_and_refresh() {
        let PresetSchemaNode::HigherOrderChoice(mut schema) =
            builtin_schema::encoder_preset_schema()
        else {
            unreachable!()
        };
        schema.options.reverse();
        let mut control = Control::new(schema);
        let mut session = ServerSessionManager::new(None);
        control
            .update_session_settings(&json::to_value(&session.session().session_settings).unwrap());
        assert_eq!(
            control.preset_json["Encoder preset"]["variant"],
            "SurfXR default"
        );
        let values = control
            .modifiers
            .iter()
            .find(|(name, _)| name == "SurfXR default")
            .unwrap()
            .1
            .clone();
        session.set_session_values(values).unwrap();
        let mut saved = json::to_value(&session.session().session_settings).unwrap();
        for _ in 0..8 {
            control.update_session_settings(&saved);
            assert_eq!(
                control.preset_json["Encoder preset"]["variant"],
                "SurfXR default"
            );
        }
        // A manual change to the expanded tuning no longer matches the SurfXR preset.
        for mode in ["QuarterResolution", "FullResolution"] {
            saved["video"]["encoder_config"]["nvenc"]["multi_pass"]["variant"] = json::json!(mode);
            control.update_session_settings(&saved);
            assert_eq!(control.preset_json["Encoder preset"]["variant"], "Custom");
        }
        saved["video"]["encoder_config"]["nvenc"]["quality_preset"]["variant"] = json::json!("P7");
        control.update_session_settings(&saved);
        assert_eq!(control.preset_json["Encoder preset"]["variant"], "Custom");
    }
}

impl Control {
    pub fn new(schema: HigherOrderChoiceSchema) -> Self {
        let modifiers = schema
            .options
            .iter()
            .map(|option| {
                (
                    option.display_name.clone(),
                    option
                        .modifiers
                        .iter()
                        .map(|modifier| match &modifier.operation {
                            PresetModifierOperation::Assign(value) => PathValuePair {
                                path: surfxr_common::show_err(surfxr_packets::parse_path(
                                    &modifier.target_path,
                                ))
                                .unwrap_or_default(),
                                value: value.clone(),
                            },
                        })
                        .collect(),
                )
            })
            .collect();

        let mut strings = schema.strings;
        strings.insert("display_name".into(), schema.name.clone());

        let control_schema = SchemaNode::Section {
            entries: vec![SchemaEntry {
                name: schema.name.clone(),
                strings,
                flags: schema.flags,
                content: SchemaNode::Choice {
                    default: schema
                        .options
                        .iter()
                        .find(|option| option.display_name == schema.default_option_display_name)
                        .unwrap()
                        .display_name
                        .clone(),
                    variants: schema
                        .options
                        .into_iter()
                        .map(|option| SchemaEntry {
                            name: option.display_name.clone(),
                            strings: [("display_name".into(), option.display_name)]
                                .into_iter()
                                .collect(),
                            flags: HashSet::new(),
                            content: None,
                        })
                        .collect(),
                    gui: Some(schema.gui),
                },
            }],
            gui_collapsible: false,
        };

        let control = SettingControl::new(
            NestingInfo {
                path: vec![],
                indentation_level: 0,
            },
            control_schema,
        );

        let preset_json = json::json!({ {&schema.name}: { "variant": "" } });

        Self {
            name: schema.name,
            modifiers,
            control,
            preset_json,
        }
    }

    pub fn update_session_settings(&mut self, session_setting_json: &json::Value) {
        let mut selected_option = String::new();
        let mut matched_fields = 0;

        'outer: for (key, descs) in &self.modifiers {
            for desc in descs {
                let mut session_ref = session_setting_json;

                // The caller supplies session_settings, so skip that root path segment.
                let Some(segments) = desc.path.get(1..) else {
                    continue 'outer;
                };
                for segment in segments {
                    let next = match segment {
                        PathSegment::Name(name) => session_ref.get(name),
                        PathSegment::Index(index) => session_ref.get(index),
                    };
                    let Some(next) = next else {
                        debug!(
                            "Preset modifier targets \"{}\", which is not in the current session",
                            surfxr_packets::path_to_string(&desc.path)
                        );
                        continue 'outer;
                    };
                    session_ref = next;
                }

                if !components::json_values_eq(session_ref, &desc.value) {
                    continue 'outer;
                }
            }

            // Prefer the most specific match. Schema order breaks ties, so a broad
            // Speed preset cannot randomly hide the matching SurfXR encoder setup.
            if selected_option.is_empty() || descs.len() > matched_fields {
                selected_option.clone_from(key);
                matched_fields = descs.len();
            }
        }

        // Note: if no modifier matched, the control will unselect all options
        self.preset_json[&self.name]["variant"] = json::Value::String(selected_option);
    }

    pub fn ui(&mut self, ui: &mut Ui) -> Vec<PathValuePair> {
        if let Some(desc) = self.control.ui(ui, &mut self.preset_json, false) {
            // todo: handle children requests
            self.modifiers
                .iter()
                .find(|(name, _)| Some(name.as_str()) == desc.value.as_str())
                .map(|(_, values)| values.clone())
                .unwrap_or_default()
        } else {
            vec![]
        }
    }
}
