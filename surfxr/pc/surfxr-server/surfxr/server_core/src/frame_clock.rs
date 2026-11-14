//! Negotiated calibration evidence attached to each head/controller sample.
pub const STAMP_SIZE: usize = 80;
#[derive(Clone, Copy, Debug)]
pub struct Calibration {
    pub revision: u64,
    pub error_ns: u64,
}
pub fn negotiate(capabilities: &str, extensions: &str) -> (String, bool) {
    let caps=serde_json::from_str::<serde_json::Value>(capabilities).unwrap_or_default();
    let mut ack=serde_json::from_str::<serde_json::Value>(extensions).unwrap_or_default();
    let version=caps["frame_clock_calibration"].as_u64().unwrap_or(0);
    if !cfg!(windows) || !(1..=2).contains(&version) ||
        ack["frame_pose_time"].as_u64()!=Some(1) {return (extensions.into(),false);}
    ack["frame_clock_calibration"]=serde_json::json!(version);
    (ack.to_string(),true)
}
pub(crate) fn read_stamp(bytes:&[u8],poll:u64,last_revision:u64)->Option<Calibration> {
    if bytes.len()!=STAMP_SIZE {return None;}
    let field=|offset|u64::from_le_bytes(bytes[offset..offset+8].try_into().unwrap());
    let revision=field(48);let error_ns=field(56);let rate_ppb=field(64);let calibrated_client_ns=field(72);
    // Error and reply age describe estimate quality, not stream liveness.
    // Keep structural/order/unit checks; do not reject a valid holdover pose.
    if revision==0 || revision<last_revision || error_ns==0 ||
        !(990_000_000..=1_010_000_000).contains(&rate_ppb) || calibrated_client_ns==0 ||
        calibrated_client_ns>poll {return None;}
    Some(Calibration {revision,error_ns})
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn structural_evidence_is_required_but_quality_does_not_gate_holdover() {
        let mut b=vec![0;STAMP_SIZE];
        for (offset,value) in [(48,9u64),(56,250_000),(64,1_000_150_000),(72,10_000_000_000)] {
            b[offset..offset+8].copy_from_slice(&value.to_le_bytes());
        }
        let poll=10_002_000_000;assert_eq!(read_stamp(&b,poll,8).unwrap().error_ns,250_000);
        assert!(read_stamp(&b,poll,10).is_none());
        assert!(read_stamp(&b,9_999_999_999,0).is_none());
        assert!(read_stamp(&b,70_000_000_000,0).is_some());
        b[48..56].copy_from_slice(&1u64.to_le_bytes());
        b[56..64].copy_from_slice(&30_000_000u64.to_le_bytes());
        assert_eq!(read_stamp(&b,70_000_000_000,0).unwrap().error_ns,30_000_000);
        for (offset,value) in [(48,0u64),(56,0),(64,1),(72,0)] {
            let mut bad=b.clone();bad[offset..offset+8].copy_from_slice(&value.to_le_bytes());
            assert!(read_stamp(&bad,poll,0).is_none());
        }
        assert!(read_stamp(&b[..48],poll,0).is_none());
    }
    #[test]
    fn calibration_requires_explicit_pose_contract() {
        assert!(!negotiate(r#"{"frame_clock_calibration":1}"#,"{}").1);
        for caps in ["{}",r#"{"frame_clock_calibration":3}"#,r#"{"frame_clock_calibration":true}"#] {
            assert!(!negotiate(caps,r#"{"frame_pose_time":1}"#).1);
        }
        assert_eq!(negotiate(r#"{"frame_clock_calibration":1}"#,r#"{"frame_pose_time":1}"#).1,cfg!(windows));
        let (ack,enabled)=negotiate(r#"{"frame_clock_calibration":2}"#,r#"{"frame_pose_time":1}"#);
        assert_eq!(enabled,cfg!(windows));
        if enabled {assert_eq!(serde_json::from_str::<serde_json::Value>(&ack).unwrap()["frame_clock_calibration"],2);}
    }
}
