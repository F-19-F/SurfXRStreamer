use super::*;

pub(super) fn make_settings(negotiated: Option<&ServerNegotiatedStreamingConfig>) -> Settings {
    let settings = surfxr_server_core::runtime_settings();
    let video = &settings.video;
    let nvenc = &video.encoder_config.nvenc;
    let amf = &video.encoder_config.amf;
    let hdr = &video.encoder_config.hdr;

    let (controllers_enabled, controller_is_tracker, use_separate_hand_trackers) =
        if let Switch::Enabled(config) = &settings.headset.controllers {
            (
                true,
                matches!(config.emulation_mode, ControllersEmulationMode::ViveTracker),
                config
                    .hand_skeleton
                    .as_option()
                    .is_some_and(|c| c.steamvr_input_2_0),
            )
        } else {
            (false, false, false)
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

    let foveated_encoding = negotiated.and_then(|n| n.foveated_encoding);
    let foveation_params = foveated_encoding.unwrap_or_default();

    let (enable_color_correction, brightness, contrast, saturation, gamma, sharpening) =
        if let Switch::Enabled(config) = &video.color_correction {
            (
                true,
                config.brightness,
                config.contrast,
                config.saturation,
                config.gamma,
                config.sharpening,
            )
        } else {
            (false, 0.0, 0.0, 0.0, 0.0, 0.0)
        };

    // Resolution and refresh rate come from negotiation; fall back to persisted steamvr_hmd_init_config.
    let (render_width, render_height, target_width, target_height, refresh_rate) =
        if let Some(n) = negotiated {
            (
                n.transcoding_view_resolution.x * 2,
                n.transcoding_view_resolution.y,
                n.emulated_headset_view_resolution.x * 2,
                n.emulated_headset_view_resolution.y,
                n.refresh_rate as i32,
            )
        } else {
            let openvr = surfxr_server_core::steamvr_hmd_init_config();
            (
                openvr.eye_resolution_width * 2,
                openvr.eye_resolution_height,
                openvr.target_eye_resolution_width * 2,
                openvr.target_eye_resolution_height,
                openvr.refresh_rate as i32,
            )
        };

    // Encoder params determined by negotiation; default to zero before any client connects.
    let (codec, h264_profile, use_10bit_encoder, encoding_gamma, enable_hdr) =
        if let Some(n) = negotiated {
            (
                n.codec as i32,
                n.h264_profile as i32,
                n.use_10bit_encoder,
                n.encoding_gamma as f64,
                n.enable_hdr,
            )
        } else {
            (0, 0, false, 0.0, false)
        };

    // The vsync grid runs at the rate the driver is told about, set here so the announcer never
    // has to read a setting back across the boundary.
    vsync::set_refresh_rate(refresh_rate as f32);

    Settings {
        m_refreshRate: refresh_rate,
        m_renderWidth: render_width,
        m_renderHeight: render_height,
        m_recommendedTargetWidth: target_width as i32,
        m_recommendedTargetHeight: target_height as i32,
        m_nAdapterIndex: video.adapter_index as i32,
        m_enableFoveatedEncoding: foveated_encoding.is_some(),
        m_frameGaze: negotiated.and_then(|config| config.frame_gaze).map_or_else(
            FfiFrameGazeConfig::default,
            |config| FfiFrameGazeConfig {
                enabled: true,
                baseWidth: config.base_width,
                baseHeight: config.base_height,
                focusWidth: config.focus_width,
                focusHeight: config.focus_height,
            },
        ),
        m_foveatedEncoding: FfiFoveatedEncodingParams {
            encodedViewResolution: foveation_params.encoded_view_resolution,
            viewRatio: foveation_params.view_ratio,
            centerSize: foveation_params.center_size,
            centerShifts: foveation_params.center_shifts,
            edgeRatio: foveation_params.edge_ratio,
        },
        m_enableColorCorrection: enable_color_correction,
        m_brightness: brightness,
        m_contrast: contrast,
        m_saturation: saturation,
        m_gamma: gamma,
        m_sharpening: sharpening,
        m_codec: codec,
        m_h264Profile: h264_profile,
        m_use10bitEncoder: use_10bit_encoder,
        m_encodingGamma: encoding_gamma,
        m_enableHdr: enable_hdr,
        m_forceHdrSrgbCorrection: hdr.force_hdr_srgb_correction,
        m_clampHdrExtendedRange: hdr.clamp_hdr_extended_range,
        m_enableAmfPreAnalysis: amf.enable_pre_analysis,
        m_enableVbaq: video.encoder_config.enable_vbaq,
        m_enableAmfHmqb: amf.enable_hmqb,
        m_useAmfPreproc: amf.use_preproc,
        m_amfPreProcSigma: amf.preproc_sigma,
        m_amfPreProcTor: amf.preproc_tor,
        m_encoderQualityPreset: video.encoder_config.quality_preset as u32,
        m_amdBitrateCorruptionFix: video.bitrate.image_corruption_fix,
        m_nvencQualityPreset: nvenc.quality_preset as u32,
        m_rateControlMode: video.encoder_config.rate_control_mode as u32,
        m_fillerData: video.encoder_config.filler_data,
        m_entropyCoding: video.encoder_config.entropy_coding as u32,
        m_forceSwEncoding: video.encoder_config.software.force_software_encoding,
        m_swThreadCount: video.encoder_config.software.thread_count,
        m_nvencTuningPreset: nvenc.tuning_preset as u32,
        m_nvencMultiPass: nvenc.multi_pass as u32,
        m_nvencSplitEncodeMode: nvenc.split_encode_mode as u32,
        m_nvencAdaptiveQuantizationMode: nvenc.adaptive_quantization_mode as u32,
        m_nvencLowDelayKeyFrameScale: nvenc.low_delay_key_frame_scale,
        m_nvencRefreshRate: nvenc.refresh_rate,
        m_nvencEnableIntraRefresh: nvenc.enable_intra_refresh,
        m_nvencIntraRefreshPeriod: nvenc.intra_refresh_period,
        m_nvencIntraRefreshCount: nvenc.intra_refresh_count,
        m_nvencMaxNumRefFrames: nvenc.max_num_ref_frames,
        m_nvencGopLength: nvenc.gop_length,
        m_nvencPFrameStrategy: nvenc.p_frame_strategy,
        m_nvencRateControlMode: nvenc.rate_control_mode,
        m_nvencRcBufferSize: nvenc.rc_buffer_size,
        m_nvencRcInitialDelay: nvenc.rc_initial_delay,
        m_nvencRcMaxBitrate: nvenc.rc_max_bitrate,
        m_nvencRcAverageBitrate: nvenc.rc_average_bitrate,
        m_nvencEnableWeightedPrediction: nvenc.enable_weighted_prediction,
        m_minimumIdrIntervalMs: settings.connection.minimum_idr_interval_ms,
        m_enableViveTrackerProxy: settings.headset.enable_vive_tracker_proxy,
        m_trackingRefOnly: settings.headset.tracking_ref_only,
        m_enableControllers: controllers_enabled,
        m_controllerIsTracker: controller_is_tracker,
        m_enableBodyTrackingFakeVive: body_tracking_vive_enabled,
        m_bodyTrackingHasLegs: body_tracking_has_legs,
        m_useSeparateHandTrackers: use_separate_hand_trackers,
    }
}
