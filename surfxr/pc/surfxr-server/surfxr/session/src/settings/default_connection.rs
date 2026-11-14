use super::*;

pub(super) fn make() -> ConnectionConfigDefault {
    let socket_buffer = SocketBufferSizeDefault {
        Custom: 100000,
        variant: SocketBufferSizeDefaultVariant::Maximum,
    };
    let socket_buffer_config = SocketBufferConfigDefault {
        send_size_bytes: socket_buffer.clone(),
        recv_size_bytes: socket_buffer,
    };

ConnectionConfigDefault {
            stream_protocol: SocketProtocolDefault {
                variant: SocketProtocolDefaultVariant::Udp,
            },
            client_discovery: SwitchDefault {
                enabled: true,
                content: DiscoveryConfigDefault {
                    auto_trust_clients: cfg!(debug_assertions),
                },
            },
            wired_client_type: ClientFlavorDefault {
                Custom: "surfxr.client".to_owned(),
                variant: if surfxr_common::is_stable() {
                    ClientFlavorDefaultVariant::Store
                } else {
                    ClientFlavorDefaultVariant::Github
                },
            },
            wired_client_autolaunch: SwitchDefault {
                enabled: true,
                content: WiredClientAutoLaunchConfigDefault { boot_delay: 0 },
            },
            web_server_port: surfxr_common::ports::WEB,
            stream_port: surfxr_common::ports::VR_MEDIA,
            osc_local_port: surfxr_common::ports::OSC_LOCAL,
            dscp: OptionalDefault {
                set: false,
                content: DscpTosDefault {
                    ClassSelector: 7,
                    AssuredForwarding: DscpTosAssuredForwardingDefault {
                        class: 4,
                        drop_probability: DropProbabilityDefault {
                            variant: DropProbabilityDefaultVariant::Low,
                        },
                    },
                    variant: DscpTosDefaultVariant::ExpeditedForwarding,
                },
            },
            server_buffer_config: socket_buffer_config.clone(),
            client_buffer_config: socket_buffer_config,
            max_queued_server_video_frames: 1024,
            avoid_video_glitching: false,
            minimum_idr_interval_ms: 100,
            enable_on_connect_script: false,
            enable_on_disconnect_script: false,
            allow_untrusted_http: false,
            allow_remote_dashboard: false,
            packet_size: 1400,
            statistics_history_size: 256,
        }
}
