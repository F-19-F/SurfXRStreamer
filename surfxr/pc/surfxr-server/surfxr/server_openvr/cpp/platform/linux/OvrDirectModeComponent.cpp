#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstdlib>
#include <iostream>
#include <unistd.h>

#include "OvrDirectModeComponent.h"

#include "surfxr_server/Logger.h"
#include "surfxr_server/include/openvr_math.h"
#include <cmath>
#include <cstring>
#include <vector>

#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "DirectModeWorker.inl"

void OvrDirectModeComponent::SetViewParams(const FfiViewParams params[2]) {
    std::unique_lock<std::mutex> lock(m_viewParamsMutex);
    m_viewParams[0] = params[0];
    m_viewParams[1] = params[1];
    m_viewParamsValid = true;
}

OvrDirectModeComponent::OvrDirectModeComponent(std::shared_ptr<PoseHistory> poseHistory)
    : m_poseHistory(poseHistory)
    , m_submitLayer(0) {
    m_encodeWorker = std::thread(&OvrDirectModeComponent::EncodeWorkerLoop, this);
    StartVsyncAnnouncer();
}

OvrDirectModeComponent::~OvrDirectModeComponent() {
    // Stops before the encode worker: the announcer's grid is what
    // PostPresent holds against, and a hold outliving the worker leaves a
    // present waiting on a frame nothing will encode.
    StopVsyncAnnouncer();
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_workerExit = true;
        m_jobPending = false;
    }
    m_jobCv.notify_one();
    if (m_encodeWorker.joinable()) {
        m_encodeWorker.join();
    }
    // The worker is gone; whatever job was pending will never present.
    closeWaitFds(m_job.waitFds);
    for (int fd : m_syncFds) {
        if (fd >= 0) {
            close(fd);
        }
    }
}

void OvrDirectModeComponent::CreateSwapTextureSet(
    uint32_t unPid,
    const SwapTextureSetDesc_t* pSwapTextureSetDesc,
    SwapTextureSet_t* pOutSwapTextureSet
) {
    Info(
        "CreateSwapTextureSet pid=%d Format=%d %dx%d SampleCount=%d\n",
        unPid,
        pSwapTextureSetDesc->nFormat,
        pSwapTextureSetDesc->nWidth,
        pSwapTextureSetDesc->nHeight,
        pSwapTextureSetDesc->nSampleCount
    );

    ProcessResource* processResource = new ProcessResource();
    processResource->textDesc = *pSwapTextureSetDesc;
    processResource->pid = unPid;
    m_swapchainIndices[processResource] = 0;

    {
        auto pid = getpid();
        Info("VrServer PID %d\n", pid);
    }

    uint32_t usageFlags = static_cast<uint32_t>(
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled
        | vk::ImageUsageFlagBits::eInputAttachment
    );

    uint32_t creationFlags = static_cast<uint32_t>(vk::ImageCreateFlagBits::eMutableFormat);

    for (int i = 0; i < 3; i++) {
        vr::SharedTextureHandle_t myHandle = 0;
        bool success = vr::VRIPCResourceManager()->NewSharedVulkanImage(
            pSwapTextureSetDesc->nFormat,
            pSwapTextureSetDesc->nWidth,
            pSwapTextureSetDesc->nHeight,
            true,
            false,
            true,
            1,
            1,
            creationFlags,
            usageFlags,
            &myHandle
        );

        uint64_t ipcHandle = 0;
        vr::VRIPCResourceManager()->RefResource(myHandle, &ipcHandle);

        if (!success) {
            Error("VRCIPCResourceManager: Failed to create shared texture\n");
            CleanupProcessResource(processResource);
            return;
        }

        int fd = 0;
        auto ret = vr::VRIPCResourceManager()->ReceiveSharedFd(ipcHandle, &fd);
        if (ret == false) {
            Error("Failed to get fd for texture\n");
            vr::VRIPCResourceManager()->UnrefResource(myHandle);
            CleanupProcessResource(processResource);
            return;
        }

        processResource->fds[i] = fd;
        processResource->sharedHandles[i] = myHandle;

        m_handleMap.insert(
            std::make_pair(processResource->sharedHandles[i], std::make_pair(processResource, i))
        );
        pOutSwapTextureSet->rSharedTextureHandles[i] = myHandle;
        Info("Created Texture %d %p\n", i, processResource->sharedHandles[i]);
    }
}

