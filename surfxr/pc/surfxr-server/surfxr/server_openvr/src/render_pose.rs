//! Freeze the actual compositor source pose before capture owns a GPU lease.
use crate::{HEAD_POSE_QUEUE, Pose};
use surfxr_common::{glam::{Mat3, Quat, Vec3}, parking_lot::Mutex};
use std::{collections::VecDeque, time::Duration};

static RECORDED: Mutex<VecDeque<Duration>> = Mutex::new(VecDeque::new());
pub(super) fn clear() { RECORDED.lock().clear(); HEAD_POSE_QUEUE.lock().clear(); }

fn from_matrix(m: &[f32; 12]) -> Option<Pose> {
    if !m.iter().all(|v| v.is_finite()) { return None; }
    let rotation = Mat3::from_cols(Vec3::new(m[0],m[4],m[8]),
        Vec3::new(m[1],m[5],m[9]), Vec3::new(m[2],m[6],m[10]));
    if (rotation.determinant()-1.).abs() > 0.01 { return None; }
    let orientation = Quat::from_mat3(&rotation).normalize();
    orientation.is_finite().then_some(Pose { orientation, position: Vec3::new(m[3],m[7],m[11]) })
}

#[unsafe(export_name = "RecordFramePose")]
unsafe extern "C" fn record(timestamp_ns: u64, matrix: *const f32) -> bool {
    if matrix.is_null() || timestamp_ns == 0 { return false; }
    // The synchronous C++ caller supplies a contiguous HmdMatrix34_t (12 floats).
    let matrix: &[f32; 12] = unsafe { &*matrix.cast() };
    let Some(pose) = from_matrix(matrix) else { return false; };
    let timestamp = Duration::from_nanos(timestamp_ns);
    if !crate::SERVER_CORE_CONTEXT.read().as_ref().is_some_and(|context|
        matches!(context.frame_pose_time(timestamp), Ok(Some(_)))) { return false; }
    let mut recorded = RECORDED.lock();
    let mut queue = HEAD_POSE_QUEUE.lock();
    store(timestamp,pose,&mut recorded,&mut queue)
}

fn store(timestamp: Duration, pose: Pose, recorded: &mut VecDeque<Duration>,
    queue: &mut VecDeque<(Duration,Pose)>) -> bool {
    if recorded.contains(&timestamp) { return false; }
    let Some((_, entry)) = queue.iter_mut().find(|(ts,_)| *ts == timestamp) else { return false; };
    *entry = pose;
    recorded.push_back(timestamp);
    while recorded.len() > 360 { recorded.pop_front(); }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn preserves_submitted_translation_and_rotation() {
        let m = [0.,0.,1.,3., 0.,1.,0.,4., -1.,0.,0.,5.];
        let pose = from_matrix(&m).unwrap();
        assert_eq!(pose.position, Vec3::new(3.,4.,5.));
        assert!((pose.orientation * Vec3::Z - Vec3::X).length() < 1e-6);
        assert!(from_matrix(&[0.;12]).is_none());
        let mut bad=m; bad[3]=f32::NAN; assert!(from_matrix(&bad).is_none());
    }
    #[test]
    fn source_pose_is_frozen_per_identity() {
        let ts=Duration::from_nanos(100);let mut queue=VecDeque::from([(ts,Pose::IDENTITY)]);
        let mut recorded=VecDeque::new();let pose=Pose {position:Vec3::ONE,..Pose::IDENTITY};
        assert!(store(ts,pose,&mut recorded,&mut queue));
        assert!(!store(ts,Pose::IDENTITY,&mut recorded,&mut queue));
        assert_eq!(queue.front().unwrap().1.position,Vec3::ONE);
        assert!(!store(ts+Duration::from_nanos(1),pose,&mut recorded,&mut queue));
        for i in 0..400 { let ts=Duration::from_secs(i+1); queue.push_back((ts,Pose::IDENTITY));
            assert!(store(ts,pose,&mut recorded,&mut queue)); }
        assert_eq!(recorded.len(),360);
    }
}
