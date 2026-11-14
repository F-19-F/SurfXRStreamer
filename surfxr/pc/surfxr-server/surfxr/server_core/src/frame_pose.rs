//! Negotiated present-time head/controller poses and per-session clock mapping.
use crate::{ConnectionContext, ServerCoreContext};
use surfxr_common::{DeviceMotion, HEAD_ID, HAND_LEFT_ID, HAND_RIGHT_ID};
use std::{collections::VecDeque, sync::{LazyLock, atomic::{AtomicU64, Ordering}}, time::{Duration, Instant}};

const STAMP_MAGIC: u32 = 0x31505645;
const CLOCK_MAGIC: u32 = 0x31435645;
const STAMP_SIZE: usize = 48;
const MAX_AGE_NS: u64 = 200_000_000;
static CLOCK_START: LazyLock<Instant> = LazyLock::new(Instant::now);
static GENERATION: AtomicU64 = AtomicU64::new(1);
pub fn now_ns() -> u64 { CLOCK_START.elapsed().as_nanos() as u64 + 1 }

#[derive(Clone, Copy, Debug)]
pub struct PoseTime { pub server_ns: u64, pub epoch: u64, pub uncertainty_ns: u64 }
impl PoseTime {
    pub fn offset_at(self, now: u64) -> Option<f64> {
        let delta = self.server_ns as i128 - now as i128;
        // Bound physically implausible prediction age in both directions.
        // Network uncertainty alone must not reject current pose samples.
        (-200_000_000..=200_000_000).contains(&delta).then_some(delta as f64 * 1e-9)
    }
}
pub struct FramePoseState {
    generation: u64,
    epoch: u64,
    sequence: u64,
    poll_ns: u64,
    active: bool,
    calibrated: bool,
    calibration_revision: u64,
    samples: VecDeque<(Duration, PoseTime)>,
}
impl Default for FramePoseState {
    fn default() -> Self {
        Self { generation: GENERATION.fetch_add(1, Ordering::Relaxed), epoch: 0,
            sequence: 0, poll_ns: 0, active: true, calibrated:false, calibration_revision:0, samples: VecDeque::new() }
    }
}
fn u64_at(bytes: &[u8], offset: usize) -> u64 { u64::from_le_bytes(bytes[offset..offset+8].try_into().unwrap()) }
fn tagged(bytes: &[u8], size: usize, magic: u32, version: u32) -> bool {
    bytes.len() == size && bytes[..4] == magic.to_le_bytes() && bytes[4..8] == version.to_le_bytes()
}
pub fn negotiate(capabilities: &str, extensions: &str) -> (String, bool) {
    let caps = serde_json::from_str::<serde_json::Value>(capabilities).unwrap_or_default();
    if !cfg!(windows) || caps["frame_pose_time"].as_u64() != Some(1) { return (extensions.into(), false); }
    let mut ack = serde_json::from_str::<serde_json::Value>(extensions).unwrap_or_else(|_|serde_json::json!({}));
    if !ack.is_object() { return (extensions.into(), false); }
    ack["frame_pose_time"] = serde_json::json!(1);
    (ack.to_string(), true)
}
pub(crate) fn clock_reply(ctx: &ConnectionContext, bytes: &[u8], received_ns: u64) -> Option<Vec<u8>> {
    if !tagged(bytes, 16, CLOCK_MAGIC, 1) { return None; }
    let state = ctx.frame_pose.lock();
    let state = state.as_ref()?;
    if !state.active { return None; }
    let sent = u64_at(bytes, 8);
    if sent == 0 || sent > i64::MAX as u64 / 4 { return None; }
    let mut reply = Vec::with_capacity(40);
    reply.extend(CLOCK_MAGIC.to_le_bytes()); reply.extend(1u32.to_le_bytes());
    for value in [state.generation, sent, received_ns, now_ns()] { reply.extend(value.to_le_bytes()); }
    Some(reply)
}
impl FramePoseState {
    pub fn new(calibrated:bool)->Self {Self {calibrated,..Self::default()}}
    pub fn stop(&mut self) { self.active = false; self.samples.clear(); }
    pub fn receive<'a>(&mut self, poll: Duration, payload: &'a [u8], now: u64,
        motions: &[(u64, DeviceMotion)]) -> Option<(&'a [u8], PoseTime)> {
        if !self.active { return None; }
        let size=if self.calibrated {crate::frame_clock::STAMP_SIZE} else {STAMP_SIZE};
        let split = payload.len().checked_sub(size)?;
        let bytes = &payload[split..];
        if !tagged(bytes,size,STAMP_MAGIC,if self.calibrated {2} else {1}) || u64_at(bytes,8)!=self.generation {return None;}
        let epoch = u64_at(bytes, 16); let sequence = u64_at(bytes, 24);
        let client_ns = u64_at(bytes, 40); let poll_ns = u64::try_from(poll.as_nanos()).ok()?;
        let server_ns = u64_at(bytes, 32);
        let calibration=if self.calibrated {Some(crate::frame_clock::read_stamp(bytes,poll_ns,self.calibration_revision)?)} else {None};
        let time=PoseTime {server_ns,epoch,uncertainty_ns:calibration.map_or(0,|c|c.error_ns)};
        if epoch == 0 || epoch < self.epoch || sequence == 0 ||
            (epoch == self.epoch && sequence <= self.sequence) || poll_ns <= self.poll_ns ||
            client_ns == 0 || client_ns > poll_ns || poll_ns-client_ns > MAX_AGE_NS ||
            time.server_ns == 0 || time.offset_at(now).is_none() || !valid_motions(motions) { return None; }
        if epoch != self.epoch { self.samples.clear(); }
        self.epoch = epoch; self.sequence = sequence; self.poll_ns = poll_ns;
        if let Some(c)=calibration {self.calibration_revision=c.revision;}
        self.samples.push_back((poll, time)); while self.samples.len() > 1024 { self.samples.pop_front(); }
        Some((&payload[..split], time))
    }
}
fn valid_motions(motions: &[(u64, DeviceMotion)]) -> bool {
    let ids = [*HEAD_ID, *HAND_LEFT_ID, *HAND_RIGHT_ID];
    motions.len() <= 3 && motions.iter().filter(|(id,_)| *id == *HEAD_ID).count() == 1 &&
        motions.iter().enumerate().all(|(i,(id,m))| ids.contains(id) &&
            !motions[..i].iter().any(|(old,_)| old == id) && m.pose.position.is_finite() &&
            m.pose.orientation.is_finite() && (m.pose.orientation.length_squared()-1.).abs() < 0.01 &&
            m.linear_velocity.is_finite() && m.angular_velocity.is_finite())
}
impl ServerCoreContext {
    // None is the untouched legacy path; Err means a negotiated sample expired
    // or was evicted, and must never fall back to legacy prediction.
    pub fn frame_pose_time(&self, poll: Duration) -> Result<Option<PoseTime>, ()> {
        let guard = self.connection_context.frame_pose.lock();
        let Some(state) = guard.as_ref() else { return Ok(None); };
        state.samples.iter().rev().find_map(|(ts,time)| (*ts == poll).then_some(*time))
            .filter(|time|time.epoch == state.epoch && time.offset_at(now_ns()).is_some()).map(Some).ok_or(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use surfxr_common::glam::{Quat, Vec3};
    fn packet(state: &FramePoseState, epoch: u64, seq: u64, server: u64, client: u64) -> Vec<u8> {
        let mut b = Vec::new(); b.extend(STAMP_MAGIC.to_le_bytes()); b.extend(1u32.to_le_bytes());
        for n in [state.generation,epoch,seq,server,client] { b.extend(n.to_le_bytes()); } b
    }
    #[test]
    fn calibrated_session_requires_v2_without_switching_to_legacy() {
        let mut state=FramePoseState::new(true);
        let head=[(*HEAD_ID,DeviceMotion {pose:surfxr_common::Pose::IDENTITY,
            linear_velocity:Vec3::new(1.,2.,3.),angular_velocity:Vec3::new(4.,5.,6.)})];
        let poll=Duration::from_nanos(5_002_000_000);let now=1_000_000_000;
        let legacy=packet(&state,1,1,now-2_000_000,5_000_000_000);
        assert!(state.receive(poll,&legacy,now,&head).is_none());
        let mut calibrated=legacy.clone();calibrated[4..8].copy_from_slice(&2u32.to_le_bytes());
        for field in [9u64,250_000,1_000_180_000,4_900_000_000] {calibrated.extend(field.to_le_bytes());}
        let (_,time)=state.receive(poll,&calibrated,now,&head).unwrap();
        assert_eq!(time.uncertainty_ns,250_000);assert_eq!(time.offset_at(now),Some(-0.002));
        assert!(state.receive(Duration::from_nanos(5_003_000_000),&legacy,now,&head).is_none());
        assert!(state.calibrated);assert_eq!(state.calibration_revision,9);
    }
    #[test]
    fn fresh_poses_survive_wide_uncertainty_and_long_clock_reply_loss() {
        let mut state=FramePoseState::new(true);
        let head=[(*HEAD_ID,DeviceMotion {pose:surfxr_common::Pose::IDENTITY,
            linear_velocity:Vec3::ZERO,angular_velocity:Vec3::ZERO})];
        for sequence in 1..=3 {
            let now=60_000_000_000+sequence*10_000_000;
            let client=90_000_000_000+sequence*10_000_000;
            let mut bytes=packet(&state,1,sequence,now+10_000_000,client);
            bytes[4..8].copy_from_slice(&2u32.to_le_bytes());
            for value in [1u64,30_000_000,1_000_000_000,5_000_000_000] {bytes.extend(value.to_le_bytes());}
            let (_,time)=state.receive(Duration::from_nanos(client+1_000_000),&bytes,now,&head).unwrap();
            assert_eq!(time.offset_at(now),Some(0.01));
        }
        assert_eq!(state.samples.len(),3);assert!(state.active);
    }
    #[test]
    fn age_epoch_duplicates_and_invalid_motion_are_rejected() {
        let mut state = FramePoseState::default();
        let motion = DeviceMotion { pose: surfxr_common::Pose {orientation: Quat::IDENTITY, position:Vec3::ZERO},
            linear_velocity: Vec3::new(1.,2.,3.), angular_velocity: Vec3::new(4.,5.,6.) };
        let head = [(*HEAD_ID,motion)]; let now = 1_000_000_000;
        let packet = packet(&state,1,1,now-4_000_000,5_000_000_000);
        let poll = Duration::from_nanos(5_001_000_000);
        for end in 0..packet.len() { assert!(state.receive(poll,&packet[..end],now,&head).is_none()); }
        let (_, time) = state.receive(poll,&packet,now,&head).unwrap();
        assert_eq!(time.offset_at(now),Some(-0.004));
        assert!(time.offset_at(now+200_000_000).is_none());
        assert!(state.receive(poll,&packet,now,&head).is_none());
        let mut bad = head; bad[0].1.angular_velocity.x=f32::NAN;
        let next = super::tests::packet(&state,2,2,now,5_002_000_000);
        assert!(state.receive(Duration::from_nanos(5_003_000_000),&next,now,&bad).is_none());
        assert!(state.receive(Duration::from_nanos(5_003_000_000),&next,now,&head).is_some());
        let old = super::tests::packet(&state,1,3,now,5_004_000_000);
        assert!(state.receive(Duration::from_nanos(5_005_000_000),&old,now,&head).is_none());
        assert_eq!(state.samples.len(),1);
        state.stop();
        let stopped = super::tests::packet(&state,2,3,now,5_006_000_000);
        assert!(state.receive(Duration::from_nanos(5_007_000_000),&stopped,now,&head).is_none());
        assert!(state.samples.is_empty());
    }
    #[test]
    fn explicit_capability_and_clock_age_bounds() {
        for caps in ["", "{}", r#"{"frame_pose_time":true}"#, r#"{"frame_pose_time":2}"#] {
            assert!(!negotiate(caps,"{}").1);
        }
        let (ack,enabled)=negotiate(r#"{"frame_pose_time":1}"#,r#"{"frame_video_haptics":1}"#);
        assert_eq!(enabled,cfg!(windows));
        assert_eq!(serde_json::from_str::<serde_json::Value>(&ack).unwrap()["frame_video_haptics"],1);
        assert!(!negotiate(r#"{"frame_pose_time":1}"#,"null").1);
        let t=PoseTime {server_ns:1_000_000_000,epoch:1,uncertainty_ns:0};
        assert_eq!(t.offset_at(1_200_000_000),Some(-0.2));
        assert!(t.offset_at(1_200_000_001).is_none());
        assert_eq!(t.offset_at(980_000_000),Some(0.02));
        assert!(t.offset_at(799_999_999).is_none());
        let uncertain=PoseTime {uncertainty_ns:30_000_000,..t};
        assert_eq!(uncertain.offset_at(1_010_000_000),Some(-0.01));
    }
    #[test]
    fn suffix_keeps_gaze_bytes_and_rejects_foreign_sessions() {
        let mut state=FramePoseState::default();let other=FramePoseState::default();
        let motion=DeviceMotion {pose:surfxr_common::Pose::IDENTITY,linear_velocity:Vec3::ZERO,angular_velocity:Vec3::ZERO};
        let head=[(*HEAD_ID,motion)];let poll=Duration::from_nanos(1000);
        assert!(state.receive(poll,&packet(&other,1,1,1000,900),1001,&head).is_none());
        let mut bytes=vec![0];bytes.extend(packet(&state,1,1,1000,900));
        assert_eq!(state.receive(poll,&bytes,1001,&head).unwrap().0,&[0]);
        let duplicate=[head[0],head[0]];
        assert!(!valid_motions(&duplicate));
        assert!(!valid_motions(&[(*HAND_LEFT_ID,motion)]));
    }
}
