#include "CEncoder.h"
#include "EncoderThreadScheduling.h"

namespace {
bool supports_frame_process(ID3D11Device* device) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))
        && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))
        && desc.VendorId == 0x10de;
}
}

CEncoder::CEncoder()
    : m_bExiting(false)
    , m_targetTimestampNs(0) {
    m_encodeFinished.Set();
}

CEncoder::~CEncoder() {
    m_worker.reset();
    if (m_videoEncoder) {
        m_videoEncoder->Shutdown();
        m_videoEncoder.reset();
    }
    m_mailbox.reset();
    if (m_encoderRender) m_encoderRender->Shutdown();
}

void CEncoder::Initialize(std::shared_ptr<CD3DRender> d3dRender) {
    m_FrameRender = std::make_shared<FrameRender>(d3dRender);
    m_FrameRender->Startup();
    uint32_t encoderWidth, encoderHeight;
    m_FrameRender->GetEncodingResolution(&encoderWidth, &encoderHeight);

    char processMode[2]{};
    const bool forceInProcess = GetEnvironmentVariableA("SURFXR_FRAME_PROCESS", processMode,
        sizeof(processMode)) == 1 && processMode[0] == '0';
    const bool daemon=FrameWorkerDaemonRequired();
    if(daemon&&(!supports_frame_process(d3dRender->GetDevice())||Settings_Instance()->m_forceSwEncoding))
        throw std::runtime_error("Unified Frame daemon requires the desktop's NVIDIA hardware encoder");
    if ((Settings_Instance()->m_frameGaze.enabled||daemon) && !Settings_Instance()->m_forceSwEncoding
        && (!forceInProcess||daemon) && supports_frame_process(d3dRender->GetDevice())) {
        try {
            m_worker = std::make_unique<frame_worker::Parent>(d3dRender->GetDevice(),
                m_FrameRender->GetTexture().Get());
            FrameWorkerHapticsObserver(true);
            return;
        } catch (const std::exception& error) {
            if(daemon)throw;
            Warn("Frame worker unavailable, using in-process encoder: %s", error.what());
            FrameWorkerHapticsObserver(false);
            m_worker.reset();
        }
    }

    Exception vplException;
    Exception vceException;
    Exception nvencException;
#ifdef SURFXR_GPL
    Exception swException;

    if (Settings_Instance()->m_forceSwEncoding) {
        try {
            Debug("Try to use VideoEncoderSW.\n");
            m_videoEncoder
                = std::make_shared<VideoEncoderSW>(d3dRender, encoderWidth, encoderHeight);
            m_videoEncoder->Initialize();
            return;
        } catch (Exception e) {
            swException = e;
        }
    }
#endif

    try {
        Debug("Try to use VideoEncoderAMF.\n");
        m_videoEncoder = std::make_shared<VideoEncoderAMF>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
    } catch (Exception e) {
        vceException = e;
    }
    try {
        Debug("Try to use VideoEncoderNVENC.\n");
        // Only the negotiated 8-bit Frame atlas uses this new path. Keep AMF,
        // VPL, software and other clients on their existing resource contract.
        auto encoderRender = d3dRender;
        char asyncMode[2]{};
        const bool forceSync = GetEnvironmentVariableA(
            "SURFXR_FRAME_ASYNC_ENCODER", asyncMode, sizeof(asyncMode)) == 1 && asyncMode[0] == '0';
        if (Settings_Instance()->m_frameGaze.enabled && !forceSync) {
            auto independent = std::make_shared<CD3DRender>();
            if (independent->Initialize(Settings_Instance()->m_nAdapterIndex)) {
                try {
                    m_mailbox = std::make_unique<EncoderFrameMailbox>(d3dRender->GetDevice(),
                        independent->GetDevice(), m_FrameRender->GetTexture().Get());
                    encoderRender = independent;
                    m_encoderRender = independent;
                } catch (const std::exception& e) {
                    Warn("Independent Frame encoder handoff unavailable: %s", e.what());
                    independent->Shutdown();
                }
            }
        }
        m_videoEncoder
            = std::make_shared<VideoEncoderNVENC>(encoderRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        Info("Frame encoder independent device: %u", unsigned(m_mailbox != nullptr));
        return;
    } catch (Exception e) {
        nvencException = e;
        m_mailbox.reset();
        if (m_encoderRender) m_encoderRender->Shutdown();
        m_encoderRender.reset();
    }
    try {
        Debug("Try to use VideoEncoderVPL.\n");
        m_videoEncoder = std::make_shared<VideoEncoderVPL>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
    } catch (Exception e) {
        vplException = e;
    }
#ifdef SURFXR_GPL
    try {
        Debug("Try to use VideoEncoderSW.\n");
        m_videoEncoder = std::make_shared<VideoEncoderSW>(d3dRender, encoderWidth, encoderHeight);
        m_videoEncoder->Initialize();
        return;
    } catch (Exception e) {
        swException = e;
    }
    throw MakeException(
        "All VideoEncoder are not available. VCE: %s, NVENC: %s, VPL: %s, SW: %s",
        vceException.what(),
        nvencException.what(),
        vplException.what(),
        swException.what()
    );
#else
    throw MakeException(
        "All VideoEncoder are not available. VCE: %s, NVENC: %s, VPL: %s",
        vceException.what(),
        nvencException.what(),
        vplException.what()
    );
#endif
}

void CEncoder::SetViewParams(
    vr::HmdRect2_t projLeft,
    vr::HmdMatrix34_t eyeToHeadLeft,
    vr::HmdRect2_t projRight,
    vr::HmdMatrix34_t eyeToHeadRight
) {
    m_FrameRender->SetViewParams(projLeft, eyeToHeadLeft, projRight, eyeToHeadRight);
}

bool CEncoder::CopyToStaging(
    ID3D11Texture2D* pTexture[][2],
    vr::VRTextureBounds_t bounds[][2],
    vr::HmdMatrix34_t poses[],
    const vr::HmdMatrix34_t& targetPose,
    int layerCount,
    bool recentering,
    uint64_t presentationTime,
    uint64_t targetTimestampNs,
    const std::string& message,
    const std::string& debugText
) {
    m_pipelineStats.CaptureAttempt();
    const auto started = EncoderPipelineStats::NowUs();
    const auto processStarted = m_worker ? frame_worker::now_us() : 0;
    const auto processSlot = m_worker ? m_worker->begin_capture() : std::nullopt;
    if (m_worker && !processSlot) { m_pipelineStats.Rejected(); return false; }
    const auto slot = m_mailbox ? m_mailbox->TryBeginCapture() : std::nullopt;
    if (m_mailbox && !slot) {
        m_pipelineStats.Rejected();
        return false;
    }
    m_presentationTime = presentationTime;
    m_targetTimestampNs = targetTimestampNs;
    try {
        m_FrameRender->Startup();
        if (!m_FrameRender->RenderFrame(
            pTexture, bounds, poses, targetPose, layerCount, recentering, targetTimestampNs, message, debugText
        )) {
            if (processSlot) m_worker->cancel(*processSlot);
            if (slot) m_mailbox->CancelCapture(*slot);
            return false;
        }
    } catch (...) {
        if (processSlot) m_worker->cancel(*processSlot);
        if (slot) m_mailbox->CancelCapture(*slot);
        throw;
    }
    if (processSlot) {
        FfiFrameWorkerSource source{};
        if (!FrameWorkerSource(targetTimestampNs, &source)) {
            m_worker->cancel(*processSlot); return false;
        }
        // Capture identity comes from the actual original-image attempt, never
        // from pose matching or the worker's repeated encoding of a held lease.
        source.captureTimestampNs = processStarted * 1000;
        m_worker->publish(*processSlot, m_FrameRender->GetTexture().Get(),
            presentationTime, targetTimestampNs, source, processStarted);
    }
    if (slot) m_mailbox->EndCapture(*slot, m_FrameRender->GetTexture().Get(),
                                  presentationTime, targetTimestampNs);
    m_pipelineStats.Captured(started);
    return true;
}

void CEncoder::Run() {
    Debug("CEncoder: Start thread. Id=%d\n", GetCurrentThreadId());
    if (m_worker) {
        while (!m_bExiting) {
            try { m_worker->pump(); }
            catch (const std::exception& error) {
                Error("Frame worker failed: %s", error.what());
                ShutdownSteamvr(); break;
            }
            m_newFrameReady.Wait(2);
        }
        return;
    }
    const EncoderThreadScheduling scheduling;

    while (!m_bExiting) {
        m_pipelineStats.Report(m_mailbox != nullptr, m_mailbox ? m_mailbox->TakeDiscarded() : 0);
        if (m_mailbox) {
            EncoderFrameMailbox::Frame frame{};
            if (!m_mailbox->TryBeginEncode(frame)) {
                m_newFrameReady.Wait(1);
                continue;
            }
            if (m_bExiting) {
                m_mailbox->EndEncode(frame.slot);
                break;
            }
            const auto started = EncoderPipelineStats::NowUs();
            const auto queueUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                EncoderPipelineStats::Clock::now() - frame.queuedAt).count());
            try {
                m_videoEncoder->Transmit(frame.texture, frame.presentationTime,
                    frame.targetTimestampNs, m_scheduler.CheckIDRInsertion());
            } catch (...) {
                m_mailbox->EndEncode(frame.slot);
                throw;
            }
            m_mailbox->EndEncode(frame.slot);
            m_pipelineStats.Encoded(started, queueUs);
            continue;
        }
        if (!m_newFrameReady.Wait(50)) continue;
        if (m_bExiting)
            break;

        const auto started = EncoderPipelineStats::NowUs();
        if (m_FrameRender->GetTexture()) {
            m_videoEncoder->Transmit(
                m_FrameRender->GetTexture().Get(),
                m_presentationTime,
                m_targetTimestampNs,
                m_scheduler.CheckIDRInsertion()
            );
        }
        m_pipelineStats.Encoded(started, 0);

        m_encodeFinished.Set();
    }
}

void CEncoder::Stop() {
    m_bExiting = true;
    m_newFrameReady.Set();
    Join();
    if (m_worker) { FrameWorkerHapticsObserver(false); m_worker->stop(); }
    m_FrameRender.reset();
}

void CEncoder::NewFrameReady() {
    if (!m_mailbox && !m_worker) m_encodeFinished.Reset();
    m_newFrameReady.Set();
}

void CEncoder::WaitForEncode() { if (!m_mailbox && !m_worker) m_encodeFinished.Wait(); }

void CEncoder::OnStreamStart() { if (m_worker) m_worker->request_idr(); else m_scheduler.OnStreamStart(); }

void CEncoder::InsertIDR() { if (m_worker) m_worker->request_idr(); else m_scheduler.InsertIDR(); }
