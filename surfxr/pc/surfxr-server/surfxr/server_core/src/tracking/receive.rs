use super::*;

pub fn tracking_loop(
    ctx: &ConnectionContext,
    initial_settings: Settings,
    mut tracking_receiver: StreamReceiver<TrackingData>,
    is_streaming: impl Fn() -> bool,
) {
    let mut hand_gesture_manager = HandGestureManager::new();

    let mut gestures_button_mapping_manager =
        initial_settings
            .headset
            .controllers
            .as_option()
            .map(|config| {
                ButtonMappingManager::new_automatic(
                    &HAND_GESTURE_BUTTON_SET,
                    &config.emulation_mode,
                    &config.button_mapping_config,
                )
            });

    let eye_tracking_osc_port = initial_settings
        .headset
        .face_tracking
        .as_option()
        .and_then(|config| config.eye_tracking_osc_override.as_option())
        .map(|config| config.port);
    let mut eye_gaze_receiver = eye_tracking_osc_port.and_then(|port| {
        EyeGazeReceiver::new(port)
            .inspect_err(|error| warn!("Failed to bind external gaze OSC port {port}: {error}"))
            .ok()
    });

    let mut face_tracking_sink = initial_settings
        .headset
        .face_tracking
        .into_option()
        .and_then(|config| config.sink.social_presence.into_option())
        .and_then(|config| {
            FaceTrackingSink::new(config, initial_settings.connection.osc_local_port)
                .inspect_err(|error| warn!("Failed to initialize social presence sink: {error}"))
                .ok()
        });

    let mut body_tracking_sink = initial_settings
        .headset
        .body_tracking
        .into_option()
        .and_then(|config| {
            BodyTrackingSink::new(config.sink, initial_settings.connection.osc_local_port).ok()
        });

    let mut vmc_sink = initial_settings
        .headset
        .vmc
        .into_option()
        .and_then(|config| VMCSink::new(config).ok());

    while is_streaming() {
        let data = match tracking_receiver.recv(STREAMING_RECV_TIMEOUT) {
            Ok(tracking) => tracking,
            Err(ConnectionError::TryAgain(_)) => continue,
            Err(ConnectionError::Other(_)) => return,
        };
        let Ok((mut tracking, payload)) = data.get() else {
            return;
        };

        let timestamp = tracking.poll_timestamp;
        let (payload, pose_time) = {
            let mut state = ctx.frame_pose.lock();
            if let Some(state) = state.as_mut() {
                let Some((payload,time)) = state.receive(timestamp,payload,crate::frame_pose::now_ns(),&tracking.device_motions) else { continue; };
                (payload,Some(time))
            } else { (payload,None) }
        };
        if let Some(gaze) = ctx.frame_gaze.lock().as_mut() {
            if !gaze.update(timestamp, payload, std::time::Instant::now()) { return; }
        }

        if eye_tracking_osc_port.is_some() {
            // Select one eye input before foveation and social output consume it. Do not fall
            // back to headset eyes when OSC input is missing, invalid or unavailable.
            tracking.face.eyes_social = [None; 2];
            tracking.face.eyes_combined = if let Some(receiver) = &mut eye_gaze_receiver {
                match receiver.receive(timestamp) {
                    Ok(gaze) => gaze,
                    Err(error) => {
                        warn!("External gaze OSC receiver stopped: {error}");
                        eye_gaze_receiver = None;

                        None
                    }
                }
            } else {
                None
            };
        }

        if let Some(stats) = &mut *ctx.statistics_manager.write() {
            stats.report_tracking_received(timestamp);
        }

        let controllers_config = {
            let data_lock = SESSION_MANAGER.read();
            data_lock
                .settings()
                .headset
                .controllers
                .clone()
                .into_option()
        };

        let device_motion_keys;
        {
            let mut tracking_manager_lock = ctx.tracking_manager.write();
            let session_manager_lock = SESSION_MANAGER.read();
            let headset_config = &session_manager_lock.settings().headset;

            tracking.device_motions.extend_from_slice(
                &body::get_default_body_trackers_from_detached_controllers(
                    &tracking.device_motions,
                ),
            );
            tracking.device_motions.extend_from_slice(
                &body::get_default_body_trackers_from_motion_trackers_bd(&tracking.device_motions),
            );
            if let Some(skeleton) = &tracking.body {
                tracking
                    .device_motions
                    .extend_from_slice(&body::extract_default_trackers(skeleton));
            }

            device_motion_keys = tracking
                .device_motions
                .iter()
                .map(|(id, _)| *id)
                .collect::<Vec<_>>();

            let velocity_multiplier = if pose_time.is_some() { 1.0 } else { session_manager_lock.settings().extra.velocities_multiplier };
            tracking.device_motions.iter_mut().for_each(|(_, motion)| {
                motion.linear_velocity *= velocity_multiplier;
                motion.angular_velocity *= velocity_multiplier;
            });

            if let Some(time) = pose_time { tracking_manager_lock.frame_epoch(time.epoch); }
            tracking_manager_lock.report_device_motions(
                headset_config,
                timestamp,
                &tracking.device_motions,
            );

            if let Some(skeleton) = tracking.hand_skeletons[0] {
                tracking_manager_lock.report_hand_skeleton(HandType::Left, timestamp, skeleton);
            }
            if let Some(skeleton) = tracking.hand_skeletons[1] {
                tracking_manager_lock.report_hand_skeleton(HandType::Right, timestamp, skeleton);
            }

            tracking_manager_lock.report_combined_eye_gaze(timestamp, tracking.face.eyes_combined);

            if let Some(sink) = &mut face_tracking_sink {
                sink.send_tracking(&tracking.face);
            }

            if session_manager_lock.settings().extra.logging.log_tracking {
                let device_motions = device_motion_keys
                    .iter()
                    .filter_map(move |id| {
                        Some((
                            (*DEVICE_ID_TO_PATH.get(id)?).into(),
                            tracking_manager_lock
                                .get_device_motion(*id, timestamp)
                                .unwrap(),
                        ))
                    })
                    .collect::<Vec<(String, DeviceMotion)>>();

                surfxr_events::send_event(EventType::Tracking(Box::new(TrackingEvent {
                    device_motions,
                    hand_skeletons: tracking.hand_skeletons,
                    face: tracking.face,
                })))
            }
        };

        // Handle hand gestures
        if let (Some(gestures_config), Some(gestures_button_mapping_manager)) = (
            controllers_config
                .as_ref()
                .and_then(|c| c.hand_tracking_interaction.as_option()),
            &mut gestures_button_mapping_manager,
        ) {
            if !device_motion_keys.contains(&*inp::HAND_LEFT_ID)
                && let Some(hand_skeleton) = tracking.hand_skeletons[0]
            {
                ctx.events_sender
                    .send(ServerCoreEvent::Buttons(
                        hand_gestures::trigger_hand_gesture_actions(
                            gestures_button_mapping_manager,
                            *inp::HAND_LEFT_ID,
                            &hand_gesture_manager.get_active_gestures(
                                &hand_skeleton,
                                gestures_config,
                                *inp::HAND_LEFT_ID,
                            ),
                            gestures_config.only_touch,
                        ),
                    ))
                    .ok();
            }
            if !device_motion_keys.contains(&*inp::HAND_RIGHT_ID)
                && let Some(hand_skeleton) = tracking.hand_skeletons[1]
            {
                ctx.events_sender
                    .send(ServerCoreEvent::Buttons(
                        hand_gestures::trigger_hand_gesture_actions(
                            gestures_button_mapping_manager,
                            *inp::HAND_RIGHT_ID,
                            &hand_gesture_manager.get_active_gestures(
                                &hand_skeleton,
                                gestures_config,
                                *inp::HAND_RIGHT_ID,
                            ),
                            gestures_config.only_touch,
                        ),
                    ))
                    .ok();
            }
        }

        ctx.events_sender
            .send(ServerCoreEvent::Tracking {
                poll_timestamp: tracking.poll_timestamp,
            })
            .ok();

        let publish_vmc = matches!(
            SESSION_MANAGER.read().settings().headset.vmc,
            Switch::Enabled(VMCConfig { publish: true, .. })
        );
        if publish_vmc {
            let orientation_correction = matches!(
                SESSION_MANAGER.read().settings().headset.vmc,
                Switch::Enabled(VMCConfig {
                    orientation_correction: true,
                    ..
                })
            );

            if let Some(sink) = &mut vmc_sink {
                let tracking_manager_lock = ctx.tracking_manager.read();
                let device_motions = device_motion_keys
                    .iter()
                    .map(move |id| {
                        (
                            *id,
                            tracking_manager_lock
                                .get_device_motion(*id, timestamp)
                                .unwrap(),
                        )
                    })
                    .collect::<Vec<(u64, DeviceMotion)>>();

                if let Some(skeleton) = tracking.hand_skeletons[0] {
                    sink.send_hand_tracking(HandType::Left, &skeleton, orientation_correction);
                }
                if let Some(skeleton) = tracking.hand_skeletons[1] {
                    sink.send_hand_tracking(HandType::Right, &skeleton, orientation_correction);
                }
                sink.send_tracking(&device_motions, orientation_correction);
            }
        }

        let track_body = matches!(
            SESSION_MANAGER.read().settings().headset.body_tracking,
            Switch::Enabled(BodyTrackingConfig { tracked: true, .. })
        );
        if track_body && let Some(sink) = &mut body_tracking_sink {
            let tracking_manager_lock = ctx.tracking_manager.read();
            let device_motions = device_motion_keys
                .iter()
                .map(move |id| {
                    (
                        *id,
                        tracking_manager_lock
                            .get_device_motion(*id, timestamp)
                            .unwrap(),
                    )
                })
                .collect::<Vec<_>>();
            sink.send_tracking(&device_motions);
        }
    }
}
