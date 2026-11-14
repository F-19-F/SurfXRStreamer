//! Search only changes visibility. It never mutates collapsed state or saved settings.
use surfxr_session::settings_schema::SchemaNode;
use eframe::egui::{Id, Ui};

pub fn query(ui: &Ui) -> String {
    ui.ctx().data_mut(|data| {
        data.get_temp::<String>(Id::new("advanced_search"))
            .unwrap_or_default()
    })
}

pub fn set(ui: &Ui, query: &str) {
    ui.ctx()
        .data_mut(|data| data.insert_temp(Id::new("advanced_search"), query.to_owned()));
}

pub fn matches(text: &str, query: &str) -> bool {
    crate::i18n::search_matches(text, query)
}

pub fn schema_text(schema: &SchemaNode) -> String {
    match schema {
        SchemaNode::Section { entries, .. } => entries
            .iter()
            .filter(|e| !e.flags.contains("hidden"))
            .map(|e| {
                format!(
                    "{} {} {}",
                    e.name.replace('_', " "),
                    e.strings.values().cloned().collect::<Vec<_>>().join(" "),
                    schema_text(&e.content)
                )
            })
            .collect::<Vec<_>>()
            .join(" "),
        SchemaNode::Switch { content, .. } | SchemaNode::Optional { content, .. } => {
            schema_text(content)
        }
        SchemaNode::Choice { variants, .. } => variants
            .iter()
            .map(|e| {
                format!(
                    "{} {} {}",
                    e.name,
                    e.strings.values().cloned().collect::<Vec<_>>().join(" "),
                    e.content.as_ref().map(schema_text).unwrap_or_default()
                )
            })
            .collect::<Vec<_>>()
            .join(" "),
        _ => String::new(),
    }
    .to_lowercase()
}
