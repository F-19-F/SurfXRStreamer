use super::*;

pub(super) fn make() -> VideoConfigDefault {
    let view_resolution = FrameSizeDefault {
        variant: FrameSizeDefaultVariant::Absolute,
        Scale: 1.0,
        Absolute: FrameSizeAbsoluteDefault {
            width: 3072,
            height: OptionalDefault {
                set: true,
                content: 3264,
            },
        },
    };
VideoConfigDefault {
            passthrough: SwitchDefault {
                enabled: false,
                content: PassthroughModeDefault {
                    variant: PassthroughModeDefaultVariant::Blend,
                    Blend: PassthroughModeBlendDefault {
                        premultiplied_alpha: true,
                        threshold: 0.5,
                    },
                    RgbChromaKey: RgbChromaKeyConfigDefault {
                        red: 0,
                        green: 255,
                        blue: 0,
                        distance_threshold: 85,
                        feathering: 0.05,
                    },
                    HsvChromaKey: HsvChromaKeyConfigDefault {
                        hue_start_max_deg: 70.0,
                        hue_start_min_deg: 80.0,
                        hue_end_min_deg: 160.0,
                        hue_end_max_deg: 170.0,
                        saturation_start_max: 0.2,
                        saturation_start_min: 0.3,
                        saturation_end_min: 1.0,
                        saturation_end_max: 1.1,
                        value_start_max: 0.0,
                        value_start_min: 0.1,
                        value_end_min: 1.0,
                        value_end_max: 1.1,
                    },
                },
            },
            clientside_post_processing: SwitchDefault {
                enabled: false,
                content: ClientsidePostProcessingConfigDefault {
                    super_sampling: ClientsidePostProcessingSuperSamplingModeDefault {
                        variant: ClientsidePostProcessingSuperSamplingModeDefaultVariant::Quality,
                    },
                    sharpening: ClientsidePostProcessingSharpeningModeDefault {
                        variant: ClientsidePostProcessingSharpeningModeDefaultVariant::Quality,
                    },
                },
            },
            upscaling: SwitchDefault {
                enabled: false,
                content: UpscalingConfigDefault {
                    edge_direction: true,
                    edge_threshold: 4.0,
                    edge_sharpness: 2.0,
                    upscale_factor: 1.5,
                },
            },
            adapter_index: 0,
            transcoding_view_resolution: view_resolution.clone(),
            emulated_headset_view_resolution: view_resolution,
            preferred_fps: 120.,
            max_buffering_frames: 2.0,
            buffering_history_weight: 0.90,
            enforce_server_frame_pacing: true,
            bitrate: BitrateConfigDefault {
                gui_collapsed: false,
                mode: BitrateModeDefault {
                    ConstantMbps: 100,
                    Adaptive: BitrateModeAdaptiveDefault {
                        gui_collapsed: true,
                        saturation_multiplier: 0.95,
                        max_throughput_mbps: SwitchDefault {
                            enabled: false,
                            content: 100,
                        },
                        min_throughput_mbps: SwitchDefault {
                            enabled: false,
                            content: 5,
                        },
                        max_network_latency_ms: SwitchDefault {
                            enabled: false,
                            content: 8,
                        },
                        encoder_latency_limiter: SwitchDefault {
                            enabled: true,
                            content: EncoderLatencyLimiterDefault {
                                max_saturation_multiplier: 0.9,
                            },
                        },
                        decoder_latency_limiter: SwitchDefault {
                            enabled: true,
                            content: DecoderLatencyLimiterDefault {
                                gui_collapsed: false,
                                max_decoder_latency_ms: 30,
                                latency_overstep_frames: 90,
                                latency_overstep_multiplier: 0.99,
                            },
                        },
                    },
                    variant: BitrateModeDefaultVariant::ConstantMbps,
                },
                adapt_to_framerate: SwitchDefault {
                    enabled: false,
                    content: BitrateAdaptiveFramerateConfigDefault {
                        framerate_reset_threshold_multiplier: 2.0,
                    },
                },
                history_size: 256,
                image_corruption_fix: false,
            },
            preferred_codec: CodecTypeDefault {
                variant: CodecTypeDefaultVariant::Hevc,
            },
            encoder_config: EncoderConfigDefault {
                gui_collapsed: true,
                rate_control_mode: RateControlModeDefault {
                    variant: RateControlModeDefaultVariant::Cbr,
                },
                filler_data: false,
                h264_profile: H264ProfileDefault {
                    variant: H264ProfileDefaultVariant::High,
                },
                entropy_coding: EntropyCodingDefault {
                    variant: EntropyCodingDefaultVariant::Cavlc,
                },
                use_10bit: OptionalDefault {
                    set: false,
                    content: false,
                },
                encoding_gamma: OptionalDefault {
                    set: false,
                    content: 1.0,
                },
                hdr: HDRConfigDefault {
                    gui_collapsed: true,
                    enable: OptionalDefault {
                        set: false,
                        content: false,
                    },
                    force_hdr_srgb_correction: false,
                    clamp_hdr_extended_range: false,
                },
                nvenc: NvencConfigDefault {
                    gui_collapsed: true,
                    quality_preset: EncoderQualityPresetNvidiaDefault {
                        variant: EncoderQualityPresetNvidiaDefaultVariant::P1,
                    },
                    tuning_preset: NvencTuningPresetDefault {
                        variant: NvencTuningPresetDefaultVariant::LowLatency,
                    },
                    multi_pass: NvencMultiPassDefault {
                        variant: NvencMultiPassDefaultVariant::Disabled,
                    },
                    split_encode_mode: NvencSplitEncodeModeDefault {
                        variant: NvencSplitEncodeModeDefaultVariant::ForceThree,
                    },
                    adaptive_quantization_mode: NvencAdaptiveQuantizationModeDefault {
                        variant: NvencAdaptiveQuantizationModeDefaultVariant::Spatial,
                    },
                    low_delay_key_frame_scale: -1,
                    refresh_rate: -1,
                    enable_intra_refresh: false,
                    intra_refresh_period: -1,
                    intra_refresh_count: -1,
                    max_num_ref_frames: -1,
                    gop_length: -1,
                    p_frame_strategy: -1,
                    rate_control_mode: -1,
                    rc_buffer_size: -1,
                    rc_initial_delay: -1,
                    rc_max_bitrate: -1,
                    rc_average_bitrate: -1,
                    enable_weighted_prediction: false,
                },
                quality_preset: EncoderQualityPresetDefault {
                    variant: EncoderQualityPresetDefaultVariant::Speed,
                },
                enable_vbaq: false,
                amf: AmfConfigDefault {
                    gui_collapsed: true,
                    enable_pre_analysis: false,
                    enable_hmqb: false,
                    use_preproc: false,
                    preproc_sigma: 4,
                    preproc_tor: 7,
                },
                software: SoftwareEncodingConfigDefault {
                    gui_collapsed: true,
                    force_software_encoding: false,
                    thread_count: 0,
                },
            },
            mediacodec_extra_options: {
                fn int32_default(int32: i32) -> MediacodecPropertyDefault {
                    MediacodecPropertyDefault {
                        ty: MediacodecPropTypeDefault {
                            variant: MediacodecPropTypeDefaultVariant::Int32,
                        },
                        value: int32.to_string(),
                    }
                }
                DictionaryDefault {
                    gui_collapsed: true,
                    key: "".into(),
                    value: int32_default(0),
                    content: vec![
                        ("operating-rate".into(), int32_default(i32::MAX)),
                        ("priority".into(), int32_default(0)),
                        // low-latency: only applicable on API level 30. Quest 1 and 2 might not be
                        // cabable, since they are on level 29.
                        // ("low-latency".into(), int32_default(1)), // Android smartphones crashes enabling this feature (https://github.com/PhoneVR-Developers/alvr-cardboard/issues/5)
                        (
                            "vendor.qti-ext-dec-low-latency.enable".into(),
                            int32_default(1),
                        ),
                    ],
                }
            },
            frame_gaze: frame_gaze::defaults(),
            foveated_encoding: SwitchDefault {
                enabled: true,
                content: FoveatedEncodingConfigDefault {
                    gui_collapsed: true,
                    force_enable: false,
                    center_size: ArrayDefault {
                        gui_collapsed: false,
                        content: [0.45, 0.4],
                    },
                    center_shift: ArrayDefault {
                        gui_collapsed: false,
                        content: [0.4, 0.1],
                    },
                    edge_ratio: ArrayDefault {
                        gui_collapsed: false,
                        content: [4., 5.],
                    },
                },
            },
            clientside_foveation: SwitchDefault {
                enabled: false,
                content: ClientsideFoveationConfigDefault {
                    mode: ClientsideFoveationModeDefault {
                        Static: ClientsideFoveationModeStaticDefault {
                            level: ClientsideFoveationLevelDefault {
                                variant: ClientsideFoveationLevelDefaultVariant::High,
                            },
                        },
                        Dynamic: ClientsideFoveationModeDynamicDefault {
                            max_level: ClientsideFoveationLevelDefault {
                                variant: ClientsideFoveationLevelDefaultVariant::High,
                            },
                        },
                        variant: ClientsideFoveationModeDefaultVariant::Dynamic,
                    },
                    vertical_offset_deg: 0.0,
                },
            },
            force_software_decoder: false,
            color_correction: SwitchDefault {
                enabled: false,
                content: ColorCorrectionConfigDefault {
                    brightness: 0.,
                    contrast: 0.,
                    saturation: 0.5,
                    gamma: 1.,
                    sharpening: 0.5,
                },
            },
        }
}
