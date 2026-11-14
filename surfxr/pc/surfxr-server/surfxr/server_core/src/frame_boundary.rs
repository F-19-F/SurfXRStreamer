//! Negotiated, bounded collision quads in Frame standing/STAGE space.
use surfxr_common::{Pose, glam::Vec3};
use crate::{ConnectionContext, ServerCoreEvent, SESSION_MANAGER};

// Frame already supplies calibrated STAGE poses. Never derive a second origin
// from last_head_pose: reliable PlayspaceSync can overtake UDP tracking across
// a Stage change. SteamVR owns the centered standing transform on the PC.
pub(crate) fn recenter(tracking: &mut crate::tracking::TrackingManager,
    configured: &surfxr_session::RecenteringMode, enabled: bool) {
    tracking.recenter(if enabled { &surfxr_session::RecenteringMode::Stage } else { configured });
}

pub struct FrameBoundary {
    pub epoch: u64,
    pub width: f32,
    pub depth: f32,
    pub quads: Vec<[f32; 12]>,
}

pub fn requested(capabilities: &str) -> bool {
    serde_json::from_str::<serde_json::Value>(capabilities).ok()
        .is_some_and(|v| v["frame_boundary_sync"].as_u64() == Some(1))
}

pub fn negotiate(capabilities: &str, extensions: &str) -> (String, bool) {
    let enabled = requested(capabilities);
    if !enabled { return (extensions.into(), false); }
    let mut value: serde_json::Value = serde_json::from_str(extensions)
        .unwrap_or_else(|_| serde_json::json!({}));
    value["frame_boundary_sync"] = serde_json::json!(1);
    (value.to_string(), true)
}

pub fn decode(bytes: &[u8]) -> Option<FrameBoundary> {
    if bytes.len() < 28 { return None; }
    let u32_at = |i| u32::from_le_bytes(bytes[i..i+4].try_into().unwrap());
    if u32_at(0) != 0x56444244 || u32_at(4) != 1 { return None; }
    let count = u32_at(24) as usize;
    if !(3..=64).contains(&count) || bytes.len() != 28 + count * 48 { return None; }
    let real = |i| f32::from_bits(u32_at(i));
    let width = real(16); let depth = real(20);
    if ![width, depth].iter().all(|v| v.is_finite() && *v > 0.0 && *v <= 100.0) { return None; }
    let mut quads = Vec::with_capacity(count);
    for i in 0..count {
        let quad = std::array::from_fn(|j| real(28 + i * 48 + j * 4));
        if !quad.iter().all(|v: &f32| v.is_finite() && v.abs() <= 100.0) { return None; }
        quads.push(quad);
    }
    Some(FrameBoundary { epoch: u64::from_le_bytes(bytes[8..16].try_into().unwrap()), width, depth, quads })
}

impl FrameBoundary {
    pub fn transform(&mut self, origin: Pose) {
        for quad in &mut self.quads {
            for point in quad.chunks_exact_mut(3) {
                let p = origin.position + origin.orientation * Vec3::from_slice(point);
                point.copy_from_slice(&p.to_array());
            }
        }
    }
}