/** Used to textures created using CreateSwapTextureSet.  Only one of the set's handles needs to be
 * used to destroy the entire set. */
void OvrDirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) {
    Info("DestroySwapTextureSet %p\n", sharedTextureHandle);

    m_presentMutex.lock();
    auto id = m_handleMap.find(sharedTextureHandle);
    if (id != m_handleMap.end()) {
        ProcessResource* p = id->second.first;
        CleanupProcessResource(p);
    } else {
        Debug("Requested to destroy not managing texture. handle:%p\n", sharedTextureHandle);
    }
    m_presentMutex.unlock();
}

/** Used to purge all texture sets for a given process. */
void OvrDirectModeComponent::DestroyAllSwapTextureSets(uint32_t unPid) {
    Info("DestroyAllSwapTextureSets pid=%d\n", unPid);

    m_presentMutex.lock();
    std::vector<ProcessResource*> resourcesToDestroy;
    for (auto it = m_handleMap.begin(); it != m_handleMap.end(); ++it) {
        if (it->second.first->pid == unPid && it->second.second == 0) {
            resourcesToDestroy.push_back(it->second.first);
        }
    }
    for (auto* p : resourcesToDestroy) {
        CleanupProcessResource(p);
    }
    m_presentMutex.unlock();
}

void OvrDirectModeComponent::CleanupProcessResource(ProcessResource* processResource) {
    for (int i = 0; i < 3; i++) {
        if (processResource->sharedHandles[i]) {
            vr::VRIPCResourceManager()->UnrefResource(processResource->sharedHandles[i]);
            m_handleMap.erase(processResource->sharedHandles[i]);
            processResource->sharedHandles[i] = 0;
        }
        if (processResource->fds[i] >= 0) {
            close(processResource->fds[i]);
            processResource->fds[i] = -1;
        }
    }
    m_swapchainIndices.erase(processResource);
    delete processResource;
}

/** After Present returns, calls this to get the next index to use for rendering. */
void OvrDirectModeComponent::GetNextSwapTextureSetIndex(
    vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]
) {
    Debug("OvrDirectModeComponent::GetNextSwapTextureSetIndex");

    m_presentMutex.lock();
    for (int eye = 0; eye < 2; eye++) {
        auto it = m_handleMap.find(sharedTextureHandles[eye]);
        if (it == m_handleMap.end()) {
            continue;
        }
        auto& idx = m_swapchainIndices[it->second.first];
        idx = (idx + 1) % 3;
        if (pIndices) {
            (*pIndices)[eye] = idx;
        }
    }
    m_presentMutex.unlock();
}

/** Call once per layer to draw for this frame.  One shared texture handle per eye.  Textures must
 * be created using CreateSwapTextureSet and should be alternated per frame.  Call Present once all
 * layers have been submitted. */
void OvrDirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) {
    m_presentMutex.lock();

    auto pPose = &perEye[0].mHmdPose; // TODO: are both poses the same? Name HMD suggests yes.

    if (m_submitLayer == 0) {
        // Detect FrameIndex of submitted frame by pPose.
        // This is important part to achieve smooth headtracking.
        // We search for history of TrackingInfo and find the TrackingInfo which have nearest matrix
        // value.

        auto pose = m_poseHistory->GetBestPoseMatch(*pPose);
        if (pose) {
            // found the frameIndex
            m_prevTargetTimestampNs = m_targetTimestampNs;
            m_targetTimestampNs = pose->targetTimestampNs;

            m_prevFramePoseRotation = m_framePoseRotation;
            m_framePoseRotation.x = pose->motion.pose.orientation.x;
            m_framePoseRotation.y = pose->motion.pose.orientation.y;
            m_framePoseRotation.z = pose->motion.pose.orientation.z;
            m_framePoseRotation.w = pose->motion.pose.orientation.w;
        } else {
            m_targetTimestampNs = 0;
            m_framePoseRotation = HmdQuaternion_Init(0.0, 0.0, 0.0, 0.0);
        }
    }
    if (m_submitLayer < MAX_LAYERS) {
        m_submitLayers[m_submitLayer][0] = perEye[0];
        m_submitLayers[m_submitLayer][1] = perEye[1];
        m_submitLayer++;
    } else {
        Warn("Too many layers submitted!\n");
    }

    m_presentMutex.unlock();
}

