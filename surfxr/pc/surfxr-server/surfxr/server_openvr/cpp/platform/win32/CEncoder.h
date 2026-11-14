#pragma once
#include "shared/d3drender.h"

#include "shared/threadtools.h"

#include "FrameRender.h"
#include "EncoderFrameMailbox.h"
#include "EncoderPipelineStats.h"
#include "frame-worker/FrameWorkerParent.h"
#include "VideoEncoder.h"
#include "VideoEncoderAMF.h"
#include "VideoEncoderNVENC.h"
#include "VideoEncoderVPL.h"
#include "surfxr_server/Utils.h"
#include <d3d11.h>
#include <d3d11_1.h>
#include <map>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl.h>
#ifdef SURFXR_GPL
#include "VideoEncoderSW.h"
#endif
#include "surfxr_server/IDRScheduler.h"

using Microsoft::WRL::ComPtr;

//----------------------------------------------------------------------------
// Frame NVENC capture/encoding use separate devices and a bounded GPU mailbox.
// Other encoders retain the original synchronized staging path.
//----------------------------------------------------------------------------
class CEncoder : public CThread {
public:
    CEncoder();
    ~CEncoder();

    void Initialize(std::shared_ptr<CD3DRender> d3dRender);

    void SetViewParams(
        vr::HmdRect2_t projLeft,
        vr::HmdMatrix34_t eyeToHeadLeft,
        vr::HmdRect2_t projRight,
        vr::HmdMatrix34_t eyeToHeadRight
    );

    bool CopyToStaging(
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
    );

    virtual void Run();

    virtual void Stop();

    void NewFrameReady();

    void WaitForEncode();

    void OnStreamStart();

    // The child owns a socket exported for this connection, never the next one.
    bool RequiresSessionStop() const { return m_worker != nullptr; }

    void InsertIDR();

private:
    CThreadEvent m_newFrameReady, m_encodeFinished;
    std::shared_ptr<VideoEncoder> m_videoEncoder;
    std::atomic<bool> m_bExiting;
    uint64_t m_presentationTime;
    uint64_t m_targetTimestampNs;

    std::shared_ptr<FrameRender> m_FrameRender;
    std::shared_ptr<CD3DRender> m_encoderRender;
    std::unique_ptr<EncoderFrameMailbox> m_mailbox;
    std::unique_ptr<frame_worker::Parent> m_worker;
    EncoderPipelineStats m_pipelineStats;

    IDRScheduler m_scheduler;
};
