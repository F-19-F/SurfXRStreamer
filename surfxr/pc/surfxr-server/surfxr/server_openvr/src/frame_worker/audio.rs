//! Daemon microphone playback reuses SURFXR device selection and recovery.
use surfxr_common::settings_schema::Switch;
use surfxr_session::AudioConfig;
use std::{
    ffi::c_void,
    thread,
    time::{Duration, Instant},
};

#[repr(C)]
pub struct AudioCallbacks {
    owner: *mut c_void,
    stopped: extern "C" fn(*mut c_void) -> bool,
    receive: extern "C" fn(*mut c_void, *mut u8, usize, *mut bool) -> i32,
}
fn config(bytes: *const u8, size: usize) -> Option<AudioConfig> {
    if bytes.is_null() || size == 0 || size > 65536 {
        return None;
    }
    serde_json::from_slice(unsafe { std::slice::from_raw_parts(bytes, size) }).ok()
}
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonAudioConfig(
    bytes: *const u8,
    size: usize,
    device: *mut u16,
    capacity: usize,
) -> u32 {
    let Some(config) = config(bytes, size) else {
        return 0;
    };
    let metadata: serde_json::Value = serde_json::from_slice(unsafe { std::slice::from_raw_parts(bytes, size) }).unwrap_or_default();
    let mut flags = if metadata["frame_daemon_media_aes"].as_bool()==Some(true) {16} else {0};
    if matches!(config.microphone, Switch::Enabled(_)) {
        flags |= 2;
    }
    if let Switch::Enabled(game) = config.game_audio {
        flags |= 1;
        if game.mute_when_streaming {
            flags |= 8;
        }
        if let Some(id) = metadata["frame_daemon_playback_id"]
            .as_str()
            .filter(|id| !id.is_empty() && id.len() < 4096)
        {
            let wide: Vec<_> = id.encode_utf16().chain(Some(0)).collect();
            if !device.is_null() && wide.len() <= capacity {
                unsafe {
                    std::ptr::copy_nonoverlapping(wide.as_ptr(), device, wide.len());
                }
                return flags | 4;
            }
        }
        if let Ok(device_id) = surfxr_audio::new_output(game.device.as_ref())
            .and_then(|device| surfxr_audio::get_windows_device_id(&device))
        {
            let wide: Vec<_> = device_id.encode_utf16().chain(Some(0)).collect();
            if !device.is_null() && wide.len() <= capacity {
                unsafe {
                    std::ptr::copy_nonoverlapping(wide.as_ptr(), device, wide.len());
                }
                flags |= 4;
            }
        }
    }
    flags
}
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonCapabilities(
    config: *const u8,
    config_size: usize,
    caps: *const u8,
    caps_size: usize,
    ip: *const u8,
    out: *mut u8,
    capacity: usize,
) -> i32 {
    if config.is_null()
        || caps.is_null()
        || ip.is_null()
        || out.is_null()
        || config_size > 65536
        || caps_size > 65536
    {
        return -1;
    }
    let result = || -> Option<Vec<u8>> {
        let metadata: serde_json::Value =
            serde_json::from_slice(unsafe { std::slice::from_raw_parts(config, config_size) })
                .ok()?;
        let bytes = unsafe { std::slice::from_raw_parts(caps, caps_size) };
        let (mut packet, used): (surfxr_packets::ClientConnectionResult, usize) =
            bincode::serde::decode_from_slice(bytes, bincode::config::standard()).ok()?;
        if used != caps_size {
            return None;
        }
        let surfxr_packets::ClientConnectionResult::ConnectionAccepted(info) = &mut packet else {
            return None;
        };
        let caps = info.streaming_capabilities.as_mut()?;
        let mut extension: serde_json::Value = serde_json::from_str(&caps.ext_str).ok()?;
        let name = metadata["frame_daemon_hostname"].as_str()?;
        if name.is_empty()
            || name.len() > 63
            || !name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
        {
            return None;
        }
        extension["frame_client_hostname"] = serde_json::json!(name);
        extension["frame_daemon"] = serde_json::json!(1);
        extension["frame_daemon_media_overhead"] = metadata["frame_daemon_media_overhead"].clone();
        let ip = unsafe { std::slice::from_raw_parts(ip, 4) };
        extension["frame_daemon_peer_ip"] =
            serde_json::json!(std::net::Ipv4Addr::new(ip[0], ip[1], ip[2], ip[3]).to_string());
        for field in ["frame_daemon_playback_id", "frame_daemon_recording_id"] {
            if let Some(id) = metadata[field]
                .as_str()
                .filter(|id| !id.is_empty() && id.len() < 4096)
            {
                extension[field] = serde_json::json!(id);
            }
        }
        caps.ext_str = extension.to_string();
        bincode::serde::encode_to_vec(packet, bincode::config::standard()).ok()
    };
    match result() {
        Some(bytes) if bytes.len() <= capacity => {
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
            }
            bytes.len() as i32
        }
        _ => -1,
    }
}
#[unsafe(no_mangle)]
pub extern "C" fn FrameDaemonMicrophone(
    bytes: *const u8,
    size: usize,
    callbacks: *const AudioCallbacks,
) -> i32 {
    let Some(config) = config(bytes, size) else {
        return 2;
    };
    let Some(callbacks) = (unsafe { callbacks.as_ref() }) else {
        return 2;
    };
    let Switch::Enabled(microphone) = config.microphone else {
        return 0;
    };
    let running = || !(callbacks.stopped)(callbacks.owner);
    while running() {
        let attempt = || -> surfxr_common::anyhow::Result<()> {
            let (sink, source) =
                surfxr_audio::new_virtual_microphone_pair(microphone.devices.clone())?;
            surfxr_audio::cap_steam_microphone_boost_windows(&source)?;
            surfxr_audio::play_audio_from(
                &running,
                &sink,
                1,
                48000,
                microphone.buffering.clone(),
                || {
                    let mut bytes = vec![0; 192000];
                    let mut lost = false;
                    let size = (callbacks.receive)(
                        callbacks.owner,
                        bytes.as_mut_ptr(),
                        bytes.len(),
                        &mut lost,
                    );
                    if size < 0 {
                        return Err(surfxr_common::anyhow::anyhow!(
                            "daemon_microphone_receive_failed"
                        ));
                    }
                    if size == 0 {
                        return Ok(None);
                    }
                    if size as usize > bytes.len() || size % 2 != 0 {
                        return Err(surfxr_common::anyhow::anyhow!(
                            "daemon_microphone_packet_bound"
                        ));
                    }
                    bytes.truncate(size as usize);
                    Ok(Some((bytes, lost)))
                },
            )
        };
        if attempt().is_ok() {
            break;
        }
        let end = Instant::now() + Duration::from_secs(1);
        while running() && Instant::now() < end {
            thread::sleep(Duration::from_millis(20));
        }
    }
    0
}
