//! Explicit Frame base/focus atlas; ordinary SURFXR headers remain unchanged.
use surfxr_common::anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
pub struct FrameGazeConfig {
    pub version: u32,
    pub base_width: u32,
    pub base_height: u32,
    pub focus_width: u32,
    pub focus_height: u32,
}
pub fn frame_gaze_requested(capabilities: &str, enabled: bool) -> bool {
    let caps: Value = serde_json::from_str(capabilities).unwrap_or(Value::Null);
    enabled && caps.get("frame_gaze_streaming").and_then(Value::as_u64) == Some(1)
}

pub fn negotiate_frame_gaze(
    capabilities: &str,
    ext: &str,
    resolution: [u32; 2],
    focus_percent: [u32; 2],
    supported: bool,
) -> Result<(String, Option<FrameGazeConfig>)> {
    if !frame_gaze_requested(capabilities, supported) {
        return Ok((ext.into(), None));
    }
    let [w, h] = resolution;
    ensure!(
        (32..=4096).contains(&w) && (32..=8192).contains(&h),
        "Invalid gaze resolution"
    );
    ensure!(
        focus_percent.iter().all(|p| (10..=33).contains(p)),
        "Invalid gaze focus percentage"
    );
    let align = |n: u32, divisor: u32| n.div_ceil(divisor * 32) * 32;
    let config = FrameGazeConfig {
        version: 1,
        base_width: align(w, 3),
        base_height: align(h, 3),
        focus_width: align(w * focus_percent[0], 100),
        focus_height: align(h * focus_percent[1], 100),
    };
    let mut ack: Value = serde_json::from_str(ext)?;
    ensure!(ack.is_object(), "Invalid Frame extension");
    ack["frame_gaze_streaming"] = serde_json::to_value(config)?;
    Ok((ack.to_string(), Some(config)))
}
#[derive(Debug, Serialize, Deserialize)]
pub struct FrameGazeEye {
    pub valid: bool,
    pub position: [f32; 2],
}
#[derive(Debug, Serialize, Deserialize)]
pub struct FrameGazeSample {
    pub sampled_ns: u64,
    pub origin_epoch: u64,
    pub eyes: [FrameGazeEye; 2],
}
pub fn decode_frame_gaze(bytes: &[u8]) -> Result<Option<FrameGazeSample>> {
    let (sample, count) = bincode::serde::decode_from_slice(bytes, bincode::config::standard())?;
    ensure!(count == bytes.len(), "Trailing Frame gaze bytes");
    Ok(sample)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn requires_capability_and_preserves_other_extensions() {
        let caps = r#"{"frame_gaze_streaming":1}"#;
        let ext = r#"{"frame_video_haptics":1}"#;
        for (cap, supported) in [
            ("{}", true),
            (caps, false),
            (r#"{"frame_gaze_streaming":2}"#, true),
        ] {
            assert_eq!(
                negotiate_frame_gaze(cap, ext, [3072, 3264], [25, 25], supported).unwrap(),
                (ext.into(), None)
            );
        }
        let (ack, config) = negotiate_frame_gaze(caps, ext, [3072, 3264], [25, 25], true).unwrap();
        assert_eq!(
            config.unwrap(),
            FrameGazeConfig {
                version: 1,
                base_width: 1024,
                base_height: 1088,
                focus_width: 768,
                focus_height: 832
            }
        );
        assert_eq!(
            serde_json::from_str::<Value>(&ack).unwrap()["frame_video_haptics"],
            1
        );
    }
    #[test]
    fn focus_sizes_stay_inside_atlas_slots_and_invalid_sizes_are_rejected() {
        let caps = r#"{"frame_gaze_streaming":1}"#;
        for resolution in [[32, 32], [2144, 2144], [3072, 3264], [4096, 8192]] {
            for percent in [[10, 10], [20, 30], [25, 25], [33, 33]] {
                let (_, config) =
                    negotiate_frame_gaze(caps, "{}", resolution, percent, true).unwrap();
                let config = config.unwrap();
                assert!(config.focus_width <= config.base_width);
                assert!(
                    config.focus_width <= resolution[0] && config.focus_height <= resolution[1]
                );
                assert!(config.base_height + config.focus_height <= 8192);
                assert_eq!(config.focus_width % 32, 0);
                assert_eq!(config.focus_height % 32, 0);
                assert!(config.focus_width * 100 >= resolution[0] * percent[0]);
                assert!(config.focus_height * 100 >= resolution[1] * percent[1]);
            }
        }
        let (_, config) = negotiate_frame_gaze(caps, "{}", [3072, 3264], [20, 30], true).unwrap();
        let config = config.unwrap();
        assert_eq!((config.focus_width, config.focus_height), (640, 992));
        for percent in [[0, 25], [25, 34], [u32::MAX, 25]] {
            assert!(negotiate_frame_gaze(caps, "{}", [3072, 3264], percent, true).is_err());
        }
        assert!(!frame_gaze_requested(caps, false));
        assert!(frame_gaze_requested(caps, true));
    }

    #[test]
    fn cpp_gaze_bytes_and_strict_bounds() {
        let mut bytes = vec![1, 251, 232, 3, 1, 1];
        bytes.extend(0.25f32.to_le_bytes());
        bytes.extend((-0.5f32).to_le_bytes());
        bytes.push(0);
        bytes.extend([0; 8]);
        let sample = decode_frame_gaze(&bytes).unwrap().unwrap();
        assert_eq!(sample.sampled_ns, 1000);
        assert_eq!(sample.eyes[0].position, [0.25, -0.5]);
        assert!(!sample.eyes[1].valid);
        assert!(decode_frame_gaze(&bytes[..bytes.len() - 1]).is_err());
        bytes.push(0);
        assert!(decode_frame_gaze(&bytes).is_err());
        assert!(decode_frame_gaze(&[0]).unwrap().is_none());
    }
}
