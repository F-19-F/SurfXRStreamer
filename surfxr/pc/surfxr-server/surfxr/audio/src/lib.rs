#[cfg(windows)]
mod capture_sender;
mod playback;
pub use playback::{get_next_frame_batch, play_audio_loop, play_audio_from, receive_samples_loop};
#[cfg(windows)]
mod windows;

#[cfg(target_os = "linux")]
pub mod linux;

#[cfg(windows)]
pub use crate::windows::*;

use surfxr_common::{
    ToAny,
    anyhow::{self, Context, Result, bail},
    info,
    parking_lot::Mutex,
};
use surfxr_session::{CustomAudioDeviceConfig, MicrophoneDevicesConfig};
use surfxr_sockets::StreamSender;
use cpal::{
    BufferSize, Host, Sample, SampleFormat, StreamConfig,
    traits::{DeviceTrait, HostTrait, StreamTrait},
};
use std::{sync::Arc, thread, time::Duration};

pub use cpal::Device;

fn device_from_custom_config(
    host: &Host,
    config: &CustomAudioDeviceConfig,
    is_output: bool,
) -> Result<Device> {
    let mut devices = if is_output {
        host.output_devices()?
    } else {
        host.input_devices()?
    };

    Ok(match config {
        CustomAudioDeviceConfig::NameSubstring(name_substring) => devices
            .find(|d| {
                d.name()
                    .map(|name| name.to_lowercase().contains(&name_substring.to_lowercase()))
                    .unwrap_or(false)
            })
            .with_context(|| {
                format!("Cannot find audio device which name contains \"{name_substring}\"")
            })?,
        CustomAudioDeviceConfig::Index(index) => devices
            .nth(*index)
            .with_context(|| format!("Cannot find audio device at index {index}"))?,
    })
}

pub fn new_output(config: Option<&CustomAudioDeviceConfig>) -> Result<Device> {
    let host = cpal::default_host();

    let device = match config {
        None => host
            .default_output_device()
            .context("No output audio device found")?,
        Some(config) => device_from_custom_config(&host, config, true)?,
    };

    Ok(device)
}

pub fn new_input(config: Option<CustomAudioDeviceConfig>) -> Result<Device> {
    let host = cpal::default_host();

    let device = match config {
        None => host
            .default_input_device()
            .context("No input audio device found")?,
        Some(config) => device_from_custom_config(&host, &config, false)?,
    };

    Ok(device)
}

// returns (sink, source)
pub fn new_virtual_microphone_pair(config: MicrophoneDevicesConfig) -> Result<(Device, Device)> {
    // No-op on Windows (this is windows specific code)
    let host = cpal::default_host();

    let (sink_name, source_name) = match config {
        MicrophoneDevicesConfig::Automatic => {
            // NOTE: This will iterate over all devices for every option it tries, if the audio
            // code is slow, change that first
            return [
                MicrophoneDevicesConfig::SteamStreaming,
                MicrophoneDevicesConfig::VAC,
                MicrophoneDevicesConfig::VBCable,
                MicrophoneDevicesConfig::VoiceMeeter,
                MicrophoneDevicesConfig::VoiceMeeterAux,
                MicrophoneDevicesConfig::VoiceMeeterVaio3,
            ]
            .into_iter()
            .find_map(|cable_type| new_virtual_microphone_pair(cable_type).ok())
            .context("No microphones found");
        }

        MicrophoneDevicesConfig::SteamStreaming => {
            ("Steam Streaming Microphone", "Steam Streaming Microphone")
        }
        MicrophoneDevicesConfig::VAC => ("Line 1", "Line 1"),
        MicrophoneDevicesConfig::VBCable => ("CABLE Input", "CABLE Output"),
        MicrophoneDevicesConfig::VoiceMeeter => ("VoiceMeeter Input", "VoiceMeeter Output"),
        MicrophoneDevicesConfig::VoiceMeeterAux => {
            ("VoiceMeeter Aux Input", "VoiceMeeter Aux Output")
        }
        MicrophoneDevicesConfig::VoiceMeeterVaio3 => {
            ("VoiceMeeter VAIO3 Input", "VoiceMeeter VAIO3 Output")
        }

        MicrophoneDevicesConfig::Custom { sink, source } => {
            return Ok((
                device_from_custom_config(&host, &sink, true)?,
                device_from_custom_config(&host, &source, false)?,
            ));
        }
    };

    let sink = host
            .output_devices()?
            .find(|d| d.name().unwrap_or_default().contains(sink_name))
            .with_context(|| format!("Virtual microphone playback endpoint '{sink_name}' not found. Check Windows audio devices or select an installed device in Advanced > Audio."))?;

    let source = host
        .input_devices()?
        .find(|d| d.name().unwrap_or_default().contains(source_name))
        .with_context(|| format!("Virtual microphone recording endpoint '{source_name}' not found. Check Windows audio devices or select an installed device in Advanced > Audio."))?;

    Ok((sink, source))
}

