use super::*;

pub(super) fn spawn(ctx: Arc<ConnectionContext>, initial_settings: &Settings, client_hostname: String,
    frame_daemon: bool, game_audio_sender: surfxr_sockets::StreamSender<()>,
    mut microphone_receiver: surfxr_sockets::StreamReceiver<()>, game_audio_sample_rate: u32,
    microphone_sample_rate: u32) -> (thread::JoinHandle<()>, thread::JoinHandle<()>) {
    #[cfg(not(target_os = "linux"))]
    let _ = game_audio_sample_rate;
    #[cfg_attr(target_os = "linux", expect(unused_variables))]
    let game_audio_thread = if let Switch::Enabled(config) =
        if frame_daemon {Switch::Disabled}else{initial_settings.audio.game_audio.clone()}
    {
        #[cfg(windows)]
        let ctx = Arc::clone(&ctx);

        let client_hostname = client_hostname.clone();
        thread::spawn(move || {
            #[cfg(not(target_os = "linux"))]
            while is_streaming(&client_hostname) {
                {
                    let device = match surfxr_audio::new_output(config.device.as_ref()) {
                        Ok(data) => data,
                        Err(e) => {
                            warn!("New audio device failed: {e:?}");
                            thread::sleep(RETRY_CONNECT_MIN_INTERVAL);
                            continue;
                        }
                    };

                    #[cfg(windows)]
                    if let Ok(id) = surfxr_audio::get_windows_device_id(&device) {
                        let prop = surfxr_session::OpenvrProperty {
                            key: surfxr_session::OpenvrPropKey::AudioDefaultPlaybackDeviceIdString,
                            value: id,
                        };
                        ctx.events_sender
                            .send(ServerCoreEvent::SetOpenvrProperty {
                                device_id: *surfxr_common::HEAD_ID,
                                prop,
                            })
                            .ok();
                    } else {
                        continue;
                    };

                    if let Err(e) = surfxr_audio::record_audio_blocking(
                        Arc::new({
                            let client_hostname = client_hostname.clone();
                            move || is_streaming(&client_hostname)
                        }),
                        game_audio_sender.clone(),
                        &device,
                        2,
                        config.mute_when_streaming,
                    ) {
                        error!("Audio record error: {e:?}");
                    }

                    #[cfg(windows)]
                    if let Ok(id) = surfxr_audio::new_output(None)
                        .and_then(|d| surfxr_audio::get_windows_device_id(&d))
                    {
                        let prop = surfxr_session::OpenvrProperty {
                            key: surfxr_session::OpenvrPropKey::AudioDefaultPlaybackDeviceIdString,
                            value: id,
                        };
                        ctx.events_sender
                            .send(ServerCoreEvent::SetOpenvrProperty {
                                device_id: *surfxr_common::HEAD_ID,
                                prop,
                            })
                            .ok();
                    }
                }
            }
        })
    } else {
        thread::spawn(|| ())
    };

    #[cfg(not(target_os = "linux"))]
    let microphone_thread =
        if let Switch::Enabled(config) = if frame_daemon{Switch::Disabled}else{initial_settings.audio.microphone.clone()} {
            let ctx = Arc::clone(&ctx);
            let client_hostname = client_hostname.clone();
            thread::spawn(move || {
                crate::microphone::run(
                    ctx,
                    || is_streaming(&client_hostname),
                    config,
                    microphone_sample_rate,
                    &mut microphone_receiver,
                );
            })
        } else {
            thread::spawn(|| ())
        };

    #[cfg(target_os = "linux")]
    let microphone_thread = {
        use surfxr_audio::linux::{self, AudioInfo};
        let mic = if let Switch::Enabled(config) = initial_settings.audio.microphone.clone() {
            Some((
                AudioInfo {
                    sample_rate: microphone_sample_rate,
                    channel_count: 1,
                },
                config.buffering,
            ))
        } else {
            None
        };

        let audio_info = initial_settings
            .audio
            .game_audio
            .enabled()
            .then_some(AudioInfo {
                sample_rate: game_audio_sample_rate,
                channel_count: 2,
            });

        if mic.is_some() || audio_info.is_some() {
            let client_hostname = client_hostname.clone();
            thread::spawn(move || {
                linux::audio_loop(
                    {
                        let client_hostname = client_hostname.clone();
                        move || is_streaming(&client_hostname)
                    },
                    game_audio_sender,
                    audio_info,
                    &mut microphone_receiver,
                    mic,
                );
            })
        } else {
            thread::spawn(|| ())
        }
    };

    (game_audio_thread, microphone_thread)
}
