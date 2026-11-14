use crate::{ConnectionContext, ServerCoreEvent};
use surfxr_common::{anyhow::Result, info, warn};
use surfxr_session::MicrophoneConfig;
use surfxr_sockets::StreamReceiver;
use std::{
    sync::Arc,
    thread,
    time::{Duration, Instant},
};

// Keep the network subscription alive across endpoint removal/re-enable and
// WASAPI render failures. Only reopen the configured virtual cable pair.
pub(crate) fn run(
    ctx: Arc<ConnectionContext>,
    is_streaming: impl Fn() -> bool,
    config: MicrophoneConfig,
    sample_rate: u32,
    receiver: &mut StreamReceiver<()>,
) {
    let mut failures = 0u64;
    while is_streaming() {
        let mut attempt = || -> Result<()> {
            let (sink, source) = surfxr_audio::new_virtual_microphone_pair(config.devices.clone())?;
            #[cfg(windows)]
            if let Some(previous_db) = surfxr_audio::cap_steam_microphone_boost_windows(&source)? {
                info!("Steam microphone capture boost reduced: {previous_db} dB -> 0 dB");
            }
            #[cfg(windows)]
            if let Ok(id) = surfxr_audio::get_windows_device_id(&source) {
                ctx.events_sender
                    .send(ServerCoreEvent::SetOpenvrProperty {
                        device_id: *surfxr_common::HEAD_ID,
                        prop: surfxr_session::OpenvrProperty {
                            key: surfxr_session::OpenvrPropKey::AudioDefaultRecordingDeviceIdString,
                            value: id,
                        },
                    })
                    .ok();
            }
            #[cfg(not(windows))]
            let _ = (&ctx, &source);
            surfxr_audio::play_audio_loop(
                &is_streaming,
                &sink,
                1,
                sample_rate,
                config.buffering.clone(),
                receiver,
            )
        };
        match attempt() {
            Ok(()) => break,
            Err(_) if is_streaming() => {
                failures += 1;
                warn!(
                    "Headset microphone output failed; retrying configured device in 1 s (failures={failures})"
                );
                let deadline = Instant::now() + Duration::from_secs(1);
                while is_streaming() && Instant::now() < deadline {
                    thread::sleep(Duration::from_millis(20));
                }
            }
            Err(_) => break,
        }
    }
    info!("Headset microphone output stopped: failures={failures}");
}