pub fn input_sample_rate(dev: &Device) -> Result<u32> {
    let config = dev
        .default_input_config()
        // On Windows, loopback devices are not recognized as input devices. Use output config.
        .or_else(|_| dev.default_output_config())?;

    Ok(config.sample_rate().0)
}

pub enum AudioRecordState {
    Recording,
    ShouldStop,
    Err(Option<anyhow::Error>),
}

pub enum AudioChannel {
    FrontLeft,
    FrontRight,
    Center,
    SurroundLeft,
    SurroundRight,
    BackLeft,
    BackRight,
    Top,
    HighFrontLeft,
    HighFrontRight,
    HighFrontCenter,
    HighBackLeft,
    HighBackRight,
    LowFrequency,
}

fn downmix_channels(channels: &[AudioChannel], data: &[u8], out_channels: u16) -> Vec<u8> {
    let mut left = 0.0;
    let mut right = 0.0;

    for i in 0..channels.len() {
        let [l, r] = match &channels[i] {
            AudioChannel::FrontLeft => [1.0, 0.0],
            AudioChannel::FrontRight => [0.0, 1.0],
            AudioChannel::Center => [0.707, 0.707],
            AudioChannel::SurroundLeft => [0.707, 0.0],
            AudioChannel::SurroundRight => [0.0, 0.707],
            AudioChannel::BackLeft => [0.707, 0.0],
            AudioChannel::BackRight => [0.0, 0.707],
            AudioChannel::Top => [0.577, 0.577],
            AudioChannel::HighFrontLeft => [0.707, 0.0],
            AudioChannel::HighFrontRight => [0.0, 0.707],
            AudioChannel::HighFrontCenter => [0.5, 0.5],
            AudioChannel::HighBackLeft => [0.5, 0.0],
            AudioChannel::HighBackRight => [0.0, 0.5],
            _ => [0.0, 0.0],
        };
        let val = i16::from_ne_bytes([data[i * 2], data[i * 2 + 1]]).to_sample::<f32>();
        left += val * l;
        right += val * r;
    }

    if out_channels == 1 {
        let bytes = ((left + right) / 2.0).to_sample::<i16>().to_ne_bytes();
        vec![bytes[0], bytes[1]]
    } else {
        let left_bytes = left.to_sample::<i16>().to_ne_bytes();
        let right_bytes = right.to_sample::<i16>().to_ne_bytes();
        vec![left_bytes[0], left_bytes[1], right_bytes[0], right_bytes[1]]
    }
}

fn downmix_audio(data: Vec<u8>, in_channels: u16, out_channels: u16) -> Vec<u8> {
    if in_channels == out_channels {
        data
    } else if in_channels == 1 && out_channels == 2 {
        data.chunks_exact(2)
            .flat_map(|c| vec![c[0], c[1], c[0], c[1]])
            .collect()
    } else {
        let channels = match in_channels {
            2 => vec![AudioChannel::FrontLeft, AudioChannel::FrontRight],
            3 => vec![
                AudioChannel::FrontLeft,
                AudioChannel::FrontRight,
                AudioChannel::LowFrequency,
            ],
            4 => vec![
                AudioChannel::FrontLeft,
                AudioChannel::FrontRight,
                AudioChannel::BackLeft,
                AudioChannel::BackRight,
            ],
            6 => vec![
                AudioChannel::FrontLeft,
                AudioChannel::FrontRight,
                AudioChannel::Center,
                AudioChannel::LowFrequency,
                AudioChannel::SurroundLeft, // Sometimes actually BackLeft, has same level so it's okay
                AudioChannel::SurroundRight, // Sometimes actually BackRight, has same level so it's okay
            ],
            8 => vec![
                AudioChannel::FrontLeft,
                AudioChannel::FrontRight,
                AudioChannel::Center,
                AudioChannel::LowFrequency,
                AudioChannel::BackLeft,
                AudioChannel::BackRight,
                AudioChannel::SurroundLeft,
                AudioChannel::SurroundRight,
            ],
            _ => unreachable!("Invalid input channel count"),
        };

        data.chunks_exact(in_channels as usize * 2)
            .flat_map(|c| downmix_channels(&channels, c, out_channels))
            .collect()
    }
}

