use super::*;

pub(super) fn make() -> AudioConfigDefault {
    let default_custom_audio_device = CustomAudioDeviceConfigDefault {
        NameSubstring: "".into(),
        Index: 0,
        variant: CustomAudioDeviceConfigDefaultVariant::NameSubstring,
    };
AudioConfigDefault {
            game_audio: SwitchDefault {
                enabled: true,
                content: GameAudioConfigDefault {
                    gui_collapsed: true,
                    device: OptionalDefault {
                        set: false,
                        content: default_custom_audio_device.clone(),
                    },
                    mute_when_streaming: true,
                    buffering: AudioBufferingConfigDefault {
                        gui_collapsed: true,
                        average_buffering_ms: 50,
                        batch_ms: 10,
                    },
                },
            },
            microphone: SwitchDefault {
                enabled: cfg!(any(windows, target_os = "linux")),
                content: MicrophoneConfigDefault {
                    gui_collapsed: true,
                    devices: MicrophoneDevicesConfigDefault {
                        Custom: MicrophoneDevicesConfigCustomDefault {
                            source: default_custom_audio_device.clone(),
                            sink: default_custom_audio_device,
                        },
                        variant: if cfg!(windows) {
                            MicrophoneDevicesConfigDefaultVariant::SteamStreaming
                        } else {
                            MicrophoneDevicesConfigDefaultVariant::Automatic
                        },
                    },
                    buffering: AudioBufferingConfigDefault {
                        gui_collapsed: true,
                        average_buffering_ms: 50,
                        batch_ms: 10,
                    },
                },
            },
        }
}
