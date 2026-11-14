use super::*;

pub(super) fn spawn(ctx: Arc<ConnectionContext>, initial_settings: Settings, client_hostname: String,
    controllers_config: Option<surfxr_session::ControllersConfig>,
    control_sender: Arc<Mutex<surfxr_sockets::ControlSocketSender<ServerControlPacket>>>,
    mut control_receiver: surfxr_sockets::ControlSocketReceiver<ClientControlPacket>, disconnect_notif: Arc<Condvar>,
    frame_boundary_sync: bool, frame_controller_state: bool, frame_pose_time: bool) -> thread::JoinHandle<()> {
    thread::spawn({
        let ctx = Arc::clone(&ctx);

        let controllers_config = controllers_config.as_ref();
        let mut controller_button_mapping_manager = controllers_config.map(|config| {
            if let Some(mappings) = &config.button_mappings {
                ButtonMappingManager::new_manual(mappings)
            } else {
                ButtonMappingManager::new_automatic(
                    &CONTROLLER_PROFILE_INFO
                        .get(&surfxr_common::hash_string(QUEST_CONTROLLER_PROFILE_PATH))
                        .unwrap()
                        .button_set,
                    &config.emulation_mode,
                    &config.button_mapping_config,
                )
            }
        });
        let controllers_emulation_mode =
            controllers_config.map(|config| config.emulation_mode.clone());

        let disconnect_notif = Arc::clone(&disconnect_notif);
        let control_sender = Arc::clone(&control_sender);
        let client_hostname = client_hostname.clone();
        move || {
            let mut disconnection_deadline = Instant::now() + KEEPALIVE_TIMEOUT;
            while is_streaming(&client_hostname) {
                let packet = match control_receiver.recv(STREAMING_RECV_TIMEOUT) {
                    Ok(packet) => packet,
                    Err(ConnectionError::TryAgain(_)) => {
                        if Instant::now() > disconnection_deadline {
                            info!("Client disconnected. Timeout");
                            break;
                        } else {
                            continue;
                        }
                    }
                    Err(e) => {
                        info!("Client disconnected. Cause: {e}");
                        break;
                    }
                };

                let control_received_ns = crate::frame_pose::now_ns();
                match packet {
                    ClientControlPacket::PlayspaceSync(packet) => {
                        if !initial_settings.headset.tracking_ref_only {
                            let session_manager_lock = SESSION_MANAGER.read();
                            let config = &session_manager_lock.settings().headset;
                            crate::frame_boundary::recenter(&mut ctx.tracking_manager.write(),
                                &config.recentering_mode, frame_boundary_sync);

                            if !frame_boundary_sync {
                                let area = packet.unwrap_or(Vec2::new(2.0, 2.0));
                                let wh = area.x * area.y;
                                if wh.is_finite() && wh > 0.0 {
                                    info!("Received new playspace with size: {}", area);
                                    ctx.events_sender
                                        .send(ServerCoreEvent::PlayspaceSync(area))
                                        .ok();
                                } else {
                                    warn!("Received invalid playspace size: {}", area);
                                    ctx.events_sender
                                        .send(ServerCoreEvent::PlayspaceSync(Vec2::new(2.0, 2.0)))
                                        .ok();
                                }
                            }
                        }
                    }
                    ClientControlPacket::RequestIdr => {
                        if let Some(config) = ctx.decoder_config.lock().clone() {
                            control_sender
                                .lock()
                                .send(&ServerControlPacket::DecoderConfig(config))
                                .ok();
                        }
                        ctx.events_sender.send(ServerCoreEvent::RequestIDR).ok();
                    }
                    ClientControlPacket::LocalViewParams(params) => {
                        ctx.events_sender
                            .send(ServerCoreEvent::LocalViewParams(params))
                            .ok();
                    }
                    ClientControlPacket::Battery(packet) => {
                        ctx.events_sender
                            .send(ServerCoreEvent::Battery(packet.clone()))
                            .ok();

                        if let Some(stats) = &mut *ctx.statistics_manager.write() {
                            stats.report_battery(
                                packet.device_id,
                                packet.gauge_value,
                                packet.is_plugged,
                            );
                        }
                    }
                    ClientControlPacket::Buttons(entries) => {
                        {
                            let session_manager_lock = SESSION_MANAGER.read();
                            if session_manager_lock
                                .settings()
                                .extra
                                .logging
                                .log_button_presses
                            {
                                surfxr_events::send_event(EventType::Buttons(
                                    entries
                                        .iter()
                                        .map(|e| ButtonEvent {
                                            path: BUTTON_INFO.get(&e.path_id).map_or_else(
                                                || format!("Unknown (ID: {:#16x})", e.path_id),
                                                |info| info.path.to_owned(),
                                            ),
                                            value: e.value,
                                        })
                                        .collect(),
                                ));
                            }
                        }

                        if let Some(manager) = &mut controller_button_mapping_manager {
                            let button_entries = entries
                                .iter()
                                .flat_map(|entry| manager.map_button(entry))
                                .collect::<Vec<_>>();

                            if !button_entries.is_empty() {
                                ctx.events_sender
                                    .send(ServerCoreEvent::Buttons(button_entries))
                                    .ok();
                            }
                        };
                    }
                    ClientControlPacket::ActiveInteractionProfile { input_ids, .. } => {
                        controller_button_mapping_manager = if let Switch::Enabled(config) =
                            &SESSION_MANAGER.read().settings().headset.controllers
                        {
                            if let Some(mappings) = &config.button_mappings {
                                Some(ButtonMappingManager::new_manual(mappings))
                            } else {
                                controllers_emulation_mode.as_ref().map(|emulation_mode| {
                                    ButtonMappingManager::new_automatic(
                                        &input_ids,
                                        emulation_mode,
                                        &config.button_mapping_config,
                                    )
                                })
                            }
                        } else {
                            None
                        };
                    }
                    ClientControlPacket::Log { level, message } => {
                        info!("Client {client_hostname}: [{level:?}] {message}")
                    }
                    ClientControlPacket::KeepAlive | ClientControlPacket::StreamReady => (),
                    ClientControlPacket::ProximityState(headset_is_worn) => {
                        ctx.events_sender
                            .send(ServerCoreEvent::ProximityState(headset_is_worn))
                            .ok();
                    }
                    ClientControlPacket::ReservedBuffer(bytes) => {
                        if frame_pose_time {
                            let mut sender = control_sender.lock();
                            if let Some(reply) = crate::frame_pose::clock_reply(&ctx, &bytes, control_received_ns) {
                                sender.send(&ServerControlPacket::ReservedBuffer(reply)).ok();
                            }
                        }
                        if frame_controller_state
                            && let Some(state) = crate::frame_controller_state::decode(&bytes) {
                            ctx.events_sender.send(ServerCoreEvent::FrameControllerState(state)).ok();
                        }
                        if frame_boundary_sync {
                            crate::frame_boundary::receive(&ctx, &bytes);
                        }
                        if matches!(controllers_emulation_mode.as_ref(), Some(ControllersEmulationMode::SteamFrame))
                            && let Some(skeleton) = crate::frame_skeleton::decode(&bytes) {
                            ctx.events_sender.send(ServerCoreEvent::FrameSkeleton(skeleton)).ok();
                        }
                    }
                    ClientControlPacket::Reserved(_) => (),
                }

                disconnection_deadline = Instant::now() + KEEPALIVE_TIMEOUT;
            }

            disconnect_notif.notify_one();
        }
    })
}
