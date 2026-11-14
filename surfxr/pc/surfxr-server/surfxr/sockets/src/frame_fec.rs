//! Negotiated video-only XOR repair. Legacy clients keep their exact UDP bytes.
use surfxr_common::anyhow::Result;
use serde_json::{Value, json};

pub fn negotiate_frame_video_fec(capabilities: &str, transport_ext: &str) -> Result<(String, bool)> {
    let caps: Value = serde_json::from_str(capabilities).unwrap_or(Value::Null);
    let mut ack: Value = serde_json::from_str(transport_ext)?;
    let enabled = caps.get("frame_video_fec").and_then(Value::as_u64) == Some(1)
        && ack["frame_split_streams_udp"]["version"].as_u64() == Some(1);
    if enabled {
        ack["frame_video_fec"] = json!({"version": 1, "data_shards": 16});
    }
    Ok((ack.to_string(), enabled))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn repair_requires_explicit_udp_negotiation() {
        let udp = r#"{"frame_split_streams_udp":{"version":1,"priority_port":19945}}"#;
        for caps in ["", "opaque", "{}", r#"{"frame_video_fec":2}"#] {
            assert!(!negotiate_frame_video_fec(caps, udp).unwrap().1);
        }
        let caps = r#"{"frame_video_fec":1}"#;
        assert!(!negotiate_frame_video_fec(caps, "{}").unwrap().1);
        assert!(!negotiate_frame_video_fec(caps, r#"{"frame_split_streams":{"version":1}}"#).unwrap().1);
        let (ack, enabled) = negotiate_frame_video_fec(caps, udp).unwrap();
        assert!(enabled);
        let ack: Value = serde_json::from_str(&ack).unwrap();
        assert_eq!(ack["frame_split_streams_udp"]["priority_port"], 19945);
        assert_eq!(ack["frame_video_fec"]["data_shards"], 16);
    }
}
