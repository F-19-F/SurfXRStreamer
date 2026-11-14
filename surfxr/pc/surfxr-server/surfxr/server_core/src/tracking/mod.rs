mod receive;
pub use receive::tracking_loop;
mod body;
mod eye_gaze;
mod face;
mod vmc;

pub use body::*;
pub use face::*;
pub use vmc::*;

use crate::{
    ConnectionContext, SESSION_MANAGER, ServerCoreEvent,
    connection::STREAMING_RECV_TIMEOUT,
    hand_gestures::{self, HAND_GESTURE_BUTTON_SET, HandGestureManager},
    input_mapping::ButtonMappingManager,
};
use surfxr_common::{
    ConnectionError, DEVICE_ID_TO_PATH, DeviceMotion, Pose, ViewParams,
    glam::{Quat, Vec3},
    inputs as inp, warn,
};
use surfxr_events::{EventType, TrackingEvent};
use surfxr_packets::TrackingData;
use surfxr_session::{
    BodyTrackingConfig, HeadsetConfig, RecenteringMode, Settings, VMCConfig,
    settings_schema::Switch,
};
use surfxr_sockets::StreamReceiver;
use eye_gaze::EyeGazeReceiver;
use std::{
    cmp::Ordering,
    collections::{HashMap, VecDeque},
    f32::consts::PI,
    time::Duration,
};

const DEG_TO_RAD: f32 = PI / 180.0;

#[derive(Debug)]
pub enum HandType {
    Left = 0,
    Right = 1,
}

// todo: Move this struct to Settings and use it for every tracked device
#[derive(Default)]
struct MotionConfig {
    // Position offset applied after rotation offset
    pose_offset: Pose,
    linear_velocity_cutoff: f32,
    angular_velocity_cutoff: f32,
}

pub struct TrackingManager {
    last_head_pose: Pose,             // client's reference space
    inverse_recentering_origin: Pose, // client's reference space
    device_motions_history: HashMap<u64, VecDeque<(Duration, DeviceMotion)>>,
    hand_skeletons_history: [VecDeque<(Duration, [Pose; 26])>; 2],
    combined_eye_gaze_history: VecDeque<(Duration, Option<Quat>)>,
    max_history_size: usize,
    frame_epoch: u64,
}

impl TrackingManager {
    pub fn new(max_history_size: usize) -> TrackingManager {
        TrackingManager {
            last_head_pose: Pose::IDENTITY,
            inverse_recentering_origin: Pose::IDENTITY,
            device_motions_history: HashMap::new(),
            hand_skeletons_history: [VecDeque::new(), VecDeque::new()],
            combined_eye_gaze_history: VecDeque::new(),
            max_history_size,
            frame_epoch: 0,
        }
    }

    fn frame_epoch(&mut self, epoch: u64) {
        if self.frame_epoch != epoch {
            self.device_motions_history.clear();
            self.hand_skeletons_history = [VecDeque::new(), VecDeque::new()];
            self.combined_eye_gaze_history.clear();
            self.frame_epoch = epoch;
        }
    }

    pub fn recenter(&mut self, recentering_mode: &RecenteringMode) {
        let position = match recentering_mode {
            RecenteringMode::Stage => Vec3::ZERO,
            RecenteringMode::LocalFloor => {
                let mut pos = self.last_head_pose.position;
                pos.y = 0.0;

                pos
            }
            RecenteringMode::Local { view_height } | RecenteringMode::Tilted { view_height } => {
                self.last_head_pose.position - Vec3::new(0.0, *view_height, 0.0)
            }
        };

        let orientation = match recentering_mode {
            RecenteringMode::Stage => Quat::IDENTITY,
            RecenteringMode::LocalFloor | RecenteringMode::Local { .. } => {
                let mut rot = self.last_head_pose.orientation;
                // extract yaw rotation
                rot.x = 0.0;
                rot.z = 0.0;
                rot = rot.normalize();

                rot
            }
            RecenteringMode::Tilted { .. } => self.last_head_pose.orientation,
        };

        self.inverse_recentering_origin = Pose {
            position,
            orientation,
        }
        .inverse();
    }

