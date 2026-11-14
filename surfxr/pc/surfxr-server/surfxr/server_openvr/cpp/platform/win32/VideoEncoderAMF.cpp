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

const wchar_t* VideoEncoderAMF::START_TIME_PROPERTY = L"StartTimeProperty";
const wchar_t* VideoEncoderAMF::FRAME_INDEX_PROPERTY = L"FrameIndexProperty";

AMFPipe::AMFPipe(amf::AMFComponentPtr src, AMFDataReceiver receiver)
    : m_amfComponentSrc(src)
    , m_receiver(receiver) { }

AMFPipe::~AMFPipe() {
    Debug("AMFPipe::~AMFPipe()  m_amfComponentSrc->Drain\n");
    m_amfComponentSrc->Drain();
}

void AMFPipe::doPassthrough(bool hasQueryTimeout, uint32_t timerResolution) {
    amf::AMFDataPtr data = nullptr;
    if (hasQueryTimeout) {
        AMF_RESULT res = m_amfComponentSrc->QueryOutput(&data);
        if (res == AMF_OK && data) {
            m_receiver(data);
        } else {
            Debug("Failed to get AMF component data. Last status: %d.\n", res);
        }
    } else {
        uint16_t timeout = 1000; // 1s timeout
        AMF_RESULT res = m_amfComponentSrc->QueryOutput(&data);

        timeBeginPeriod(timerResolution);
        while (!data && --timeout != 0) {
            amf_sleep(1);
            res = m_amfComponentSrc->QueryOutput(&data);
        }
        timeEndPeriod(timerResolution);

        if (data) {
            m_receiver(data);
        } else {
            Debug("Failed to get AMF component data. Last status: %d.\n", res);
        }
    }
}

AMFSolidPipe::AMFSolidPipe(amf::AMFComponentPtr src, amf::AMFComponentPtr dst)
    : AMFPipe(src, std::bind(&AMFSolidPipe::Passthrough, this, std::placeholders::_1))
    , m_amfComponentDst(dst) { }

void AMFSolidPipe::Passthrough(AMFDataPtr data) {
    auto res = m_amfComponentDst->SubmitInput(data);
    switch (res) {
    case AMF_OK:
        break;
    case AMF_INPUT_FULL:
        Debug("m_amfComponentDst->SubmitInput returns AMF_INPUT_FULL.\n");
        break;
    case AMF_NEED_MORE_INPUT:
        Debug("m_amfComponentDst->SubmitInput returns AMF_NEED_MORE_INPUT.\n");
        break;
    default:
        Debug("m_amfComponentDst->SubmitInput returns code %d.\n", res);
        break;
    }
}

AMFPipeline::AMFPipeline()
    : m_pipes() {
    TIMECAPS tc;
    m_timerResolution = timeGetDevCaps(&tc, sizeof(tc)) == TIMERR_NOERROR ? tc.wPeriodMin : 1;
}

AMFPipeline::~AMFPipeline() {
    for (auto& pipe : m_pipes) {
        delete pipe;
    }
}

void AMFPipeline::Connect(AMFPipePtr pipe) { m_pipes.emplace_back(pipe); }

void AMFPipeline::Run(bool hasQueryTimeout) {
    for (auto& pipe : m_pipes) {
        pipe->doPassthrough(hasQueryTimeout, m_timerResolution);
    }
}

//
// VideoEncoderAMF
//

VideoEncoderAMF::VideoEncoderAMF(std::shared_ptr<CD3DRender> d3dRender, int width, int height)
    : m_d3dRender(d3dRender)
    , m_codec(Settings_Instance()->m_codec)
    , m_refreshRate(Settings_Instance()->m_refreshRate)
    , m_renderWidth(width)
    , m_renderHeight(height)
    , m_bitrateInMBits(30)
    , m_surfaceFormat(amf::AMF_SURFACE_RGBA)
    , m_use10bit(Settings_Instance()->m_use10bitEncoder)
    , m_hasQueryTimeout(false) {
    if (Settings_Instance()->m_enableHdr) {
        // Bypass preprocessor and converters for HDR, since it will already be YUV
        m_surfaceFormat = m_use10bit ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12;
    }
}

VideoEncoderAMF::~VideoEncoderAMF() { }

