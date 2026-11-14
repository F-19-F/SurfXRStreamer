#include "SURFXR-common/packet_types.h"
#include "Logger.h"
#include "bindings.h"
#include <memory>
#include <mutex>
#include <array>
#include <cmath>
#include <vector>
#include <chrono>

#ifndef __APPLE__
// Workaround symbol clash in openvr.h / openvr_driver.h
namespace surfxr_chaperone {
#include <openvr.h>
}
using namespace surfxr_chaperone;
#include "FrameBoundarySync.h"
static FrameBoundarySync frameBoundarySync;
static double boundaryTime() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

std::mutex chaperone_mutex;

bool isOpenvrInit = false;

void InitOpenvrClient() {
    Debug("InitOpenvrClient");

#ifndef __APPLE__
    std::unique_lock<std::mutex> lock(chaperone_mutex);

    if (isOpenvrInit) {
        return;
    }

    vr::EVRInitError error;
    // Background needed for VRCompositor()->GetTrackingSpace()
    vr::VR_Init(&error, vr::VRApplication_Background);

    if (error != vr::VRInitError_None) {
        Warn("Failed to init OpenVR client! Error: %d", error);
        return;
    }
    isOpenvrInit = true;
#endif
}

void ShutdownOpenvrClient() {
    Debug("ShutdownOpenvrClient");

#ifndef __APPLE__
    std::unique_lock<std::mutex> lock(chaperone_mutex);

    if (!isOpenvrInit) {
        return;
    }

    isOpenvrInit = false;
    frameBoundarySync={};
    vr::VR_Shutdown();
#endif
}

bool IsOpenvrClientReady() { return isOpenvrInit; }

void _SetChaperoneArea(float areaWidth, float areaHeight) {
    Debug("SetChaperoneArea");

#ifndef __APPLE__
    std::unique_lock<std::mutex> lock(chaperone_mutex);

    const vr::HmdMatrix34_t MATRIX_IDENTITY
        = { { { 1.0, 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0, 0.0 } } };
    frameBoundarySync={};

    float perimeterPoints[4][2];

    perimeterPoints[0][0] = -1.0f * areaWidth;
    perimeterPoints[0][1] = -1.0f * areaHeight;
    perimeterPoints[1][0] = -1.0f * areaWidth;
    perimeterPoints[1][1] = 1.0f * areaHeight;
    perimeterPoints[2][0] = 1.0f * areaWidth;
    perimeterPoints[2][1] = 1.0f * areaHeight;
    perimeterPoints[3][0] = 1.0f * areaWidth;
    perimeterPoints[3][1] = -1.0f * areaHeight;

    auto setup = vr::VRChaperoneSetup();

    if (setup != nullptr) {
        vr::VRChaperoneSetup()->SetWorkingPerimeter(
            reinterpret_cast<vr::HmdVector2_t*>(perimeterPoints), 4
        );
        vr::VRChaperoneSetup()->SetWorkingStandingZeroPoseToRawTrackingPose(&MATRIX_IDENTITY);
        vr::VRChaperoneSetup()->SetWorkingSeatedZeroPoseToRawTrackingPose(&MATRIX_IDENTITY);
        vr::VRChaperoneSetup()->SetWorkingPlayAreaSize(areaWidth, areaHeight);
        vr::VRChaperoneSetup()->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
    }

    auto settings = vr::VRSettings();

    if (settings != nullptr) {
        // Hide SteamVR Chaperone
        vr::VRSettings()->SetFloat(
            vr::k_pch_CollisionBounds_Section, vr::k_pch_CollisionBounds_FadeDistance_Float, 0.0f
        );
    }

#endif
}

extern "C" void SetFrameBoundary(float width, float depth, const float* points, unsigned int count) {
#ifndef __APPLE__
    if (!points || count < 3 || count > 64 || !std::isfinite(width) || !std::isfinite(depth)
        || width <= 0 || depth <= 0 || width > 100 || depth > 100) return;
    for (unsigned i=0; i<count*12; ++i)
        if (!std::isfinite(points[i]) || std::abs(points[i]) > 200) return;
    std::unique_lock<std::mutex> lock(chaperone_mutex);
    // Retain the latest reliable update even before the background client is
    // ready. Client retransmission is unnecessary and can provoke recentering.
    frameBoundarySync.request(width,depth,points,count,boundaryTime());
#endif
}

extern "C" void PollFrameBoundary() {
#ifndef __APPLE__
    std::unique_lock<std::mutex> lock(chaperone_mutex);
    const auto now=boundaryTime();
    if (!isOpenvrInit || !frameBoundarySync.due(now)) return;
    auto* setup=vr::VRChaperoneSetup();
    if (!setup) return;
    if (frameBoundarySync.apply(*setup,now)) Info("Frame boundary synchronized");
    else Warn("Frame boundary commit failed; backing off");
    // Preserve SteamVR's visibility preferences; Frame owns its local guardian.
#endif
}

#ifdef __linux__
std::unique_ptr<vr::HmdMatrix34_t> GetInvZeroPose() {
    Debug("GetInvZeroPose");

    std::unique_lock<std::mutex> lock(chaperone_mutex);
    if (!isOpenvrInit) {
        return nullptr;
    }
    auto mat = std::make_unique<vr::HmdMatrix34_t>();
    // revert pulls live into working copy
    vr::VRChaperoneSetup()->RevertWorkingCopy();
    auto compositor = vr::VRCompositor();
    if (compositor == nullptr) {
        return nullptr;
    }
    if (compositor->GetTrackingSpace() == vr::TrackingUniverseStanding) {
        vr::VRChaperoneSetup()->GetWorkingStandingZeroPoseToRawTrackingPose(mat.get());
    } else {
        vr::VRChaperoneSetup()->GetWorkingSeatedZeroPoseToRawTrackingPose(mat.get());
    }
    return mat;
}
#endif