#[allow(unused_variables)]
#[allow(unused_mut)]
pub fn record_audio_blocking(
    is_running: Arc<dyn Fn() -> bool + Send + Sync>,
    mut sender: StreamSender<()>,
    device: &Device,
    channels_count: u16,
    mute: bool,
) -> Result<()> {
    let config = device
        .default_input_config()
        // On Windows, loopback devices are not recognized as input devices. Use output config.
        .or_else(|_| device.default_output_config())?;

    if config.channels() > 8 {
        bail!(
            "Audio devices with more than 8 channels are not supported. {}",
            "Please turn off surround audio."
        );
    } else if config.channels() == 5 || config.channels() == 7 {
        bail!(
            "Audio devices with {} channels are not supported.",
            config.channels()
        );
    }

    let stream_config = StreamConfig {
        channels: config.channels(),
        sample_rate: config.sample_rate(),
        buffer_size: BufferSize::Default,
    };

    let state = Arc::new(Mutex::new(AudioRecordState::Recording));

    #[cfg(windows)]
    let send_state = Arc::clone(&state);
    #[cfg(windows)]
    let mut capture_sender = capture_sender::CaptureSender::new(
        (config.sample_rate().0 as usize * 60 / 1000) * channels_count as usize * 2,
        Arc::clone(&is_running),
        sender.frame_udp_priority(),
        move |bytes| match sender.send_header_with_payload(&(), bytes) {
            Ok(()) => Ok(()),
            Err(error) => {
                let message = error.to_string();
                *send_state.lock() = AudioRecordState::Err(Some(error));
                Err(anyhow::anyhow!(message))
            }
        },
    );
    #[cfg(windows)]
    let producer = capture_sender.producer();

    let stream = device.build_input_stream_raw(
        &stream_config,
        config.sample_format(),
        {
            let state = Arc::clone(&state);
            let is_running = Arc::clone(&is_running);
            move |data, _| {
                let data = if config.sample_format() == SampleFormat::F32 {
                    data.bytes()
                        .chunks_exact(4)
                        .flat_map(|b| {
                            f32::from_ne_bytes([b[0], b[1], b[2], b[3]])
                                .to_sample::<i16>()
                                .to_ne_bytes()
                        })
                        .collect()
                } else {
                    data.bytes().to_vec()
                };

                let data = downmix_audio(data, config.channels(), channels_count);

                if is_running() {
                    #[cfg(windows)]
                    producer.submit(data);
                    #[cfg(not(windows))]
                    sender.send_header_with_payload(&(), &data).ok();
                } else {
                    *state.lock() = AudioRecordState::ShouldStop;
                }
            }
        },
        {
            let state = Arc::clone(&state);
            move |e| *state.lock() = AudioRecordState::Err(Some(e.into()))
        },
        None,
    )?;

    #[cfg(windows)]
    if mute && device.supports_output() {
        crate::windows::set_mute_windows_device(device, true).ok();
    }

    let mut res = stream.play().to_any();

    if res.is_ok() {
        while matches!(*state.lock(), AudioRecordState::Recording) && is_running() {
            thread::sleep(Duration::from_millis(500))
        }

        if let AudioRecordState::Err(e) = &mut *state.lock() {
            res = Err(e.take().unwrap());
        }
    }

    // Stop capture before joining the network owner: no callback can enqueue
    // PCM while teardown retires the sender and its shared socket reference.
    drop(stream);
    #[cfg(windows)]
    {
        let (dropped_bytes, send_failures) = capture_sender.stop();
        info!("Audio capture send: dropped_bytes={dropped_bytes}, send_failures={send_failures}");
    }

    #[cfg(windows)]
    if mute && device.supports_output() {
        set_mute_windows_device(device, false).ok();
    }

    res
}
