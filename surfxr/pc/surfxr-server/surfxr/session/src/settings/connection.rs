use super::*;

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
#[schema(gui = "button_group")]
pub enum SocketProtocol {
    #[schema(strings(display_name = "UDP"))]
    Udp,
    #[schema(strings(display_name = "TCP"))]
    Tcp,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct DiscoveryConfig {
    #[cfg_attr(target_os = "linux", schema(flag = "hidden"))]
    #[schema(strings(
        help = "Allow untrusted clients to connect without confirmation. This is not recommended for security reasons."
    ))]
    pub auto_trust_clients: bool,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, Default)]
pub enum SocketBufferSize {
    #[default]
    Default,
    Maximum,
    Custom(#[schema(suffix = "B")] u32),
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy, Default)]
pub struct SocketBufferConfig {
    #[schema(strings(display_name = "Send size"))]
    pub send_size_bytes: SocketBufferSize,
    #[schema(strings(display_name = "Receive size"))]
    pub recv_size_bytes: SocketBufferSize,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct WiredClientAutoLaunchConfig {
    #[schema(strings(
        help = "Delay in seconds to wait after booting the headset before trying to launch the client."
    ))]
    pub boot_delay: u32,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone)]
pub struct ConnectionConfig {
    #[schema(strings(
        help = r#"UDP: Faster, but less stable than TCP. Try this if your network is well optimized and free of interference.
TCP: Slower than UDP, but more stable. Pick this if you experience video or audio stutters with UDP."#
    ))]
    pub stream_protocol: SocketProtocol,

    pub client_discovery: Switch<DiscoveryConfig>,

    #[schema(strings(
        help = r#"Which release type of client should SURFXR look for when establishing a wired connection."#
    ))]
    pub wired_client_type: ClientFlavor,

    #[schema(strings(
        help = r#"Wether SURFXR should try to automatically launch the client when establishing a wired connection."#
    ))]
    pub wired_client_autolaunch: Switch<WiredClientAutoLaunchConfig>,

    #[cfg_attr(
        windows,
        schema(strings(
            help = "If on_connect.bat exists alongside session.json, it will be run on headset connect. Env var ACTION will be set to `connect`."
        ))
    )]
    #[cfg_attr(
        not(windows),
        schema(strings(
            help = "If on_connect.sh exists alongside session.json, it will be run on headset connect. Env var ACTION will be set to `connect`."
        ))
    )]
    pub enable_on_connect_script: bool,

    #[cfg_attr(
        windows,
        schema(strings(
            help = "If on_disconnect.bat exists alongside session.json, it will be run on headset disconnect. Env var ACTION will be set to `disconnect`."
        ))
    )]
    #[cfg_attr(
        not(windows),
        schema(strings(
            help = "If on_disconnect.sh exists alongside session.json, it will be run on headset disconnect. Env var ACTION will be set to `disconnect`."
        ))
    )]
    #[schema(flag = "real-time")]
    pub enable_on_disconnect_script: bool,

    #[schema(strings(
        display_name = "Allow untrusted HTTP",
        help = "Allow cross-origin browser requests to control SURFXR settings remotely."
    ))]
    pub allow_untrusted_http: bool,

    #[schema(flag = "steamvr-restart")]
    #[schema(strings(
        display_name = "Allow remote dashboard access",
        help = "Serve the dashboard on all network interfaces instead of localhost only. \
                Anyone who can reach this machine can then control SURFXR, as the web server \
                has no authentication."
    ))]
    pub allow_remote_dashboard: bool,

    #[schema(strings(
        help = r#"If the client, server or the network discarded one packet, discard packets until a IDR packet is found."#
    ))]
    pub avoid_video_glitching: bool,

    #[schema(gui(slider(min = 1024, max = 65507, logarithmic)), suffix = "B")]
    pub packet_size: i32,

    pub stream_port: u16,
    pub web_server_port: u16,

    #[schema(strings(display_name = "Local OSC port"))]
    pub osc_local_port: u16,

    pub server_buffer_config: SocketBufferConfig,
    pub client_buffer_config: SocketBufferConfig,

    #[schema(strings(
        help = r#"The server discards video packets if it can't push them to the network.
This could happen on TCP. A IDR frame is requested in this case."#
    ))]
    pub max_queued_server_video_frames: usize,

    #[schema(suffix = " frames")]
    pub statistics_history_size: usize,

    #[schema(strings(display_name = "Minimum IDR interval"))]
    #[schema(flag = "steamvr-restart")]
    #[schema(gui(slider(min = 5, max = 1000, step = 5)), suffix = "ms")]
    pub minimum_idr_interval_ms: u64,

    #[schema(strings(display_name = "DSCP (packet prio hints)"))]
    pub dscp: Option<DscpTos>,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
#[repr(u8)]
#[schema(gui = "button_group")]
pub enum DropProbability {
    Low = 0x01,
    Medium = 0x10,
    High = 0x11,
}

#[derive(SettingsSchema, Serialize, Deserialize, Clone, Copy)]
pub enum DscpTos {
    BestEffort,

    ClassSelector(#[schema(gui(slider(min = 1, max = 7)))] u8),

    AssuredForwarding {
        #[schema(gui(slider(min = 1, max = 4)))]
        class: u8,
        drop_probability: DropProbability,
    },

    ExpeditedForwarding,
}
