#include "VideoEncoderAMF.h"
#include "SURFXR-common/packet_types.h"

#include "surfxr_server/Logger.h"
#include "surfxr_server/bindings.h"

#define AMF_THROW_IF(expr)                                                                         \
    {                                                                                              \
        AMF_RESULT res = expr;                                                                     \
        if (res != AMF_OK) {                                                                       \
            throw MakeException("AMF Error %d. %s", res, L#expr);                                  \
        }                                                                                          \
    }

amf::AMFComponentPtr VideoEncoderAMF::MakeEncoder(
    amf::AMF_SURFACE_FORMAT inputFormat,
    int width,
    int height,
    int codec,
    int refreshRate,
    int bitrateInMbits
) {
    const wchar_t* pCodec;

    amf_int32 frameRateIn = refreshRate;
    amf_int64 bitRateIn = bitrateInMbits * 1'000'000L; // in bits

    switch (codec) {
    case SURFXR_CODEC_H264:
        if (m_use10bit) {
            throw MakeException("H.264 10-bit encoding is not supported");
        }
        pCodec = AMFVideoEncoderVCE_AVC;
        break;
    case SURFXR_CODEC_HEVC:
        pCodec = AMFVideoEncoder_HEVC;
        break;
    case SURFXR_CODEC_AV1:
        pCodec = AMFVideoEncoder_AV1;
        break;
    default:
        throw MakeException("Unsupported video encoding %d", codec);
    }

    amf::AMFComponentPtr amfEncoder;
    // Create encoder component.
    AMF_THROW_IF(g_AMFFactory.GetFactory()->CreateComponent(m_amfContext, pCodec, &amfEncoder));

    switch (codec) {
    case SURFXR_CODEC_H264: {
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
        switch (Settings_Instance()->m_h264Profile) {
        case SURFXR_H264_PROFILE_BASELINE:
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_BASELINE);
            break;
        case SURFXR_H264_PROFILE_MAIN:
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_MAIN);
            break;
        default:
        case SURFXR_H264_PROFILE_HIGH:
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
            break;
        }
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL, 42);
        switch (Settings_Instance()->m_rateControlMode) {
        case SURFXR_CBR:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR
            );
            // Required for CBR to work correctly
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, Settings_Instance()->m_fillerData
            );
            break;
        case SURFXR_VBR:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR
            );
            break;
        }

        switch (Settings_Instance()->m_entropyCoding) {
        case SURFXR_CABAC:
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_CABAC);
            break;
        case SURFXR_CAVLC:
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, AMF_VIDEO_ENCODER_CALV);
            break;
        }

        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitRateIn);
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitRateIn);
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(width, height));
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(frameRateIn, 1));
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);

        switch (Settings_Instance()->m_encoderQualityPreset) {
        case SURFXR_QUALITY:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY
            );
            break;
        case SURFXR_BALANCED:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED
            );
            break;
        case SURFXR_SPEED:
        default:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_QUALITY_PRESET, AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED
            );
            break;
        }

        amf::AMFCapsPtr caps;
        if (amfEncoder->GetCaps(&caps) == AMF_OK) {
            caps->GetProperty(AMF_VIDEO_ENCODER_CAP_PRE_ANALYSIS, &m_hasPreAnalysis);
            caps->GetProperty(AMF_VIDEO_ENCODER_CAPS_QUERY_TIMEOUT_SUPPORT, &m_hasQueryTimeout);
        }

        if (Settings_Instance()->m_enableAmfPreAnalysis) {
            if (!Settings_Instance()->m_useAmfPreproc || Settings_Instance()->m_use10bitEncoder) {
                Warn("Pre-analysis could not be enabled because \"Use preproc\" is not enabled or "
                     "\"Reduce color banding\" is enabled.");
            } else if (m_hasPreAnalysis) {
                Warn("Enabling h264 pre-analysis. You may experience higher latency when this is "
                     "enabled.");
                amfEncoder->SetProperty(
                    AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE,
                    Settings_Instance()->m_enableAmfPreAnalysis
                );
            } else {
                Warn("Pre-analysis could not be enabled because your GPU does not support it for "
                     "h264 encoding.");
            }
        }

        // Enable Full Range
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, true);

        // Use BT.2020 for HDR encoding, BT.709 otherwise.
        if (Settings_Instance()->m_enableHdr) {
            // Also specify the input formats for HDR to prevent incorrect color conversions
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020
            );
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_INPUT_TRANSFER_CHARACTERISTIC,
                AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22
            ); // sRGB
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_INPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT2020
            );

            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020
            );
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22
            ); // sRGB
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT2020
            );
        } else {
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709
            );
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22
            ); // sRGB
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT709
            );
        }

        // No noticable performance difference and should improve subjective quality by allocating
        // more bits to smooth areas
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, Settings_Instance()->m_enableVbaq);

        // May impact performance but improves quality in high-motion areas
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE, Settings_Instance()->m_enableAmfHmqb
        );

        // Turns Off IDR/I Frames
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, 0);

        // Disable AUD to produce the same stream format as VideoEncoderNVENC.
        // FIXME: This option doesn't work in 22.10.3, but works in versions prior 22.5.1
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_INSERT_AUD, false);

        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitRateIn / frameRateIn * 1.1);

        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_MAX_NUM_REFRAMES, 0);

        if (m_hasQueryTimeout) {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, 1000); // 1s timeout
        }
    }
    case SURFXR_CODEC_HEVC: {
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY
        );
        switch (Settings_Instance()->m_rateControlMode) {
        case SURFXR_CBR:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR
            );
            // Required for CBR to work correctly
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, Settings_Instance()->m_fillerData
            );
            break;
        case SURFXR_VBR:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR
            );
            break;
        }
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitRateIn);
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitRateIn);
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, ::AMFConstructSize(width, height)
        );
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_FRAMERATE, ::AMFConstructRate(frameRateIn, 1)
        );

        switch (Settings_Instance()->m_encoderQualityPreset) {
        case SURFXR_QUALITY:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY
            );
            break;
        case SURFXR_BALANCED:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED
            );
            break;
        case SURFXR_SPEED:
        default:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED
            );
            break;
        }

        if (m_use10bit) {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_10);
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10
            );
        } else {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_8);
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN
            );
        }

        amf::AMFCapsPtr caps;
        if (amfEncoder->GetCaps(&caps) == AMF_OK) {
            caps->GetProperty(AMF_VIDEO_ENCODER_HEVC_CAP_PRE_ANALYSIS, &m_hasPreAnalysis);
            caps->GetProperty(
                AMF_VIDEO_ENCODER_CAPS_HEVC_QUERY_TIMEOUT_SUPPORT, &m_hasQueryTimeout
            );
        }

        if (Settings_Instance()->m_enableAmfPreAnalysis) {
            if (!Settings_Instance()->m_useAmfPreproc || Settings_Instance()->m_use10bitEncoder) {
                Warn("Pre-analysis could not be enabled because \"Use preproc\" is not enabled or "
                     "\"Reduce color banding\" is enabled.");
            } else if (m_hasPreAnalysis) {
                Warn("Enabling HEVC pre-analysis. You may experience higher latency when this is "
                     "enabled.");
                amfEncoder->SetProperty(
                    AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE,
                    Settings_Instance()->m_enableAmfPreAnalysis
                );
            } else {
                Warn("Pre-analysis could not be enabled because your GPU does not support it for "
                     "HEVC encoding.");
            }
        }

        // Enable Full Range
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE, AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_FULL
        );

        // Use BT.2020 for HDR encoding, BT.709 otherwise.
        if (Settings_Instance()->m_enableHdr) {
            // Also specify the input formats for HDR to prevent incorrect color conversions
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE,
                AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020
            );
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_INPUT_TRANSFER_CHARACTERISTIC,
                AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22
            ); // sRGB
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT2020
            );

            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
                AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020
            );
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
                AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22
            ); // sRGB
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT2020
            );
        } else {
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
                AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709
            );
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
                AMF_COLOR_TRANSFER_CHARACTERISTIC_GAMMA22
            ); // sRGB
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT709
            );
        }

        // No noticable performance difference and should improve subjective quality by allocating
        // more bits to smooth areas
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, Settings_Instance()->m_enableVbaq
        );

        // May impact performance but improves quality in high-motion areas
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_HIGH_MOTION_QUALITY_BOOST_ENABLE,
            Settings_Instance()->m_enableAmfHmqb
        );

        // Turns Off IDR/I Frames
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, 0);
        // Set infinite GOP length
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, 0);

        // Disable AUD to produce the same stream format as VideoEncoderNVENC.
        // FIXME: This option doesn't work in 22.10.3, but works in versions prior 22.5.1
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, false);

        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitRateIn / frameRateIn * 1.1
        );

        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES, 0);

        if (m_hasQueryTimeout) {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, 1000); // 1s timeout
        }
    }
    case SURFXR_CODEC_AV1: {
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_USAGE, AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY
        );
        switch (Settings_Instance()->m_rateControlMode) {
        case SURFXR_CBR:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR
            );
            // Required for CBR to work correctly
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_FILLER_DATA, Settings_Instance()->m_fillerData
            );
            break;
        case SURFXR_VBR:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR
            );
            break;
        }
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitRateIn);
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitRateIn);
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, ::AMFConstructSize(width, height));
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_FRAMERATE, ::AMFConstructRate(frameRateIn, 1)
        );

        switch (Settings_Instance()->m_encoderQualityPreset) {
        case SURFXR_QUALITY:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY
            );
            break;
        case SURFXR_BALANCED:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED
            );
            break;
        case SURFXR_SPEED:
        default:
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED
            );
            break;
        }

        if (m_use10bit) {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_10);
            // There's no separate profile for 10-bit for AV1 (as of AMF v1.4.33). Assumedly MAIN
            // works fine for both.
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_PROFILE, AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN
            );
        } else {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_8);
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_PROFILE, AMF_VIDEO_ENCODER_AV1_PROFILE_MAIN
            );
        }

        // There is no VBAQ option for AV1. Instead it has CAQ (Content adaptive quantization)
        if (Settings_Instance()->m_enableVbaq) {
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_AQ_MODE, AMF_VIDEO_ENCODER_AV1_AQ_MODE_CAQ
            );
        } else {
            amfEncoder->SetProperty(
                AMF_VIDEO_ENCODER_AV1_AQ_MODE, AMF_VIDEO_ENCODER_AV1_AQ_MODE_NONE
            );
        }

        amf::AMFCapsPtr caps;
        if (amfEncoder->GetCaps(&caps) == AMF_OK) {
            caps->GetProperty(AMF_VIDEO_ENCODER_AV1_CAP_PRE_ANALYSIS, &m_hasPreAnalysis);
        }

        if (Settings_Instance()->m_enableAmfPreAnalysis) {
            if (!Settings_Instance()->m_useAmfPreproc || Settings_Instance()->m_use10bitEncoder) {
                Warn("Pre-analysis could not be enabled because \"Use preproc\" is not enabled or "
                     "\"Reduce color banding\" is enabled.");
            } else if (m_hasPreAnalysis) {
                Warn("Enabling AV1 pre-analysis. You may experience higher latency when this is "
                     "enabled.");
                amfEncoder->SetProperty(
                    AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE,
                    Settings_Instance()->m_enableAmfPreAnalysis
                );
            } else {
                Warn("Pre-analysis could not be enabled because your GPU does not support it for "
                     "AV1 encoding.");
            }
        }

        // Enable Full Range
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_JPEG
        );

        // May impact performance but improves quality in high-motion areas
        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_HIGH_MOTION_QUALITY_BOOST, Settings_Instance()->m_enableAmfHmqb
        );

        // Set infinite GOP length
        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, 0);

        amfEncoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bitRateIn / frameRateIn * 1.2
        );

        amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_NUM_REFRAMES, 0);

        // AV1 assumed always has support for query timeout.
        m_hasQueryTimeout = true;

        if (m_hasQueryTimeout) {
            amfEncoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, 1000); // 1s timeout
        }
    }
    }

    Debug("Configured %s.\n", pCodec);
    AMF_THROW_IF(amfEncoder->Init(inputFormat, width, height));

    Debug("Initialized %s.\n", pCodec);

    return amfEncoder;
}