amf::AMFComponentPtr VideoEncoderAMF::MakeConverter(
    amf::AMF_SURFACE_FORMAT inputFormat, int width, int height, amf::AMF_SURFACE_FORMAT outputFormat
) {
    amf::AMFComponentPtr amfConverter;
    AMF_THROW_IF(
        g_AMFFactory.GetFactory()->CreateComponent(m_amfContext, AMFVideoConverter, &amfConverter)
    );

    AMF_THROW_IF(amfConverter->SetProperty(AMF_VIDEO_CONVERTER_MEMORY_TYPE, amf::AMF_MEMORY_DX11));
    AMF_THROW_IF(amfConverter->SetProperty(AMF_VIDEO_CONVERTER_OUTPUT_FORMAT, outputFormat));
    AMF_THROW_IF(amfConverter->SetProperty(
        AMF_VIDEO_CONVERTER_OUTPUT_SIZE, ::AMFConstructSize(width, height)
    ));

    AMF_THROW_IF(amfConverter->Init(inputFormat, width, height));

    Debug("Initialized %s.\n", AMFVideoConverter);
    return amfConverter;
}

amf::AMFComponentPtr
VideoEncoderAMF::MakePreprocessor(amf::AMF_SURFACE_FORMAT inputFormat, int width, int height) {
    amf::AMFComponentPtr amfPreprocessor;
    AMF_THROW_IF(
        g_AMFFactory.GetFactory()->CreateComponent(m_amfContext, AMFPreProcessing, &amfPreprocessor)
    );

    AMF_THROW_IF(amfPreprocessor->SetProperty(AMF_PP_ENGINE_TYPE, amf::AMF_MEMORY_DX11));
    AMF_THROW_IF(amfPreprocessor->SetProperty(
        AMF_PP_ADAPTIVE_FILTER_STRENGTH, Settings_Instance()->m_amfPreProcSigma
    ));
    AMF_THROW_IF(amfPreprocessor->SetProperty(
        AMF_PP_ADAPTIVE_FILTER_SENSITIVITY, Settings_Instance()->m_amfPreProcTor
    ));

    AMF_THROW_IF(amfPreprocessor->Init(inputFormat, width, height));

    Debug("Initialized %s.\n", AMFPreProcessing);
    return amfPreprocessor;
}

void VideoEncoderAMF::Initialize() {
    Debug("Initializing VideoEncoderAMF.\n");
    AMF_THROW_IF(g_AMFFactory.Init());

    AMF_THROW_IF(g_AMFFactory.GetFactory()->CreateContext(&m_amfContext));
    AMF_THROW_IF(m_amfContext->InitDX11(m_d3dRender->GetDevice()));

    amf::AMF_SURFACE_FORMAT inFormat = m_surfaceFormat;
    if (Settings_Instance()->m_enableHdr) {
        // Bypass preprocessor and converters for HDR, since it will already be YUV
        ;
    } else if (m_use10bit) {
        inFormat = amf::AMF_SURFACE_R10G10B10A2;
        m_amfComponents.emplace_back(
            MakeConverter(m_surfaceFormat, m_renderWidth, m_renderHeight, inFormat)
        );
    } else {
        if (Settings_Instance()->m_useAmfPreproc) {
            inFormat = amf::AMF_SURFACE_NV12;
            m_amfComponents.emplace_back(
                MakeConverter(m_surfaceFormat, m_renderWidth, m_renderHeight, inFormat)
            );
            m_amfComponents.emplace_back(MakePreprocessor(inFormat, m_renderWidth, m_renderHeight));
        }
    }
    m_amfComponents.emplace_back(MakeEncoder(
        inFormat, m_renderWidth, m_renderHeight, m_codec, m_refreshRate, m_bitrateInMBits
    ));

    m_pipeline = new AMFPipeline();
    for (int i = 0; i < m_amfComponents.size() - 1; i++) {
        m_pipeline->Connect(new AMFSolidPipe(m_amfComponents[i], m_amfComponents[i + 1]));
    }

    m_pipeline->Connect(new AMFPipe(
        m_amfComponents.back(), std::bind(&VideoEncoderAMF::Receive, this, std::placeholders::_1)
    ));

    Debug("Successfully initialized VideoEncoderAMF.\n");
}

void VideoEncoderAMF::Shutdown() {
    Debug("Shutting down VideoEncoderAMF.\n");

    delete m_pipeline;

    for (auto& component : m_amfComponents) {
        component->Terminate();
    }
    m_amfComponents.clear();

    m_amfContext->Terminate();
    m_amfContext = NULL;

    g_AMFFactory.Terminate();

    if (fpOut) {
        fpOut.close();
    }
    Debug("Successfully shutdown VideoEncoderAMF.\n");
}

