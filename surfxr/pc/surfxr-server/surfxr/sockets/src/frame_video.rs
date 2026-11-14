//! Negotiated source metadata, outside the encoded video bitstream.
use surfxr_common::anyhow::{Result, ensure};
use surfxr_packets::VideoPacketHeader;
use serde::{Serialize, Serializer, ser::SerializeTuple};
use serde_json::Value;

pub fn negotiate_frame_video_haptics(
    capabilities: &str,
    transport_ext: &str,
) -> Result<(String, bool)> {
    let caps: Value = serde_json::from_str(capabilities).unwrap_or(Value::Null);
    if caps.get("frame_video_haptics").and_then(Value::as_u64) != Some(1) {
        return Ok((transport_ext.into(), false));
    }
    let mut ack: Value = serde_json::from_str(transport_ext)?;
    ensure!(ack.is_object(), "Invalid Frame transport extension");
    ack["frame_video_haptics"] = Value::from(1);
    Ok((ack.to_string(), true))
}

pub struct FrameVideoHeader {
    pub video: VideoPacketHeader,
    /// None keeps the exact upstream header; Some appends two little-endian f32.
    pub haptics: Option<[f32; 2]>,
    pub gaze_crops: Option<[[u32; 2]; 2]>,
    /// Negotiated capture clock, raw little-endian u64 after crops. The
    /// upstream timestamp continues to identify the exact tracking/crop source.
    pub image_timestamp_ns: Option<u64>,
}

pub fn negotiate_frame_video_timestamps(capabilities: &str, transport_ext: &str,
    supported: bool) -> Result<(String, bool)> {
    let caps: Value = serde_json::from_str(capabilities).unwrap_or(Value::Null);
    if !supported || caps.get("frame_video_timestamps").and_then(Value::as_u64) != Some(1) {
        return Ok((transport_ext.into(), false));
    }
    let mut ack: Value = serde_json::from_str(transport_ext)?;
    ensure!(ack.is_object(), "Invalid Frame transport extension");
    ack["frame_video_timestamps"] = Value::from(1);
    Ok((ack.to_string(), true))
}

impl Serialize for FrameVideoHeader {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        let mut fields = serializer.serialize_tuple(
            1 + usize::from(self.haptics.is_some()) + usize::from(self.gaze_crops.is_some())
                + usize::from(self.image_timestamp_ns.is_some()),
        )?;
        fields.serialize_element(&self.video)?;
        if let Some(amplitudes) = self.haptics {
            fields.serialize_element(&amplitudes)?;
        }
        if let Some(crops) = self.gaze_crops {
            let mut bytes = [0u8; 16];
            for (out, value) in bytes.chunks_exact_mut(4).zip(crops.into_iter().flatten()) {
                out.copy_from_slice(&value.to_le_bytes());
            }
            fields.serialize_element(&bytes)?;
        }
        if let Some(timestamp) = self.image_timestamp_ns {
            fields.serialize_element(&timestamp.to_le_bytes())?;
        }
        fields.end()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_common::ViewParams;
    use bincode::config;
    use std::time::Duration;

    fn header() -> VideoPacketHeader {
        VideoPacketHeader {
            timestamp: Duration::from_nanos(1_000_000_234),
            global_view_params: [ViewParams::DUMMY; 2],
            foveation_center_shifts: None,
            is_idr: true,
        }
    }

