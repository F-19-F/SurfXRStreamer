use super::*;

pub(super) struct Negotiated {
    pub transcoding_view_resolution: UVec2,
    pub emulated_headset_view_resolution: UVec2,
    pub fps: f32,
    pub foveated_encoding: Option<SurfxrFoveatedEncodingParams>,
    pub encoder_profile: H264Profile,
    pub enable_10_bits_encoding: bool,
    pub enable_hdr: bool,
    pub encoding_gamma: f32,
    pub codec: CodecType,
    pub game_audio_sample_rate: u32,
    pub stream_protocol: SocketProtocol,
    pub priority_port: Option<u16>,
    pub video_fec: bool,
    pub video_haptics: bool,
    pub frame_gaze: Option<surfxr_sockets::FrameGazeConfig>,
    pub image_timestamps: bool,
    pub frame_lifecycle: bool,
    pub frame_boundary_sync: bool,
    pub frame_pose_time: bool,
    pub frame_clock_calibration: bool,
    pub frame_controller_state: bool,
    pub stream_config_packet: StreamConfigPacket,
}

pub(super) fn prepare(initial_settings: &Settings, streaming_caps: &surfxr_packets::VideoStreamingCapabilities,
    session: &surfxr_session::SessionConfig, frame_daemon: bool, client_ip: IpAddr) -> ConResult<Negotiated> {
    fn get_view_res(config: FrameSize, default_res: UVec2) -> UVec2 {
        let res = match config {
            FrameSize::Scale(scale) => default_res.as_vec2() * scale,
            FrameSize::Absolute { width, height } => {
                let width = width as f32;
                Vec2::new(
                    width,
                    height.map_or_else(
                        || {
                            let default_res = default_res.as_vec2();
                            width * default_res.y / default_res.x
                        },
                        |h| h as f32,
                    ),
                )
            }
        };

        UVec2::new(align32(res.x), align32(res.y))
    }

    let mut transcoding_view_resolution = get_view_res(
        initial_settings.video.transcoding_view_resolution.clone(),
        streaming_caps.default_view_resolution,
    );
    if transcoding_view_resolution.x > streaming_caps.max_view_resolution.x
        || transcoding_view_resolution.y > streaming_caps.max_view_resolution.y
    {
        warn!(
            "Chosen resolution {}x{} exceeds client maximum supported resolution of {}x{}. \
            Using maximum supported resolution at same aspect ratio.",
            transcoding_view_resolution.x,
            transcoding_view_resolution.y,
            streaming_caps.max_view_resolution.x,
            streaming_caps.max_view_resolution.y,
        );

        let transcoding_ratio =
            transcoding_view_resolution.x as f32 / transcoding_view_resolution.y as f32;

        if transcoding_ratio
            > streaming_caps.max_view_resolution.x as f32
                / streaming_caps.max_view_resolution.y as f32
        {
            transcoding_view_resolution = UVec2::new(
                align32(streaming_caps.max_view_resolution.x as f32),
                align32(streaming_caps.max_view_resolution.x as f32 / transcoding_ratio),
            );
        } else {
            transcoding_view_resolution = UVec2::new(
                align32(streaming_caps.max_view_resolution.y as f32 * transcoding_ratio),
                align32(streaming_caps.max_view_resolution.y as f32),
            );
        }
    }

    let emulated_headset_view_resolution = get_view_res(
        initial_settings
            .video
            .emulated_headset_view_resolution
            .clone(),
        streaming_caps.default_view_resolution,
    );

    let fps = {
        let mut best_match = 0_f32;
        let mut min_diff = f32::MAX;
        for rate in &streaming_caps.refresh_rates {
            let diff = (*rate - initial_settings.video.preferred_fps).abs();
            if diff < min_diff {
                best_match = *rate;
                min_diff = diff;
            }
        }
        best_match
    };

    if !streaming_caps
        .refresh_rates
        .contains(&initial_settings.video.preferred_fps)
    {
        warn!("Chosen refresh rate not supported. Using {fps}Hz");
    }

    let gaze_requested = surfxr_sockets::frame_gaze_requested(
        &streaming_caps.ext_str, initial_settings.video.frame_gaze.enabled);
    let foveated_encoding = if gaze_requested {
        None
    } else if let Switch::Enabled(config) =
        &initial_settings.video.foveated_encoding
    {
        if streaming_caps.foveated_encoding || config.force_enable {
            let mut params = SurfxrFoveatedEncodingParams {
                edge_ratio: config.edge_ratio,
                ..Default::default()
            };

            for (axis, resolution) in transcoding_view_resolution
                .to_array()
                .into_iter()
                .enumerate()
            {
                let resolution = resolution as f32;
                // # Safety: `axis` comes from a two-element resolution array, and each
                // configuration array also contains exactly two axes.
                let (center_size, center_shift, edge_ratio) = (
                    config.center_size[axis],
                    config.center_shift[axis],
                    config.edge_ratio[axis],
                );
                let edge_size = resolution - center_size * resolution;

                // Preserve the existing C++ encoder's operation order and double intermediates.
                let center_size = (1.0
                    - (edge_size as f64 / (edge_ratio as f64 * 2.0)).ceil()
                        * (edge_ratio as f64 * 2.0)
                        / resolution as f64) as f32;
                let edge_size = resolution - center_size * resolution;
                let center_shift =
                    align_foveation_center_shift(center_shift, edge_size, edge_ratio);

                let scale =
                    (center_size as f64 + (1.0 - center_size as f64) / edge_ratio as f64) as f32;
                let optimized_resolution = scale * resolution;
                let encoded_resolution = (optimized_resolution / 32.0).ceil() as u32 * 32;

                // # Safety: `axis` is 0 or 1; all output axis arrays and the eye array have length 2.
                (
                    params.encoded_view_resolution[axis],
                    params.view_ratio[axis],
                    params.center_size[axis],
                    params.center_shifts[0][axis],
                    params.center_shifts[1][axis],
                ) = (
                    encoded_resolution,
                    optimized_resolution / encoded_resolution as f32,
                    center_size,
                    center_shift,
                    center_shift,
                );
            }

            Some(params)
        } else {
            warn!("Foveated encoding is not supported by the client.");

            None
        }
    } else {
        None
    };

    let encoder_profile = if initial_settings.video.encoder_config.h264_profile == H264Profile::High
    {
        let profile = if streaming_caps.encoder_high_profile {
            H264Profile::High
        } else {
            H264Profile::Main
        };

        if profile != H264Profile::High {
            warn!("High profile encoding is not supported by the client.");
        }

        profile
    } else {
        initial_settings.video.encoder_config.h264_profile
    };

    let mut enable_10_bits_encoding = initial_settings
        .video
        .encoder_config
        .use_10bit
        .unwrap_or(streaming_caps.prefer_10bit);

    if enable_10_bits_encoding && !streaming_caps.encoder_10_bits {
        warn!("10 bits encoding is not supported by the client.");
        enable_10_bits_encoding = false
    }

    let enable_hdr = initial_settings
        .video
        .encoder_config
        .hdr
        .enable
        .unwrap_or(streaming_caps.prefer_hdr);

    let encoding_gamma = initial_settings
        .video
        .encoder_config
        .encoding_gamma
        .unwrap_or(streaming_caps.preferred_encoding_gamma);

    let codec = if initial_settings.video.preferred_codec == CodecType::AV1 {
        let codec = if streaming_caps.encoder_av1 {
            CodecType::AV1
        } else {
            CodecType::Hevc
        };

        if codec != CodecType::AV1 {
            warn!("AV1 encoding is not supported by the client.");
        }

        codec
    } else {
        initial_settings.video.preferred_codec
    };

    #[cfg(not(target_os = "windows"))]
    let game_audio_sample_rate = 44100;

    #[cfg(target_os = "windows")]
    let game_audio_sample_rate =
        if frame_daemon {48000} else if let Switch::Enabled(game_audio_config) = &initial_settings.audio.game_audio {
            let game_audio_device =
                surfxr_audio::new_output(game_audio_config.device.as_ref()).to_con()?;

            if let Switch::Enabled(microphone_config) = &initial_settings.audio.microphone
                && matches!(
                    microphone_config.devices,
                    surfxr_session::MicrophoneDevicesConfig::VAC
                        | surfxr_session::MicrophoneDevicesConfig::VBCable
                )
            {
                let (sink, _) =
                    surfxr_audio::new_virtual_microphone_pair(microphone_config.devices.clone())
                        .to_con()?;

                // VoiceMeeter and Custom devices may have arbitrary internal routing.
                // Therefore, we cannot detect the loopback issue without knowing the routing.
                if surfxr_audio::is_same_device(&game_audio_device, &sink) {
                    con_bail!("Game audio and microphone cannot point to the same device!");
                }
            }

            surfxr_audio::input_sample_rate(&game_audio_device).to_con()?
        } else {
            0
        };

    // Only the local daemon may own media/audio on behalf of the authenticated
    // Frame. Ordinary wired clients retain the upstream TCP behavior.
    let wired = client_ip.is_loopback()&&!frame_daemon;
    let stream_protocol = if wired {
        SocketProtocol::Tcp
    } else {
        initial_settings.connection.stream_protocol
    };
    let (frame_stream_ext, priority_port) = surfxr_sockets::negotiate_frame_streams(
        &streaming_caps.ext_str,
        stream_protocol,
        initial_settings.connection.stream_port,
    )
    .to_con()?;

    let (frame_stream_ext, video_fec) =
        surfxr_sockets::negotiate_frame_video_fec(&streaming_caps.ext_str, &frame_stream_ext).to_con()?;
    let (frame_stream_ext, video_haptics) =
        surfxr_sockets::negotiate_frame_video_haptics(&streaming_caps.ext_str, &frame_stream_ext)
            .to_con()?;

    let (frame_stream_ext, frame_gaze) = surfxr_sockets::negotiate_frame_gaze(
        &streaming_caps.ext_str, &frame_stream_ext, transcoding_view_resolution.to_array(),
        [initial_settings.video.frame_gaze.focus_width_percent,
            initial_settings.video.frame_gaze.focus_height_percent],
        initial_settings.video.frame_gaze.enabled && cfg!(target_os = "windows") && matches!(stream_protocol, SocketProtocol::Udp) &&
            foveated_encoding.is_none() && !enable_hdr && !enable_10_bits_encoding &&
            codec != CodecType::AV1 && (encoding_gamma - 1.0).abs() < 0.0001,
    ).to_con()?;
    let (frame_stream_ext, image_timestamps) = surfxr_sockets::negotiate_frame_video_timestamps(
        &streaming_caps.ext_str, &frame_stream_ext, frame_gaze.is_some(),
    ).to_con()?;
    let (frame_stream_ext, frame_lifecycle) = crate::frame_lifecycle::negotiate(
        &streaming_caps.ext_str, &frame_stream_ext);
    dbg_connection!("connection_pipeline: send streaming config");
    let (frame_stream_ext, frame_boundary_sync) = crate::frame_boundary::negotiate(
        &streaming_caps.ext_str, &frame_stream_ext);
    let (frame_stream_ext, frame_pose_time) = crate::frame_pose::negotiate(
        &streaming_caps.ext_str, &frame_stream_ext);
    let (frame_stream_ext, frame_clock_calibration) = crate::frame_clock::negotiate(
        &streaming_caps.ext_str, &frame_stream_ext);
    let frame_controller_state = crate::frame_controller_state::supported(
        &streaming_caps.ext_str,
        initial_settings.headset.controllers.as_option().map(|config| &config.emulation_mode));
    let stream_config_packet = StreamConfigPacket::new(
        session,
        ClientNegotiatedStreamingConfig {
            view_resolution: transcoding_view_resolution,
            refresh_rate_hint: fps,
            game_audio_sample_rate,
            foveated_encoding,
            encoding_gamma,
            enable_hdr,
            wired,
            ext_str: crate::frame_inputs::negotiate(&streaming_caps.ext_str, &frame_stream_ext,
                initial_settings.headset.controllers.as_option().map(|config| &config.emulation_mode)),
        },
    )
    .to_con()?;

    Ok(Negotiated { transcoding_view_resolution, emulated_headset_view_resolution,
        fps, foveated_encoding, encoder_profile, enable_10_bits_encoding, enable_hdr, encoding_gamma, codec,
        game_audio_sample_rate, stream_protocol, priority_port, video_fec, video_haptics, frame_gaze,
        image_timestamps, frame_lifecycle, frame_boundary_sync, frame_pose_time, frame_clock_calibration,
        frame_controller_state, stream_config_packet })
}