/** Submits queued layers for display. */
void OvrDirectModeComponent::Present(vr::SharedTextureHandle_t syncTexture) {
    m_submitLayer = 0;

    switch (m_encoderState.load()) {
    case EncoderState::RebuildRequested: {
        Info("Rebuilding encoder from negotiated settings\n");
        DrainPendingJob();
        std::lock_guard<std::mutex> encLock(m_encMutex);
        m_encGeneration++;
        enc.shutdown();
        layer0Texts.fill(0);
        m_encoderState = EncoderState::Streaming;
        break;
    }
    case EncoderState::ShutdownRequested: {
        DrainPendingJob();
        std::lock_guard<std::mutex> encLock(m_encMutex);
        m_encGeneration++;
        enc.shutdown();
        layer0Texts.fill(0);
        m_encoderState = EncoderState::Idle;
        return;
    }
    // Before a client connects there are no negotiated settings, so building
    // an encoder here would use wrong defaults and fail noisily. Do nothing
    // until streaming starts.
    case EncoderState::Idle:
        return;
    case EncoderState::Streaming:
        break;
    }

    std::optional<u32> leftIdx;
    std::optional<u32> rightIdx;

    for (u32 i = 0; i < layer0Texts.size(); ++i) {
        if (layer0Texts[i] == m_submitLayers[0][0].hTexture)
            leftIdx = i;

        if (layer0Texts[i] == m_submitLayers[0][1].hTexture)
            rightIdx = i;
    }

    if (!leftIdx.has_value() || !rightIdx.has_value()) {
        auto leftIt = m_handleMap.find(m_submitLayers[0][0].hTexture);
        auto rightIt = m_handleMap.find(m_submitLayers[0][1].hTexture);

        if (leftIt == m_handleMap.end() || rightIt == m_handleMap.end()) {
            Error(
                "Textures not found in handle map %llu, %llu\n",
                m_submitLayers[0][0].hTexture,
                m_submitLayers[0][1].hTexture
            );
            return;
        }

        std::array<int, 6> fds;

        // Vulkan takes ownership of an fd when it imports it, so the copies kept
        // in the handle map are only good for a single import. Hand it a dup
        // every time or the next rebuild fails with InvalidExternalHandle.
        for (u32 i = 0; i < 3; ++i) {
            layer0Texts[i] = leftIt->second.first->sharedHandles[i];
            fds[i] = dup(leftIt->second.first->fds[i]);
        }
        for (u32 i = 0; i < 3; ++i) {
            layer0Texts[i + 3] = rightIt->second.first->sharedHandles[i];
            fds[i + 3] = dup(rightIt->second.first->fds[i]);
        }

        for (u32 i = 0; i < fds.size(); ++i) {
            if (fds[i] == -1) {
                Error("Could not duplicate texture fd %u, skipping encoder setup\n", i);
                for (u32 j = 0; j < fds.size(); ++j) {
                    if (fds[j] != -1)
                        close(fds[j]);
                }
                layer0Texts.fill(0);
                return;
            }
        }

        // Keep a second dup of each texture fd for exporting write fences at
        // Present; Vulkan consumes the import dups below.
        for (u32 i = 0; i < fds.size(); ++i) {
            if (m_syncFds[i] >= 0) {
                close(m_syncFds[i]);
            }
            m_syncFds[i] = dup(fds[i]);
        }

        auto const& settings = Settings_Instance();

        // Hopefully it's the same for both eyes in a layer
        auto& desc = leftIt->second.first->textDesc;

        surfxr::render::RendererCreateInfo rendererCI {
            .format = (vk::Format)(VkFormat)desc.nFormat,
            .inputEyeExtent {
                .width = desc.nWidth,
                .height = desc.nHeight,
            },
            .outputExtent {
                .width = settings->m_recommendedTargetWidth,
                .height = settings->m_recommendedTargetHeight,
            },
            .inputImgFds = fds,
        };

        // Renderer and encoder setup can throw (Vulkan import, VAAPI). An
        // exception escaping Present kills vrserver, so log and stay idle.
        DrainPendingJob();
        std::lock_guard<std::mutex> encLock(m_encMutex);
        m_encGeneration++;

        try {
            enc.createImages(rendererCI);
            enc.initEncoding();
        } catch (std::exception const& e) {
            Error(
                "Could not set up the encoder: %s. Staying idle until the next client connect.\n",
                e.what()
            );
            enc.shutdown();
            layer0Texts.fill(0);
            m_encoderState = EncoderState::Idle;
        }

        // We'll get em next time
        return;
    }

    // TODO: Merge layers or something

    FrameJob job {};
    job.leftIdx = leftIdx.value();
    job.rightIdx = rightIdx.value();
    job.targetTimestampNs = m_targetTimestampNs;
    job.renderOrientation = m_framePoseRotation;
    job.generation = m_encGeneration;
    job.enqueueTime = std::chrono::steady_clock::now();

    // Export each eye's write fences so the render submission starts exactly
    // when the writer finishes. Both eyes or neither: a single armed eye
    // would order one read correctly and leave the other racing, which is
    // strictly harder to diagnose than the symmetric fallback.
    {
        u32 const eyeSlot[2] = { leftIdx.value(), rightIdx.value() };
        for (int e = 0; e < 2; ++e) {
            if (m_syncFds[eyeSlot[e]] >= 0) {
                job.waitFds[e] = exportReadSyncFile(m_syncFds[eyeSlot[e]]);
            }
        }
        if (job.waitFds[0] < 0 || job.waitFds[1] < 0) {
            closeWaitFds(job.waitFds);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_workerExit) {
            // Teardown already drained the mailbox; do not repopulate it.
            closeWaitFds(job.waitFds);
            return;
        }
        // Latest wins: a still-pending job is replaced, and its fences will
        // never be waited on, so close them here.
        if (m_jobPending) {
            closeWaitFds(m_job.waitFds);
        }
        m_job = job;
        m_jobPending = true;
    }
    m_jobCv.notify_one();
}