    #[test]
    fn video_haptics_requires_explicit_version_and_preserves_transport_ack() {
        let transport = r#"{"frame_split_streams_udp":{"version":1,"priority_port":19945}}"#;
        for caps in [
            "",
            "opaque",
            "{}",
            r#"{"frame_video_haptics":2}"#,
            r#"{"frame_video_haptics":true}"#,
        ] {
            assert_eq!(
                negotiate_frame_video_haptics(caps, transport).unwrap(),
                (transport.into(), false)
            );
        }
        let (ack, enabled) =
            negotiate_frame_video_haptics(r#"{"frame_video_haptics":1}"#, transport).unwrap();
        let ack: Value = serde_json::from_str(&ack).unwrap();
        assert!(enabled);
        assert_eq!(ack["frame_video_haptics"], 1);
        assert_eq!(ack["frame_split_streams_udp"]["priority_port"], 19945);
    }

    #[test]
    fn source_amplitudes_follow_the_original_header_without_touching_nals() {
        let original = bincode::serde::encode_to_vec(header(), config::standard()).unwrap();
        let legacy = bincode::serde::encode_to_vec(
            FrameVideoHeader {
                video: header(),
                haptics: None,
                gaze_crops: None,
                image_timestamp_ns: None,
            },
            config::standard(),
        )
        .unwrap();
        assert_eq!(legacy, original);
        let mut extended = bincode::serde::encode_to_vec(
            FrameVideoHeader {
                video: header(),
                haptics: Some([0.25, 0.5]),
                gaze_crops: None,
                image_timestamp_ns: None,
            },
            config::standard(),
        )
        .unwrap();
        let nal = [0, 0, 0, 1, 0x40];
        extended.extend(nal);
        assert_eq!(&extended[..original.len()], original);
        assert_eq!(
            &extended[original.len()..original.len() + 8],
            &[0, 0, 128, 62, 0, 0, 0, 63]
        );
        assert_eq!(&extended[original.len() + 8..], nal);
    }

    #[test]
    fn exact_source_crops_follow_optional_haptics_without_option_tags() {
        let original = bincode::serde::encode_to_vec(header(), config::standard()).unwrap();
        for haptics in [None, Some([0.25, 0.5])] {
            let bytes = bincode::serde::encode_to_vec(
                FrameVideoHeader {
                    video: header(),
                    haptics,
                    gaze_crops: Some([[100, 200], [300, 400]]),
                    image_timestamp_ns: None,
                },
                config::standard(),
            )
            .unwrap();
            let offset = original.len() + if haptics.is_some() { 8 } else { 0 };
            assert_eq!(&bytes[..original.len()], original);
            assert_eq!(bytes.len(), offset + 16);
            let expected: Vec<u8> = [100u32, 200, 300, 400]
                .into_iter()
                .flat_map(u32::to_le_bytes)
                .collect();
            assert_eq!(&bytes[offset..], expected);
        }
    }

    #[test]
    fn capture_timestamp_is_explicitly_negotiated_and_keeps_original_pose_header() {
        let transport = r#"{"frame_gaze_streaming":{"version":1}}"#;
        for caps in ["{}", r#"{"frame_video_timestamps":2}"#, r#"{"frame_video_timestamps":true}"#] {
            assert_eq!(negotiate_frame_video_timestamps(caps, transport, true).unwrap(), (transport.into(), false));
        }
        assert!(!negotiate_frame_video_timestamps(r#"{"frame_video_timestamps":1}"#, transport, false).unwrap().1);
        let (ack, enabled) = negotiate_frame_video_timestamps(r#"{"frame_video_timestamps":1}"#, transport, true).unwrap();
        let ack: Value = serde_json::from_str(&ack).unwrap();
        assert!(enabled && ack["frame_video_timestamps"] == 1);
        assert_eq!(ack["frame_gaze_streaming"]["version"], 1);
        let original = bincode::serde::encode_to_vec(header(), config::standard()).unwrap();
        let extended = bincode::serde::encode_to_vec(FrameVideoHeader {
            video: header(), haptics: None, gaze_crops: Some([[100, 200], [300, 400]]),
            image_timestamp_ns: Some(1_006_944_678),
        }, config::standard()).unwrap();
        assert_eq!(&extended[..original.len()], original);
        assert_eq!(extended.len(), original.len() + 24);
        assert_eq!(&extended[extended.len()-8..], 1_006_944_678u64.to_le_bytes());
    }
}