    pub fn recenter_pose(&self, pose: Pose) -> Pose {
        self.inverse_recentering_origin * pose
    }

    pub fn recenter_motion(&self, motion: DeviceMotion) -> DeviceMotion {
        self.inverse_recentering_origin * motion
    }

    // Performs all kinds of tracking transformations, driven by settings.
    pub fn report_device_motions(
        &mut self,
        headset_config: &HeadsetConfig,
        timestamp: Duration,
        device_motions: &[(u64, DeviceMotion)],
    ) {
        let mut device_motion_configs = HashMap::new();
        device_motion_configs.insert(*inp::HEAD_ID, MotionConfig::default());
        device_motion_configs.extend([
            (*inp::BODY_CHEST_ID, MotionConfig::default()),
            (*inp::BODY_HIPS_ID, MotionConfig::default()),
            (*inp::BODY_LEFT_ELBOW_ID, MotionConfig::default()),
            (*inp::BODY_RIGHT_ELBOW_ID, MotionConfig::default()),
            (*inp::BODY_LEFT_KNEE_ID, MotionConfig::default()),
            (*inp::BODY_LEFT_FOOT_ID, MotionConfig::default()),
            (*inp::BODY_RIGHT_KNEE_ID, MotionConfig::default()),
            (*inp::BODY_RIGHT_FOOT_ID, MotionConfig::default()),
        ]);

        if let Switch::Enabled(controllers) = &headset_config.controllers {
            device_motion_configs.insert(
                *inp::HAND_LEFT_ID,
                MotionConfig {
                    pose_offset: Pose::IDENTITY,
                    linear_velocity_cutoff: controllers.linear_velocity_cutoff,
                    angular_velocity_cutoff: controllers.angular_velocity_cutoff * DEG_TO_RAD,
                },
            );

            device_motion_configs.insert(
                *inp::HAND_RIGHT_ID,
                MotionConfig {
                    pose_offset: Pose::IDENTITY,
                    linear_velocity_cutoff: controllers.linear_velocity_cutoff,
                    angular_velocity_cutoff: controllers.angular_velocity_cutoff * DEG_TO_RAD,
                },
            );
        }

        for &(device_id, mut motion) in device_motions {
            if device_id == *inp::HEAD_ID {
                self.last_head_pose = motion.pose;
            }

            if let Some(config) = device_motion_configs.get(&device_id) {
                motion = self.recenter_motion(motion);

                motion.pose = motion.pose * config.pose_offset;

                fn cutoff(v: Vec3, threshold: f32) -> Vec3 {
                    if v.length_squared() > threshold * threshold {
                        v
                    } else {
                        Vec3::ZERO
                    }
                }

                if self.frame_epoch == 0 {
                    motion.linear_velocity = cutoff(motion.linear_velocity, config.linear_velocity_cutoff);
                    motion.angular_velocity = cutoff(motion.angular_velocity, config.angular_velocity_cutoff);
                }
            }

            if let Some(motions) = self.device_motions_history.get_mut(&device_id) {
                motions.push_front((timestamp, motion));

                if motions.len() > self.max_history_size {
                    motions.pop_back();
                }
            } else {
                self.device_motions_history
                    .insert(device_id, VecDeque::from(vec![(timestamp, motion)]));
            }
        }
    }

