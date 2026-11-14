//! UI edits select one encoder path atomically through the existing session writer.
use surfxr_packets::{PathValuePair, parse_path, path_to_string};

const GAZE: &str = "session_settings.video.frame_gaze.enabled";
const FIXED: &str = "session_settings.video.foveated_encoding.enabled";

pub fn update_guidance(schema: &mut settings_schema::SchemaNode) {
    use settings_schema::SchemaNode;
    let SchemaNode::Section { entries, .. } = schema else {
        return;
    };
    let video = entries.iter_mut().find(|e| e.name == "video").unwrap();
    let SchemaNode::Section { entries, .. } = &mut video.content else {
        return;
    };
    let fixed = entries
        .iter_mut()
        .find(|e| e.name == "foveated_encoding")
        .unwrap();
    fixed.strings.insert("notice".into(), "Selecting fixed foveation turns off Dynamic Foveated Streaming. Enabling Dynamic Foveated Streaming turns off fixed foveation; its quality values are retained.".into());
}

pub fn make_exclusive(changes: &mut Vec<PathValuePair>) {
    // Presets, ordinary switches and reset buttons all use these original paths.
    // Keep geometry/quality values when disabling the other encoder path.
    let winner = changes.iter().rev().find_map(|change| {
        let path = path_to_string(&change.path);
        (change.value == true && (path == GAZE || path == FIXED)).then_some(path)
    });
    if let Some(winner) = winner {
        let other = if winner == GAZE { FIXED } else { GAZE };
        changes.push(PathValuePair {
            path: parse_path(other).unwrap(),
            value: false.into(),
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_server_io::ServerSessionManager;
    #[test]
    fn either_encoder_is_selected_in_one_accepted_update_and_keeps_quality() {
        let mut session = ServerSessionManager::new(None);
        let original = serde_json::to_value(&session.session().session_settings).unwrap();
        for (winner, loser) in [(FIXED, GAZE), (GAZE, FIXED)] {
            let revision = session.session().frame_settings_sync.revision;
            let mut changes = vec![PathValuePair {
                path: parse_path(winner).unwrap(),
                value: true.into(),
            }];
            make_exclusive(&mut changes);
            session.set_session_values(changes).unwrap();
            let saved = serde_json::to_value(session.session()).unwrap();
            assert_eq!(
                saved.pointer(&format!("/{}", winner.replace('.', "/"))),
                Some(&true.into())
            );
            assert_eq!(
                saved.pointer(&format!("/{}", loser.replace('.', "/"))),
                Some(&false.into())
            );
            assert_eq!(
                saved["session_settings"]["video"]["foveated_encoding"]["content"],
                original["video"]["foveated_encoding"]["content"]
            );
            assert_eq!(session.session().frame_settings_sync.revision, revision + 1);
        }
    }
}
