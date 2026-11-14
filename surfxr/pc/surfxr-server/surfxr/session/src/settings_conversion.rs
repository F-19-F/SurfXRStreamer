//! Convert the complete saved schema representation into runtime settings.
use serde_json as json;
use settings_schema::SchemaNode;

pub(super) fn json_session_settings_to_settings(
    session_settings: &json::Value,
    schema: &SchemaNode,
) -> json::Value {
    match schema {
        SchemaNode::Section { entries, .. } => json::Value::Object(
            entries
                .iter()
                .map(|named_entry| {
                    (
                        named_entry.name.clone(),
                        json_session_settings_to_settings(
                            &session_settings[&named_entry.name],
                            &named_entry.content,
                        ),
                    )
                })
                .collect(),
        ),

        SchemaNode::Choice { variants, .. } => {
            let variant = session_settings["variant"].as_str().unwrap();
            let maybe_content = variants
                .iter()
                .find(|named_entry| named_entry.name == variant)
                .and_then(|named_entry| named_entry.content.as_ref())
                .map(|data_schema| {
                    json_session_settings_to_settings(&session_settings[variant], data_schema)
                });
            if let Some(content) = maybe_content {
                json::json!({ variant: content })
            } else {
                json::Value::String(variant.to_owned())
            }
        }

        SchemaNode::Optional { content, .. } => {
            if session_settings["set"].as_bool().unwrap() {
                json_session_settings_to_settings(&session_settings["content"], content)
            } else {
                json::Value::Null
            }
        }

        SchemaNode::Switch { content, .. } => {
            if session_settings["enabled"].as_bool().unwrap() {
                let content =
                    json_session_settings_to_settings(&session_settings["content"], content);

                json::json!({ "Enabled": content })
            } else {
                json::Value::String("Disabled".into())
            }
        }

        SchemaNode::Boolean { .. } | SchemaNode::Number { .. } | SchemaNode::Text { .. } => {
            session_settings.clone()
        }

        SchemaNode::Array(array_schema) => json::Value::Array(
            array_schema
                .iter()
                .enumerate()
                .map(|(idx, element_schema)| {
                    json_session_settings_to_settings(
                        &session_settings["content"][idx],
                        element_schema,
                    )
                })
                .collect(),
        ),

        SchemaNode::Vector {
            default_element, ..
        } => json::to_value(
            session_settings["content"]
                .as_array()
                .unwrap()
                .iter()
                .map(|element_json| {
                    json_session_settings_to_settings(element_json, default_element)
                })
                .collect::<Vec<_>>(),
        )
        .unwrap(),

        SchemaNode::Dictionary { default_value, .. } => json::to_value(
            json::from_value::<Vec<(String, json::Value)>>(session_settings["content"].clone())
                .unwrap()
                .into_iter()
                .map(|(key, value_json)| {
                    (
                        key,
                        json_session_settings_to_settings(&value_json, default_value),
                    )
                })
                .collect::<Vec<_>>(),
        )
        .unwrap(),
        _ => unreachable!(),
    }
}
