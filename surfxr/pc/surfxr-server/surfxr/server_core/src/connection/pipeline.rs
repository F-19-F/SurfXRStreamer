use super::*;

pub(super) fn connection_pipeline(
    ctx: Arc<ConnectionContext>,
    lifecycle_state: Arc<RwLock<LifecycleState>>,
    socket: ProtoControlSocket,
    connection_result: ClientConnectionResult,
    client_hostname: String,
    client_ip: IpAddr,
) -> ConResult {
    dbg_connection!("connection_pipeline: Begin");
    if crate::frame_desktop_tracking::requested(&connection_result) {
        let boundary = match &connection_result {
            ClientConnectionResult::ConnectionAccepted(info) => info.streaming_capabilities.as_ref()
                .is_some_and(|caps| crate::frame_boundary::requested(&caps.ext_str)),
            _ => false,
        };
        let optics = crate::frame_desktop_tracking::optics_requested(&connection_result);
        return crate::frame_desktop_tracking::run(&ctx, lifecycle_state, socket, &client_hostname, client_ip, boundary, optics);
    }
    // Wait without the session lock so the current desktop owner's admission
    // heartbeat and Dashboard settings remain responsive during teardown.
    let frame_daemon=client_ip.is_loopback()&&match &connection_result {
        ClientConnectionResult::ConnectionAccepted(info)=>info.streaming_capabilities.as_ref()
            .and_then(|caps|serde_json::from_str::<serde_json::Value>(&caps.ext_str).ok())
            .is_some_and(|value|value["frame_daemon"].as_u64()==Some(1)),_=>false,
    };
    // The paired daemon owns the encoder lease. A suspended vrserver must not
    // hold desktop admission hostage after its actual encoder has retired.
    let frame_media_lease=if frame_daemon {None}else{Some(Arc::new(surfxr_server_io::frame_media_lock::FrameMediaLease::acquire().map_err(|e|surfxr_common::anyhow::anyhow!(e)).to_con()?))};

    // This session lock will make sure settings and client list cannot be changed while connecting
    // to thos client, no other client can connect until handshake is finished. It will then be
    // temporarily relocked while shutting down the threads.
    let mut session_manager_lock = SESSION_MANAGER.write();

    dbg_connection!("connection_pipeline: Setting client state in session");
    session_manager_lock.update_client_connections(
        client_hostname.clone(),
        ClientConnectionsAction::SetConnectionState(ConnectionState::Connecting),
    );
    session_manager_lock.update_client_connections(
        client_hostname.clone(),
        ClientConnectionsAction::UpdateCurrentIp(Some(match &connection_result {
            ClientConnectionResult::ConnectionAccepted(info)=>info.streaming_capabilities.as_ref().map_or(client_ip,|caps|crate::frame_origin::ip(&caps.ext_str,client_ip)),_=>client_ip,
        })),
    );

    let maybe_streaming_caps =
        if let ClientConnectionResult::ConnectionAccepted(info) = connection_result {
            session_manager_lock.update_client_connections(
                client_hostname.clone(),
                ClientConnectionsAction::SetDisplayName(info.platform_string),
            );

            if info.client_protocol_id != surfxr_common::protocol_id_u64() {
                warn!(
                    "Trusted client is incompatible! Expected protocol ID: {}, found: {}",
                    surfxr_common::protocol_id_u64(),
                    info.client_protocol_id,
                );

                return Ok(());
            }

            info.streaming_capabilities
        } else {
            debug!("Found client in standby. Retrying");
            return Ok(());
        };

    let Some(streaming_caps) = maybe_streaming_caps else {
        con_bail!("Only streaming clients are supported for now");
    };

    let mut initial_settings = if frame_daemon { crate::runtime_settings() }
        else { session_manager_lock.settings().clone() };
    // Bitrate remains live. Restart-dependent options use this driver's launch snapshot.
    initial_settings.video.bitrate = session_manager_lock.settings().video.bitrate.clone();

    let negotiation::Negotiated { transcoding_view_resolution, emulated_headset_view_resolution,
        fps, foveated_encoding, encoder_profile, enable_10_bits_encoding, enable_hdr, encoding_gamma, codec,
        game_audio_sample_rate, stream_protocol, priority_port, video_fec, video_haptics, frame_gaze,
        image_timestamps, frame_lifecycle, frame_boundary_sync, frame_pose_time, frame_clock_calibration,
        frame_controller_state, stream_config_packet } = negotiation::prepare(
        &initial_settings, &streaming_caps, session_manager_lock.session(), frame_daemon, client_ip)?;

    let new_steamvr_hmd_init_config = SteamvrHmdInitConfig {
        eye_resolution_width: transcoding_view_resolution.x,
        eye_resolution_height: transcoding_view_resolution.y,
        target_eye_resolution_width: emulated_headset_view_resolution.x,
        target_eye_resolution_height: emulated_headset_view_resolution.y,
        refresh_rate: fps as _,
    };
    let new_hash = compute_restart_settings_hash(&new_steamvr_hmd_init_config, &initial_settings);
    if !frame_daemon && session_manager_lock.session().restart_settings_hash != new_hash {
        let mut session = session_manager_lock.session_mut();
        session.steamvr_hmd_init_config = new_steamvr_hmd_init_config;
        session.restart_settings_hash = new_hash;

        if frame_daemon {
            // This trusted daemon session was already admitted. SteamVR may
            // relaunch before Dashboard receives the restart event; retain the
            // existing bounded, single-use authorization through that restart.
            surfxr_server_io::authorize_next_driver_start(
                &FILESYSTEM_LAYOUT.get().unwrap().session(),
            ).to_con()?;
        }
        surfxr_sockets::send_restart_signal(socket, stream_config_packet)?;

        ctx.frame_sessions.restarting();
        crate::notify_restart_driver();

        *lifecycle_state.write() = LifecycleState::ShuttingDown;

        return Ok(());
    }

    dbg_connection!("connection_pipeline: Finishing handshake");
    let mut socket = SocketConnection::from_client_connection_split(
        socket,
        HANDSHAKE_ACTION_TIMEOUT,
        stream_config_packet,
        StreamSocketConfig {
            protocol: stream_protocol,
            port: initial_settings.connection.stream_port,
            buffer_config: initial_settings.connection.server_buffer_config,
            max_packet_size: initial_settings.connection.packet_size as _,
            dscp: initial_settings.connection.dscp,
        },
        priority_port,
    )?;

    socket.configure_frame_udp_video(&initial_settings.video.bitrate);
    if frame_daemon { socket.configure_frame_daemon_poll().to_con()?; }
    socket.configure_frame_video_fec(video_fec);
    dbg_connection!("connection_pipeline: Handshake successful, spawning threads");

    let disconnect_notif = Arc::new(Condvar::new());

    *ctx.statistics_manager.write() = Some(StatisticsManager::new(
        initial_settings.connection.statistics_history_size,
        Duration::from_secs_f32(1.0 / fps),
        if let Switch::Enabled(config) = &initial_settings.headset.controllers {
            config.steamvr_pipeline_frames
        } else {
            0.0
        },
    ));
    *ctx.bitrate_manager.lock() =
        BitrateManager::new(initial_settings.video.bitrate.history_size, fps);
    *ctx.tracking_manager.write() =
        TrackingManager::new(initial_settings.connection.statistics_history_size);

    let control_sender = Arc::new(Mutex::new(socket.request_reliable_stream()?));
    if frame_lifecycle { ctx.frame_sessions.register(client_hostname.clone(), Arc::clone(&control_sender)); }
    let video_sender =
        socket.request_unreliable_stream::<surfxr_sockets::FrameVideoHeader>(VIDEO);
    #[cfg(windows)]
    if (frame_gaze.is_some()||frame_daemon) && priority_port.is_some() && matches!(stream_protocol, SocketProtocol::Udp) {
        let mut pacing = match initial_settings.video.bitrate.mode {
            surfxr_session::BitrateMode::ConstantMbps(mbps) => mbps.saturating_mul(2_000_000) / 8,
            _ => 0,
        };
        if frame_daemon {
            let overhead=serde_json::from_str::<serde_json::Value>(&streaming_caps.ext_str).ok()
                .and_then(|caps|caps["frame_daemon_media_overhead"].as_u64())
                .filter(|value|matches!(value,20|28)).unwrap_or(28);
            let size=initial_settings.connection.packet_size as u64;pacing=pacing.saturating_mul(size)/(size+overhead);
        }
        *ctx.external_video_transport.lock() = Some(crate::external_video::ExternalVideoTransport {
            sender: video_sender.external_owner_endpoint(),
            packet_size: initial_settings.connection.packet_size as u32,
            queue_capacity: initial_settings.connection.max_queued_server_video_frames.clamp(1, 16) as u32,
            pacing_bytes_per_second: pacing, fec: video_fec, haptics: video_haptics,
            avoid_glitching: initial_settings.connection.avoid_video_glitching,
            image_timestamps,
            daemon: frame_daemon, gaze: frame_gaze.is_some(),
        });
    }
    let game_audio_sender: surfxr_sockets::StreamSender<()> = socket.request_unreliable_stream(AUDIO);
    let haptics_sender = socket.request_unreliable_stream(HAPTICS);

    let control_receiver = socket.subscribe_to_reliable_stream()?;
    let microphone_receiver: surfxr_sockets::StreamReceiver<()> =
        socket.subscribe_to_unreliable_stream(AUDIO, MAX_UNREAD_PACKETS);
    let tracking_receiver =
        socket.subscribe_to_unreliable_stream::<TrackingData>(TRACKING, MAX_UNREAD_PACKETS);
    let statics_receiver =
        socket.subscribe_to_unreliable_stream::<ClientStatistics>(STATISTICS, MAX_UNREAD_PACKETS);

    let (video_channel_sender, video_channel_receiver) =
        std::sync::mpsc::sync_channel(initial_settings.connection.max_queued_server_video_frames);
    *ctx.video_channel_sender.lock() = Some(video_channel_sender);
    *ctx.frame_gaze.lock() = frame_gaze.map(|config| crate::frame_gaze::FrameGazeState::new(config, transcoding_view_resolution.to_array()));
    *ctx.frame_pose.lock() = frame_pose_time.then(||crate::frame_pose::FramePoseState::new(frame_clock_calibration));
    if frame_pose_time { info!("Frame pose time v1: fresh head/grip poses, SteamVR prediction, submitted image pose"); }
    if frame_clock_calibration { info!("Frame clock calibration: measured offset/drift/error, continuous holdover, quality is not liveness"); }
    *ctx.frame_video_haptics.lock() =
        video_haptics.then(crate::haptics::FrameVideoHaptics::default);
    *ctx.haptics_sender.lock() = if video_haptics {
        None
    } else {
        Some(haptics_sender)
    };

    let video_send_thread = VideoSendWorker::spawn(video_sender, video_channel_receiver,
        Arc::clone(&ctx.frame_video_haptics), video_haptics, image_timestamps);

    let (game_audio_thread, microphone_thread) = audio::spawn(Arc::clone(&ctx), &initial_settings,
        client_hostname.clone(), frame_daemon, game_audio_sender, microphone_receiver,
        game_audio_sample_rate, streaming_caps.microphone_sample_rate);

    // Queue the per-session gate before either receive worker can publish data.
    // Negotiated clients start offline until their reliable initial state arrives.
    ctx.events_sender.send(ServerCoreEvent::FrameControllerState(
        crate::frame_controller_state::ControllerState::session(frame_controller_state))).ok();
    let tracking_receive_thread = thread::spawn({
        let ctx = Arc::clone(&ctx);
        let initial_settings = initial_settings.clone();
        let client_hostname = client_hostname.clone();
        move || {
            tracking::tracking_loop(&ctx, initial_settings, tracking_receiver, || {
                is_streaming(&client_hostname)
            });
        }
    });

    let statistics_thread = statistics::spawn(Arc::clone(&ctx), client_hostname.clone(), statics_receiver);

    let real_time_update_thread = thread::spawn({
        let control_sender = Arc::clone(&control_sender);
        let client_hostname = client_hostname.clone();
        move || {
            let mut previous_config = None;
            while is_streaming(&client_hostname) {
                let config = {
                    let session_manager_lock = SESSION_MANAGER.read();
                    let settings = session_manager_lock.settings();

                    RealTimeConfig::from_settings(settings)
                };

                let same_config = previous_config.as_ref().is_some_and(|prev| config == *prev);
                if !same_config {
                    previous_config = Some(config.clone());

                    control_sender
                        .lock()
                        .send(&ServerControlPacket::RealTimeConfig(config))
                        .ok();
                }

                thread::sleep(REAL_TIME_UPDATE_INTERVAL);
            }
        }
    });

    let keepalive_thread = thread::spawn({
        let control_sender = Arc::clone(&control_sender);
        let disconnect_notif = Arc::clone(&disconnect_notif);
        let client_hostname = client_hostname.clone();
        move || {
            while is_streaming(&client_hostname) {
                if let Err(e) = control_sender.lock().send(&ServerControlPacket::KeepAlive) {
                    info!("Client disconnected. Cause: {e:?}");

                    disconnect_notif.notify_one();

                    return;
                }

                thread::sleep(KEEPALIVE_INTERVAL);
            }
        }
    });

    let control_receive_thread = control::spawn(Arc::clone(&ctx), initial_settings.clone(),
        client_hostname.clone(), session_manager_lock.settings().headset.controllers.clone().into_option(),
        Arc::clone(&control_sender), control_receiver, Arc::clone(&disconnect_notif),
        frame_boundary_sync,
        frame_controller_state, frame_pose_time);

    let stream_receive_thread = thread::spawn({
        let disconnect_notif = Arc::clone(&disconnect_notif);
        let client_hostname = client_hostname.clone();
        move || {
            while is_streaming(&client_hostname) {
                match socket.recv_poll() {
                    Ok(()) => (),
                    Err(ConnectionError::TryAgain(_)) => continue,
                    Err(e) => {
                        info!("Client disconnected. Cause: {e}");

                        disconnect_notif.notify_one();

                        return;
                    }
                }
            }
        }
    });

    let lifecycle_check_thread = thread::spawn({
        let disconnect_notif = Arc::clone(&disconnect_notif);
        let client_hostname = client_hostname.clone();
        move || {
            while SESSION_MANAGER
                .read()
                .client_list()
                .get(&client_hostname)
                .is_some_and(|c| c.connection_state == ConnectionState::Streaming)
                && *lifecycle_state.read() == LifecycleState::Resumed
            {
                thread::sleep(STREAMING_RECV_TIMEOUT);
            }

            disconnect_notif.notify_one()
        }
    });

    if initial_settings.connection.enable_on_connect_script {
        let on_connect_script = FILESYSTEM_LAYOUT.get().map(|l| l.connect_script()).unwrap();
        info!(
            "Running on connect script (connect): {}",
            on_connect_script.display()
        );
        if let Err(e) = Command::new(&on_connect_script)
            .env("ACTION", "connect")
            .spawn()
        {
            warn!("Failed to run connect script: {e}");
        }
    }
    if initial_settings.extra.capture.startup_video_recording {
        info!("Creating recording file");
        crate::create_recording_file(&ctx, session_manager_lock.settings());
    }

    session_manager_lock.update_client_connections(
        client_hostname.clone(),
        ClientConnectionsAction::SetConnectionState(ConnectionState::Streaming),
    );

    ctx.events_sender
        .send(ServerCoreEvent::ClientConnected(
            ServerNegotiatedStreamingConfig {
                frame_media_lease: frame_media_lease.clone(),
                frame_gaze,
                transcoding_view_resolution,
                emulated_headset_view_resolution: transcoding_view_resolution,
                refresh_rate: fps as _,
                foveated_encoding,
                codec,
                h264_profile: encoder_profile,
                use_10bit_encoder: enable_10_bits_encoding,
                encoding_gamma,
                enable_hdr,
            },
        ))
        .ok();

    if frame_daemon {
        let extension=serde_json::from_str::<serde_json::Value>(&streaming_caps.ext_str).unwrap_or_default();
        for (field,key) in [
            ("frame_daemon_playback_id",surfxr_session::OpenvrPropKey::AudioDefaultPlaybackDeviceIdString),
            ("frame_daemon_recording_id",surfxr_session::OpenvrPropKey::AudioDefaultRecordingDeviceIdString),
        ] {
            if let Some(id)=extension[field].as_str().filter(|id|!id.is_empty()&&id.len()<4096) {
                ctx.events_sender.send(ServerCoreEvent::SetOpenvrProperty {device_id:*surfxr_common::HEAD_ID,
                    prop:surfxr_session::OpenvrProperty {key,value:id.into()}}).ok();
            }
        }
    }
    dbg_connection!("connection_pipeline: Threads initialized; unlocking streams");
    surfxr_common::wait_rwlock(&disconnect_notif, &mut session_manager_lock);
    dbg_connection!("connection_pipeline: Begin connection shutdown");
    ctx.frame_sessions.remove(&client_hostname);

    // This requests shutdown from threads
    video_send_thread.request_stop();
    *ctx.external_video_haptics_observer.lock() = None;
    *ctx.external_video_transport.lock() = None;
    *ctx.video_channel_sender.lock() = None;
    *ctx.haptics_sender.lock() = None;
    *ctx.frame_video_haptics.lock() = None;
    *ctx.frame_gaze.lock() = None;
    if let Some(state) = ctx.frame_pose.lock().as_mut() { state.stop(); }

    *ctx.video_recording_file.lock() = None;

    session_manager_lock.update_client_connections(
        client_hostname,
        ClientConnectionsAction::SetConnectionState(ConnectionState::Disconnecting),
    );

    let enable_on_disconnect_script = session_manager_lock
        .settings()
        .connection
        .enable_on_disconnect_script;
    if enable_on_disconnect_script {
        let on_disconnect_script = FILESYSTEM_LAYOUT
            .get()
            .map(|l| l.disconnect_script())
            .unwrap();
        info!(
            "Running on disconnect script (disconnect): {}",
            on_disconnect_script.display()
        );
        if let Err(e) = Command::new(&on_disconnect_script)
            .env("ACTION", "disconnect")
            .spawn()
        {
            warn!("Failed to run disconnect script: {e}");
        }
    }

    // Allow threads to shutdown correctly
    drop(session_manager_lock);

    // Ensure shutdown of threads
    dbg_connection!("connection_pipeline: Shutdown threads");
    video_send_thread.join();
    game_audio_thread.join().ok();
    microphone_thread.join().ok();
    tracking_receive_thread.join().ok();
    statistics_thread.join().ok();
    real_time_update_thread.join().ok();
    control_receive_thread.join().ok();
    stream_receive_thread.join().ok();
    keepalive_thread.join().ok();
    lifecycle_check_thread.join().ok();

    ctx.events_sender
        .send(ServerCoreEvent::ClientDisconnected)
        .ok();

    dbg_connection!("connection_pipeline: End");

    Ok(())
}