    // If the exact sample_timestamp is not found, use the closest one if it's not older. This makes
    // sure that we return None if there is no newer sample and always return Some otherwise.
    pub fn get_device_motion(
        &self,
        device_id: u64,
        sample_timestamp: Duration,
    ) -> Option<DeviceMotion> {
        self.device_motions_history
            .get(&device_id)
            .and_then(|motions| {
                if self.frame_epoch != 0 {
                    return motions.iter().find_map(|(ts, motion)| (*ts == sample_timestamp).then_some(*motion));
                }
                // Get first element to initialize a valid motion reference
                if let Some((_, motion)) = motions.front() {
                    let mut best_timestamp_diff = Duration::MAX;
                    let mut best_motion_ref = motion;

                    // Note: we are iterating from most recent to oldest
                    for (ts, m) in motions {
                        match ts.cmp(&sample_timestamp) {
                            Ordering::Equal => return Some(*m),
                            Ordering::Greater => {
                                let diff = ts.saturating_sub(sample_timestamp);
                                if diff < best_timestamp_diff {
                                    best_timestamp_diff = diff;
                                    best_motion_ref = m;
                                }
                            }
                            Ordering::Less => continue,
                        }
                    }

                    (best_timestamp_diff != Duration::MAX).then_some(*best_motion_ref)
                } else {
                    None
                }
            })
    }

    pub fn report_hand_skeleton(
        &mut self,
        hand_type: HandType,
        timestamp: Duration,
        mut skeleton: [Pose; 26],
    ) {
        for pose in &mut skeleton {
            *pose = self.recenter_pose(*pose);
        }

        let skeleton_history = &mut self.hand_skeletons_history[hand_type as usize];

        skeleton_history.push_back((timestamp, skeleton));

        if skeleton_history.len() > self.max_history_size {
            skeleton_history.pop_front();
        }
    }

    pub fn get_hand_skeleton(
        &self,
        hand_type: HandType,
        sample_timestamp: Duration,
    ) -> Option<&[Pose; 26]> {
        self.hand_skeletons_history[hand_type as usize]
            .iter()
            .find(|(timestamp, _)| *timestamp == sample_timestamp)
            .map(|(_, skeleton)| skeleton)
    }

    pub fn report_combined_eye_gaze(&mut self, timestamp: Duration, gaze: Option<Quat>) {
        self.combined_eye_gaze_history.push_back((timestamp, gaze));

        if self.combined_eye_gaze_history.len() > self.max_history_size {
            self.combined_eye_gaze_history.pop_front();
        }
    }

    pub fn get_combined_eye_gaze(&self, sample_timestamp: Duration) -> Option<Quat> {
        // Use the first retained sample for an exact timestamp, including missing gaze.
        self.combined_eye_gaze_history
            .iter()
            .find(|(timestamp, _)| *timestamp == sample_timestamp)
            .and_then(|(_, gaze)| *gaze)
    }

    pub fn unrecenter_view_params(&self, view_params: &mut [ViewParams; 2]) {
        for params in view_params {
            params.pose = self.inverse_recentering_origin.inverse() * params.pose;
        }
    }
}

#[cfg(test)]
mod timed_tests {
    use super::*;
    #[test]
    fn timed_motion_keeps_velocities_and_requires_exact_identity() {
        let settings=surfxr_session::SessionConfig::default().to_settings();
        let mut manager=TrackingManager::new(20);manager.frame_epoch(1);
        let ts=Duration::from_secs(1);
        let motion=DeviceMotion {pose:Pose::IDENTITY,linear_velocity:Vec3::splat(0.0001),angular_velocity:Vec3::splat(0.0001)};
        manager.report_device_motions(&settings.headset,ts,&[(*inp::HEAD_ID,motion),(*inp::HAND_LEFT_ID,motion)]);
        assert_eq!(manager.get_device_motion(*inp::HAND_LEFT_ID,ts).unwrap().linear_velocity,motion.linear_velocity);
        assert!(manager.get_device_motion(*inp::HAND_LEFT_ID,ts-Duration::from_nanos(1)).is_none());
        manager.frame_epoch(2);
        assert!(manager.get_device_motion(*inp::HEAD_ID,ts).is_none());
    }
}
