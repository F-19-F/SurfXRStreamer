use super::*;

pub(super) fn make() -> ExtraConfigDefault {
ExtraConfigDefault {
            frame_desktop: FrameDesktopConfigDefault {
                enabled: true, allow_input: true, allow_audio: true,
                keep_steamvr_tracking: true,
                media_encryption: false,
                require_system_service: false, framerate: 60, bitrate_mbps: 60, monitor_count: 1,
            },
            logging: LoggingConfigDefault {
                log_level: LogSeverityDefault { variant: LogSeverityDefaultVariant::Warning },
                client_log_report_level: SwitchDefault {
                    enabled: true,
                    content: LogSeverityDefault {
                        variant: LogSeverityDefaultVariant::Error,
                    },
                },
                log_to_disk: cfg!(debug_assertions),
                log_button_presses: false,
                log_tracking: false,
                log_haptics: false,
                notification_level: LogSeverityDefault {
                    variant: if cfg!(debug_assertions) {
                        LogSeverityDefaultVariant::Info
                    } else {
                        LogSeverityDefaultVariant::Warning
                    },
                },
                show_raw_events: SwitchDefault {
                    enabled: false,
                    content: RawEventsConfigDefault {
                        hide_spammy_events: false,
                    },
                },
                prefer_backtrace: false,
                show_notification_tip: true,
                debug_groups: DebugGroupsConfigDefault {
                    server_impl: false,
                    client_impl: false,
                    server_core: false,
                    client_core: false,
                    connection: false,
                    sockets: false,
                    server_gfx: false,
                    client_gfx: false,
                    encoder: false,
                    decoder: false,
                },
            },
            steamvr_launcher: SteamvrLauncherDefault {
                enable_surfxr_on_manual_steamvr_start: false,
                open_close_steamvr_with_dashboard: false,
                direct_launch: false,
            },
            capture: CaptureConfigDefault {
                startup_video_recording: false,
                rolling_video_files: SwitchDefault {
                    enabled: false,
                    content: RollingVideoFilesConfigDefault { duration_s: 5 },
                },
            },
            velocities_multiplier: 1.0,
            open_setup_wizard: surfxr_common::is_stable() || surfxr_common::is_nightly(),
            new_version_popup: SwitchDefault {
                enabled: false,
                content: NewVersionPopupConfigDefault {
                    hide_while_version: SURFXR_VERSION.to_string(),
                },
            },
        }
}