void VideoEncoderAMF::Transmit(
    ID3D11Texture2D* pTexture, uint64_t presentationTime, uint64_t targetTimestampNs, bool insertIDR
) {
    amf::AMFSurfacePtr surface;
    // Surface is cached by AMF.

    auto params = GetDynamicEncoderParams();
    if (params.updated) {
        amf_int64 bitRateIn = params.bitrate_bps / params.framerate * m_refreshRate; // in bps

        const amf_int64 maxRate = 1'000'000'000;
        if (bitRateIn > maxRate) {
            // Don't warn if we're within 1% (10 mbps) of the max bitrate because that case gets
            // triggered by rounding errors with the max supported rate
            if (bitRateIn > maxRate + maxRate / 100) {
                Warn("Set bitrate over max supported by AMF, clamping to 1000mbps");
            }
            bitRateIn = maxRate;
        }

        if (m_codec == SURFXR_CODEC_H264) {
            m_amfComponents.back()->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitRateIn);
            m_amfComponents.back()->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitRateIn);
            m_amfComponents.back()->SetProperty(
                AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitRateIn / m_refreshRate * 1.1
            );
        } else if (m_codec == SURFXR_CODEC_HEVC) {
            m_amfComponents.back()->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitRateIn);
            m_amfComponents.back()->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitRateIn);
            m_amfComponents.back()->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitRateIn / m_refreshRate * 1.1
            );
        } else {
            m_amfComponents.back()->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitRateIn);
            m_amfComponents.back()->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitRateIn);
            m_amfComponents.back()->SetProperty(
                AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bitRateIn / m_refreshRate * 1.1
            );
        }

        if (Settings_Instance()->m_amdBitrateCorruptionFix) {
            RequestIDR();
        }
    }

    AMF_THROW_IF(m_amfContext->AllocSurface(
        amf::AMF_MEMORY_DX11, m_surfaceFormat, m_renderWidth, m_renderHeight, &surface
    ));
    ID3D11Texture2D* textureDX11 = (ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative(
    ); // no reference counting - do not Release()
    m_d3dRender->GetContext()->CopyResource(textureDX11, pTexture);

    amf_pts start_time = amf_high_precision_clock();
    surface->SetProperty(START_TIME_PROPERTY, start_time);
    surface->SetProperty(FRAME_INDEX_PROPERTY, targetTimestampNs);

    ApplyFrameProperties(surface, insertIDR);

    m_amfComponents.front()->SubmitInput(surface);
    m_pipeline->Run(m_hasQueryTimeout);
}

void VideoEncoderAMF::Receive(AMFDataPtr data) {
    amf_pts current_time = amf_high_precision_clock();
    amf_pts start_time = 0;
    uint64_t targetTimestampNs;
    data->GetProperty(START_TIME_PROPERTY, &start_time);
    data->GetProperty(FRAME_INDEX_PROPERTY, &targetTimestampNs);

    amf::AMFBufferPtr buffer(data); // query for buffer interface

    char* p = reinterpret_cast<char*>(buffer->GetNative());
    int length = static_cast<int>(buffer->GetSize());

    if (fpOut) {
        fpOut.write(p, length);
    }

    uint64_t type;
    bool isIdr;
    if (m_codec == SURFXR_CODEC_H264) {
        data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
        isIdr = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
    } else {
        data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
        isIdr = type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
    }

    ParseFrameNals(m_codec, reinterpret_cast<uint8_t*>(p), length, targetTimestampNs, isIdr);
}

void VideoEncoderAMF::ApplyFrameProperties(const amf::AMFSurfacePtr& surface, bool insertIDR) {
    switch (m_codec) {
    case SURFXR_CODEC_H264:
        // FIXME: This option doesn't work in drivers 22.3.1 - 22.5.1, but works in 22.10.3
        surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_AUD, false);
        if (insertIDR) {
            Debug("Inserting IDR frame for H.264.\n");
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
            surface->SetProperty(
                AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR
            );
        }
        break;
    case SURFXR_CODEC_HEVC:
        // FIXME: This option works with 22.10.3, but may not work with older drivers
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_AUD, false);
        if (insertIDR) {
            Debug("Inserting IDR frame for H.265.\n");
            // Insert VPS,SPS,PPS
            // These options don't work properly on older AMD driver (Radeon Software 17.7, AMF
            // Runtime 1.4.4) Fixed in 18.9.2 & 1.4.9
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
            surface->SetProperty(
                AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR
            );
        }
        break;
    case SURFXR_CODEC_AV1:
        if (insertIDR) {
            Debug("Inserting IDR frame for AV1.\n");
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
            surface->SetProperty(
                AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY
            );
        }
        break;
    default:
        throw MakeException("Invalid video codec");
    }
}