pub(crate) fn receive(ctx: &ConnectionContext, bytes: &[u8]) {
    let Some(mut boundary) = decode(bytes) else { return; };
    let session = SESSION_MANAGER.read();
    if session.settings().headset.tracking_ref_only { return; }
    drop(session);
    // The negotiated connection keeps tracking in STAGE. Shape retries do not
    // derive an origin from a potentially older UDP head sample.
    let tracking = ctx.tracking_manager.read();
    boundary.transform(tracking.recenter_pose(Pose::IDENTITY));
    drop(tracking);
    ctx.events_sender.send(ServerCoreEvent::FrameBoundary(boundary)).ok();
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_common::glam::Quat;
    fn packet() -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend(0x56444244u32.to_le_bytes()); bytes.extend(1u32.to_le_bytes());
        bytes.extend(7u64.to_le_bytes()); bytes.extend(2f32.to_le_bytes()); bytes.extend(3f32.to_le_bytes());
        bytes.extend(3u32.to_le_bytes());
        for _ in 0..3 { for v in [1f32,0.,2.,1.,2.,2.,3.,2.,4.,3.,0.,4.] { bytes.extend(v.to_le_bytes()); } }
        bytes
    }
    #[test]
    fn bounded_wire_and_coordinate_transform() {
        let bytes = packet(); let mut b = decode(&bytes).unwrap();
        assert_eq!(b.epoch, 7); assert_eq!(b.quads.len(), 3);
        b.transform(Pose { orientation: Quat::from_rotation_y(std::f32::consts::FRAC_PI_2), position: Vec3::new(10.,1.,20.) });
        for (a,e) in b.quads[0][0..3].iter().zip([12.,1.,19.]) { assert!((a-e).abs() < 0.0001); }
        for len in 0..bytes.len() { assert!(decode(&bytes[..len]).is_none()); }
        let mut bad=bytes.clone(); bad.push(0); assert!(decode(&bad).is_none());
        for count in [0u32,2,65,u32::MAX] { let mut bad=bytes.clone(); bad[24..28].copy_from_slice(&count.to_le_bytes()); assert!(decode(&bad).is_none()); }
        for v in [f32::NAN,f32::INFINITY,-1.,0.,101.] { let mut bad=bytes.clone(); bad[16..20].copy_from_slice(&v.to_le_bytes()); assert!(decode(&bad).is_none()); }
        let mut bad=bytes; bad[28..32].copy_from_slice(&f32::NAN.to_le_bytes()); assert!(decode(&bad).is_none());
    }
    #[test]
    fn old_peers_do_not_negotiate() {
        assert_eq!(negotiate("{}", "{}"), ("{}".into(),false));
        assert!(!requested(r#"{"frame_boundary_sync":2}"#));
        let (ext,enabled)=negotiate(r#"{"frame_boundary_sync":1}"#, r#"{"frame_video_haptics":1}"#);
        assert!(enabled && requested(&ext)); assert!(ext.contains("frame_video_haptics"));
    }
    #[test]
    fn frame_stage_does_not_depend_on_control_udp_arrival_order() {
        use surfxr_common::{DeviceMotion, inputs};
        use surfxr_session::{RecenteringMode, SessionConfig};
        use std::time::Duration;
        let settings=SessionConfig::default().to_settings();
        let mut tracking=crate::tracking::TrackingManager::new(8);
        let old=Pose { position: Vec3::new(8.,1.7,-4.), orientation: Quat::from_rotation_y(1.2) };
        tracking.report_device_motions(&settings.headset,Duration::from_secs(1),
            &[(*inputs::HEAD_ID,DeviceMotion {pose:old,..Default::default()})]);
        recenter(&mut tracking,&RecenteringMode::LocalFloor,false);
        assert!(tracking.recenter_pose(old).position.x.abs()<1e-5);
        let next=Pose {position:Vec3::new(-3.,1.7,5.),..old};
        // Control arrived before the first tracking packet in the new STAGE.
        recenter(&mut tracking,&RecenteringMode::LocalFloor,true);
        assert_eq!(tracking.recenter_pose(next).position,next.position);
        assert_eq!(tracking.recenter_pose(next).orientation,next.orientation);
        tracking.report_device_motions(&settings.headset,Duration::from_secs(2),
            &[(*inputs::HEAD_ID,DeviceMotion {pose:next,..Default::default()})]);
        // Same result if control arrives after UDP, or desktop inherits VR.
        recenter(&mut tracking,&RecenteringMode::LocalFloor,true);
        assert_eq!(tracking.recenter_pose(next).position,next.position);
        assert_eq!(tracking.recenter_pose(next).orientation,next.orientation);
        let mut boundary=decode(&packet()).unwrap();let before=boundary.quads.clone();
        boundary.transform(tracking.recenter_pose(Pose::IDENTITY));
        assert_eq!(boundary.quads,before);
    }
}
