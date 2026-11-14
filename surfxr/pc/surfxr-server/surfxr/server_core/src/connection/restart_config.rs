use super::*;

// Compute a hash over all steamvr-restart settings and client-negotiated values.
// The small SteamvrHmdInitConfig carries the negotiated resolution/fps; everything else comes from
// Settings directly, using the same derivation as the old full SteamvrHmdInitConfig did.
pub fn compute_restart_settings_hash(
    steamvr_hmd_init_config: &SteamvrHmdInitConfig,
    settings: &Settings,
) -> u64 {
    let mut controller_is_tracker = false;
    let mut controller_profile: i32 = 0;
    let mut use_separate_hand_trackers = false;
    let controllers_enabled = if let Switch::Enabled(config) = &settings.headset.controllers {
        controller_is_tracker =
            matches!(config.emulation_mode, ControllersEmulationMode::ViveTracker);
        controller_profile = match config.emulation_mode {
            ControllersEmulationMode::SteamFrame => 70,
            ControllersEmulationMode::RiftSTouch => 0,
            ControllersEmulationMode::Quest1Touch => 1,
            ControllersEmulationMode::Quest2Touch => 2,
            ControllersEmulationMode::Quest3Plus => 3,
            ControllersEmulationMode::QuestPro => 4,
            ControllersEmulationMode::Pico4 => 10,
            ControllersEmulationMode::ValveIndex => 20,
            ControllersEmulationMode::ViveWand => 40,
            ControllersEmulationMode::ViveTracker => 41,
            ControllersEmulationMode::PSVR2Sense => 60,
            ControllersEmulationMode::Custom { .. } => 500,
        };
        use_separate_hand_trackers = config
            .hand_skeleton
            .as_option()
            .is_some_and(|c| c.steamvr_input_2_0);
        true
    } else {
        false
    };

    let body_tracking_vive_enabled =
        if let Switch::Enabled(config) = &settings.headset.body_tracking {
            matches!(config.sink, BodyTrackingSinkConfig::FakeViveTracker)
        } else if let Switch::Enabled(config) = &settings.headset.multimodal_tracking {
            config.detached_controllers_steamvr_sink
        } else {
            false
        };

    let body_tracking_has_legs = settings
        .headset
        .body_tracking
        .as_option()
        .map(|c| c.sources.meta.prefer_full_body)
        .unwrap_or(false);

    let mut foveation_eye_tracking = false;
    let mut foveation_center_size_x = 0.0_f32;
    let mut foveation_center_size_y = 0.0_f32;
    let mut foveation_center_shift_x = 0.0_f32;
    let mut foveation_center_shift_y = 0.0_f32;
    let mut foveation_edge_ratio_x = 0.0_f32;
    let mut foveation_edge_ratio_y = 0.0_f32;
    let enable_foveated_encoding =
        if let Switch::Enabled(config) = &settings.video.foveated_encoding {
            foveation_eye_tracking = settings
                .headset
                .face_tracking
                .as_option()
                .is_some_and(|config| config.sink.eye_tracked_foveated_encoding);
            [foveation_center_size_x, foveation_center_size_y] = config.center_size;
            [foveation_center_shift_x, foveation_center_shift_y] = config.center_shift;
            [foveation_edge_ratio_x, foveation_edge_ratio_y] = config.edge_ratio;
            true
        } else {
            false
        };

    let mut brightness = 0.0_f32;
    let mut contrast = 0.0_f32;
    let mut saturation = 0.0_f32;
    let mut gamma = 0.0_f32;
    let mut sharpening = 0.0_f32;
    let enable_color_correction = if let Switch::Enabled(config) = &settings.video.color_correction
    {
        brightness = config.brightness;
        contrast = config.contrast;
        saturation = config.saturation;
        gamma = config.gamma;
        sharpening = config.sharpening;
        true
    } else {
        false
    };

    let nvenc = &settings.video.encoder_config.nvenc;
    let amf = &settings.video.encoder_config.amf;
    let hdr = &settings.video.encoder_config.hdr;
    let enc = &settings.video.encoder_config;
    let dbg = &settings.extra.logging.debug_groups;

    let mut h = DefaultHasher::new();

    // Negotiated fields (persisted in SteamvrHmdInitConfig)
    steamvr_hmd_init_config.eye_resolution_width.hash(&mut h);
    steamvr_hmd_init_config.eye_resolution_height.hash(&mut h);
    steamvr_hmd_init_config
        .target_eye_resolution_width
        .hash(&mut h);
    steamvr_hmd_init_config
        .target_eye_resolution_height
        .hash(&mut h);
    steamvr_hmd_init_config.refresh_rate.hash(&mut h);
    // Pre-init settings fields (read directly from settings)
    settings.video.adapter_index.hash(&mut h);
    settings.headset.tracking_ref_only.hash(&mut h);
    settings.headset.enable_vive_tracker_proxy.hash(&mut h);
    // Encoder / codec
    (settings.video.preferred_codec as u8).hash(&mut h);
    (enc.h264_profile as u32).hash(&mut h);
    (enc.rate_control_mode as u32).hash(&mut h);
    enc.filler_data.hash(&mut h);
    (enc.entropy_coding as u32).hash(&mut h);
    (enc.quality_preset as u32).hash(&mut h);
    enc.enable_vbaq.hash(&mut h);
    enc.use_10bit.hash(&mut h);
    enc.encoding_gamma.map(f32::to_bits).hash(&mut h);
    enc.software.force_software_encoding.hash(&mut h);
    enc.software.thread_count.hash(&mut h);
    // HDR
    hdr.enable.hash(&mut h);
    hdr.force_hdr_srgb_correction.hash(&mut h);
    hdr.clamp_hdr_extended_range.hash(&mut h);
    // AMF
    amf.enable_pre_analysis.hash(&mut h);
    amf.enable_hmqb.hash(&mut h);
    amf.use_preproc.hash(&mut h);
    amf.preproc_sigma.hash(&mut h);
    amf.preproc_tor.hash(&mut h);
    // NVENC
    (nvenc.quality_preset as u32).hash(&mut h);
    (nvenc.tuning_preset as u32).hash(&mut h);
    (nvenc.multi_pass as u32).hash(&mut h);
    (nvenc.adaptive_quantization_mode as u32).hash(&mut h);
    nvenc.low_delay_key_frame_scale.hash(&mut h);
    nvenc.refresh_rate.hash(&mut h);
    nvenc.enable_intra_refresh.hash(&mut h);
    nvenc.intra_refresh_period.hash(&mut h);
    nvenc.intra_refresh_count.hash(&mut h);
    nvenc.max_num_ref_frames.hash(&mut h);
    nvenc.gop_length.hash(&mut h);
    nvenc.p_frame_strategy.hash(&mut h);
    nvenc.rate_control_mode.hash(&mut h);
    nvenc.rc_buffer_size.hash(&mut h);
    nvenc.rc_initial_delay.hash(&mut h);
    nvenc.rc_max_bitrate.hash(&mut h);
    nvenc.rc_average_bitrate.hash(&mut h);
    nvenc.enable_weighted_prediction.hash(&mut h);
    // Foveated encoding
    settings.video.frame_gaze.enabled.hash(&mut h);
    settings.video.frame_gaze.focus_width_percent.hash(&mut h);
    settings.video.frame_gaze.focus_height_percent.hash(&mut h);
    enable_foveated_encoding.hash(&mut h);
    foveation_eye_tracking.hash(&mut h);
    foveation_center_size_x.to_bits().hash(&mut h);
    foveation_center_size_y.to_bits().hash(&mut h);
    foveation_center_shift_x.to_bits().hash(&mut h);
    foveation_center_shift_y.to_bits().hash(&mut h);
    foveation_edge_ratio_x.to_bits().hash(&mut h);
    foveation_edge_ratio_y.to_bits().hash(&mut h);
    // Color correction
    enable_color_correction.hash(&mut h);
    brightness.to_bits().hash(&mut h);
    contrast.to_bits().hash(&mut h);
    saturation.to_bits().hash(&mut h);
    gamma.to_bits().hash(&mut h);
    sharpening.to_bits().hash(&mut h);
    // Controllers
    controllers_enabled.hash(&mut h);
    controller_is_tracker.hash(&mut h);
    controller_profile.hash(&mut h);
    use_separate_hand_trackers.hash(&mut h);
    // Body tracking
    body_tracking_vive_enabled.hash(&mut h);
    body_tracking_has_legs.hash(&mut h);
    // Misc
    settings.connection.minimum_idr_interval_ms.hash(&mut h);
    settings.video.bitrate.image_corruption_fix.hash(&mut h);
    // Debug groups
    dbg.server_impl.hash(&mut h);
    dbg.client_impl.hash(&mut h);
    dbg.server_core.hash(&mut h);
    dbg.client_core.hash(&mut h);
    dbg.connection.hash(&mut h);
    dbg.sockets.hash(&mut h);
    dbg.server_gfx.hash(&mut h);
    dbg.client_gfx.hash(&mut h);
    dbg.encoder.hash(&mut h);
    dbg.decoder.hash(&mut h);

    h.finish()
}