void OvrDirectModeComponent::PostPresent(const Throttling_t* pThrottling) {
    // Prop_Hmd_SupportsAppThrottling_Bool is set, so SteamVR sends throttle
    // hints here and predicts poses assuming we honor them. Ignoring them
    // while it throttles an app predicts poses for a cadence we are not
    // keeping, which reprojects the world against head motion.
    uint32_t throttleFrames = 0;
    if (pThrottling != nullptr) {
        throttleFrames = pThrottling->nFramesToThrottle;
        if (pThrottling->nFramesToThrottle != m_lastThrottleFrames
            || pThrottling->nAdditionalFramesToPredict != m_lastPredictFrames) {
            m_lastThrottleFrames = pThrottling->nFramesToThrottle;
            m_lastPredictFrames = pThrottling->nAdditionalFramesToPredict;
            Info(
                "Throttling: framesToThrottle=%u additionalFramesToPredict=%u\n",
                pThrottling->nFramesToThrottle,
                pThrottling->nAdditionalFramesToPredict
            );
        }
    }

    PaceAfterPresent(throttleFrames);
}

void OvrDirectModeComponent::GetFrameTiming(vr::DriverDirectMode_FrameTiming* pFrameTiming) {
    if (pFrameTiming == nullptr
        || pFrameTiming->m_nSize < sizeof(vr::DriverDirectMode_FrameTiming)) {
        return;
    }

    // Skipped ticks are not reported as dropped frames: the compositor reads a
    // drop as GPU overload and enables interleaved reprojection, which halves
    // the rate, so the grid then skips a tick every frame and reports another
    // drop, throttled from then on. Consume the counter without reporting it
    // until there is a channel that does not throttle.
    (void)TakeSkippedVsyncs();
    pFrameTiming->m_nNumFramePresents = 1;
    pFrameTiming->m_nNumMisPresented = 0;
    pFrameTiming->m_nNumDroppedFrames = 0;
    // These bits overlap the throttle mask and must not be echoed back
    // unhandled, per the interface header.
    pFrameTiming->m_nReprojectionFlags = 0;
}
