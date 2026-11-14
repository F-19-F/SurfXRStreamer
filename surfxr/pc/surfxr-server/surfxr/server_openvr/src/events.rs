use super::*;

pub(super) fn spawn_event_loop(events_receiver: mpsc::Receiver<ServerCoreEvent>) {
    let handle = thread::spawn(move || {
        if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
            context.start_connection();
        }

        let mut last_resync = Instant::now();
        let mut frame_media_lease = None;
        let mut streaming_initialized = false;
        let mut controller_state = surfxr_server_core::frame_controller_state::ControllerState::session(false);
        loop {
            unsafe { PollFrameBoundary() };
            let event = match events_receiver.recv_timeout(Duration::from_millis(5)) {
                Ok(event) => event,
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            };

            match event {
                ServerCoreEvent::SetOpenvrProperty { device_id, prop } => {
                    props::set_openvr_prop(None, device_id, prop)
                }
                ServerCoreEvent::ClientConnected(mut config) => unsafe {
                    frame_media_lease = config.frame_media_lease.take();
                    render_pose::clear();
                    FOVEATION_CENTER_QUEUE.lock().clear();
                    *EYE_TRACKED_FOVEATION.lock() = config
                        .foveated_encoding
                        .filter(|_| {
                            surfxr_server_core::settings()
                                .headset
                                .face_tracking
                                .as_option()
                                .is_some_and(|config| config.sink.eye_tracked_foveated_encoding)
                        })
                        .map(|params| {
                            EyeTrackedFoveation::new(params, config.transcoding_view_resolution)
                        });
                    streaming_initialized = InitializeStreaming(make_settings(Some(&config)));
                    if streaming_initialized {
                        RequestDriverResync();
                    } else {
                        SERVER_CORE_CONTEXT.write().take();

                        ShutdownSteamvr();
                    }
                },

                ServerCoreEvent::ClientDisconnected => unsafe {
                    render_pose::clear();
                    SetFrameControllerForwarding(false, 0);
                    SetFrameSkeleton(0, 0, ptr::null());
                    SetFrameSkeleton(1, 0, ptr::null());
                    DeinitializeStreaming();
                    streaming_initialized = false;
                    frame_media_lease.take();
                    *EYE_TRACKED_FOVEATION.lock() = None;
                    FOVEATION_CENTER_QUEUE.lock().clear();
                },
                ServerCoreEvent::Battery(info) => unsafe {
                    SetBattery(info.device_id, info.gauge_value, info.is_plugged)
                },
                ServerCoreEvent::PlayspaceSync(bounds) => unsafe {
                    SetChaperoneArea(bounds.x, bounds.y)
                },
                ServerCoreEvent::FrameControllerState(state) => {
                    if controller_state.apply(state) {
                        unsafe { SetFrameControllerForwarding(state.enabled, state.since_ns); }
                    }
                },
                ServerCoreEvent::FrameBoundary(boundary) => unsafe {
                    SetFrameBoundary(boundary.width, boundary.depth, boundary.quads.as_ptr().cast(), boundary.quads.len() as u32)
                },
                ServerCoreEvent::LocalViewParams(params) => unsafe {
                    *LOCAL_VIEW_PARAMS.write() = params;
                    if let Some(foveation) = &mut *EYE_TRACKED_FOVEATION.lock() {
                        foveation.view_params = Some(params);
                    }

                    let ffi_params = [
                        tracking::to_ffi_view_params(params[0]),
                        tracking::to_ffi_view_params(params[1]),
                    ];
                    SetLocalViewParams(ffi_params.as_ptr());
                },
                ServerCoreEvent::Tracking { poll_timestamp } => {
                    let headset_config = &surfxr_server_core::settings().headset;

                    let controllers_config = headset_config.controllers.clone().into_option();
                    let track_body = headset_config.body_tracking.enabled();

                    let tracked = controllers_config.as_ref().is_some_and(|c| c.tracked)
                        && controller_state.allows(poll_timestamp);
                    let detached_controllers = headset_config
                        .multimodal_tracking
                        .as_option()
                        .is_some_and(|c| c.detached_controllers_steamvr_sink);

                    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
                        let pose_time = match context.frame_pose_time(poll_timestamp) {
                            Ok(time) => time,
                            Err(()) => continue,
                        };
                        let sample_offset = match pose_time {
                            Some(time) => match time.offset_at(surfxr_server_core::frame_pose::now_ns()) {
                                Some(offset) => offset,
                                None => continue,
                            },
                            None => 0.0,
                        };
                        if let Some(foveation) = &mut *EYE_TRACKED_FOVEATION.lock() {
                            foveation.update(
                                poll_timestamp,
                                context.get_combined_eye_gaze(poll_timestamp),
                                Instant::now(),
                            );
                        }

                        let target_timestamp =
                            poll_timestamp + context.get_motion_to_photon_latency();
                        let controllers_pose_time_offset = context.get_tracker_pose_time_offset();
                        // We need to remove the additional offset that SteamVR adds
                        let target_controller_timestamp =
                            target_timestamp.saturating_sub(controllers_pose_time_offset);

                        let ffi_head_motion = if let Some(motion) =
                            context.get_device_motion(*HEAD_ID, poll_timestamp)
                        {
                            let motion = tracking::prediction_for_driver(motion, poll_timestamp, target_timestamp, pose_time.is_some());

                            let mut head_pose_queue_lock = HEAD_POSE_QUEUE.lock();
                            head_pose_queue_lock.push_back((poll_timestamp, motion.pose));
                            while head_pose_queue_lock.len() > 360 {
                                head_pose_queue_lock.pop_front();
                            }

                            if let Some(time) = pose_time {
                                tracking::to_timed_head_motion(motion, time.epoch, sample_offset)
                            } else {
                                tracking::to_ffi_motion(*HEAD_ID, motion)
                            }
                        } else {
                            if pose_time.is_some() { continue; }
                            FfiDeviceMotion::default()
                        };

                        let (ffi_left_controller_motion, ffi_right_controller_motion) =
                            if tracked && let Some(config) = &controllers_config {
                                let ffi_left_controller_motion = context
                                    .get_device_motion(*HAND_LEFT_ID, poll_timestamp)
                                    .map(|motion| {
                                        let motion = tracking::prediction_for_driver(motion, poll_timestamp, target_controller_timestamp, pose_time.is_some());
                                        let motion_offset = tracking::offset_controller_motion(
                                            config,
                                            *HAND_LEFT_ID,
                                            motion,
                                            pose_time.is_some(),
                                        );
                                        tracking::to_ffi_motion(*HAND_LEFT_ID, motion_offset)
                                    });
                                let ffi_right_controller_motion = context
                                    .get_device_motion(*HAND_RIGHT_ID, poll_timestamp)
                                    .map(|motion| {
                                        let motion = tracking::prediction_for_driver(motion, poll_timestamp, target_controller_timestamp, pose_time.is_some());
                                        let motion_offset = tracking::offset_controller_motion(
                                            config,
                                            *HAND_RIGHT_ID,
                                            motion,
                                            pose_time.is_some(),
                                        );
                                        tracking::to_ffi_motion(*HAND_RIGHT_ID, motion_offset)
                                    });

                                (ffi_left_controller_motion, ffi_right_controller_motion)
                            } else {
                                (None, None)
                            };

                        let (
                            ffi_left_hand_skeleton,
                            ffi_right_hand_skeleton,
                            use_separate_hand_trackers,
                            predict_hand_skeleton,
                        ) = if let Some(ControllersConfig {
                            hand_skeleton: Switch::Enabled(hand_skeleton_config),
                            ..
                        }) = controllers_config
                        {
                            let left_hand_skeleton = context
                                .get_hand_skeleton(HandType::Left, poll_timestamp)
                                .map(|s| {
                                    tracking::to_openvr_ffi_hand_skeleton(
                                        headset_config,
                                        *HAND_LEFT_ID,
                                        &s,
                                    )
                                });
                            let right_hand_skeleton = context
                                .get_hand_skeleton(HandType::Right, poll_timestamp)
                                .map(|s| {
                                    tracking::to_openvr_ffi_hand_skeleton(
                                        headset_config,
                                        *HAND_RIGHT_ID,
                                        &s,
                                    )
                                });

                            (
                                tracked.then_some(left_hand_skeleton).flatten(),
                                tracked.then_some(right_hand_skeleton).flatten(),
                                hand_skeleton_config.steamvr_input_2_0,
                                hand_skeleton_config.predict,
                            )
                        } else {
                            (None, None, false, false)
                        };

                        let ffi_left_hand_data = FfiHandData {
                            controllerMotion: if let Some(motion) = &ffi_left_controller_motion {
                                motion
                            } else {
                                ptr::null()
                            },
                            handSkeleton: if let Some(skeleton) = &ffi_left_hand_skeleton {
                                skeleton
                            } else {
                                ptr::null()
                            },
                            isHandTracker: use_separate_hand_trackers
                                && ffi_left_controller_motion.is_none()
                                && ffi_left_hand_skeleton.is_some(),
                            predictHandSkeleton: predict_hand_skeleton,
                        };
                        let ffi_right_hand_data = FfiHandData {
                            controllerMotion: if let Some(motion) = &ffi_right_controller_motion {
                                motion
                            } else {
                                ptr::null()
                            },
                            handSkeleton: if let Some(skeleton) = &ffi_right_hand_skeleton {
                                skeleton
                            } else {
                                ptr::null()
                            },
                            isHandTracker: use_separate_hand_trackers
                                && ffi_right_controller_motion.is_none()
                                && ffi_right_hand_skeleton.is_some(),
                            predictHandSkeleton: predict_hand_skeleton,
                        };

                        let ffi_body_tracker_motions = if track_body || detached_controllers {
                            tracking::BODY_TRACKER_IDS
                                .iter()
                                .filter_map(|id| {
                                    Some(tracking::to_ffi_motion(
                                        *id,
                                        context.get_device_motion(*id, poll_timestamp)?,
                                    ))
                                })
                                .collect::<Vec<_>>()
                        } else {
                            vec![]
                        };

                        // There are two pairs of controllers/hand tracking devices registered in
                        // OpenVR, two lefts and two rights. If enabled with use_separate_hand_trackers,
                        // we select at runtime which device to use (selected for left and right hand
                        // independently. Selection is done by setting deviceIsConnected.
                        unsafe {
                            SetTracking(
                                poll_timestamp.as_nanos() as _,
                                if pose_time.is_some() { sample_offset as f32 } else { controllers_pose_time_offset.as_secs_f32() },
                                ffi_head_motion,
                                ffi_left_hand_data,
                                ffi_right_hand_data,
                                ffi_body_tracker_motions.as_ptr(),
                                ffi_body_tracker_motions.len() as i32,
                            )
                        };
                    }
                }
                ServerCoreEvent::Buttons(entries) => {
                    for entry in entries {
                        let value = match entry.value {
                            ButtonValue::Binary(value) => FfiButtonValue {
                                type_: FfiButtonType_BUTTON_TYPE_BINARY,
                                __bindgen_anon_1: FfiButtonValue__bindgen_ty_1 {
                                    binary: value.into(),
                                },
                            },

                            ButtonValue::Scalar(value) => FfiButtonValue {
                                type_: FfiButtonType_BUTTON_TYPE_SCALAR,
                                __bindgen_anon_1: FfiButtonValue__bindgen_ty_1 { scalar: value },
                            },
                        };
                        unsafe { SetButton(entry.path_id, value) };
                    }
                }
                ServerCoreEvent::FrameSkeleton(value) => {
                    let enabled = surfxr_server_core::settings()
                        .headset
                        .controllers
                        .as_option()
                        .is_some_and(|config| {
                            matches!(config.emulation_mode, ControllersEmulationMode::SteamFrame)
                                && config.hand_skeleton.enabled()
                        });
                    let skeleton = enabled
                        .then_some(value.bones.as_ref())
                        .flatten()
                        .map(|bones| FfiHandSkeleton {
                            jointPositions: std::array::from_fn(|i| bones[i].position.to_array()),
                            jointRotations: std::array::from_fn(|i| {
                                let q = bones[i].orientation;
                                FfiQuat {
                                    x: q.x,
                                    y: q.y,
                                    z: q.z,
                                    w: q.w,
                                }
                            }),
                        });
                    unsafe {
                        SetFrameSkeleton(
                            value.hand,
                            value.sampled_ns,
                            skeleton.as_ref().map_or(ptr::null(), |v| v),
                        );
                    }
                }
                ServerCoreEvent::RequestIDR => unsafe { RequestIDR() },
                ServerCoreEvent::GameRenderLatencyFeedback(game_latency) => {
                    if cfg!(target_os = "linux") && game_latency.as_secs_f32() > 0.25 {
                        let now = Instant::now();
                        if now.saturating_duration_since(last_resync).as_secs_f32() > 0.1 {
                            last_resync = now;
                            warn!("Desync detected. Attempting recovery.");
                            unsafe {
                                RequestDriverResync();
                            }
                        }
                    }
                }
                ServerCoreEvent::ShutdownPending | ServerCoreEvent::RestartPending => {
                    // Dropping the context
                    SERVER_CORE_CONTEXT.write().take();

                    // todo: send different HUD message for shutdown or restart
                    unsafe { ShutdownSteamvr() };
                }
                ServerCoreEvent::ProximityState(headset_is_worn) => unsafe {
                    SetProximityState(headset_is_worn)
                },
            }
        }

        if streaming_initialized || frame_media_lease.is_some() {
            unsafe { DeinitializeStreaming() };
            frame_media_lease.take();
        }
        unsafe { ShutdownOpenvrClient() };
    });
    *EVENT_LOOP_HANDLE.lock() = Some(handle);
}
