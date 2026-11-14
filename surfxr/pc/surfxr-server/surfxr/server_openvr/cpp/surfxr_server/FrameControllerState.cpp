#include "Controller.h"
#include "Logger.h"

void Controller::ReleaseFrameInputs() {
    // UDP pose loss can arrive before the reliable button-release packet.
    // Clear inputs here while disconnecting so wake-up cannot revive held keys.
    auto* input = vr::VRDriverInput();
    for (const auto& [id, scalar] : m_buttonScalar) {
        const auto handle = m_buttonHandles.at(id);
        if (scalar) input->UpdateScalarComponent(handle, 0.f, 0.0);
        else boolean_input_.update(handle, false, 0.0);
    }
    m_frameThumbTouches.clear();m_currentThumbTouch=m_lastThumbTouch=false;
    m_currentTriggerTouch=m_lastTriggerTouch=false;m_triggerValue=m_gripValue=0;
    m_thumbTouchAnimationProgress=m_indexTouchAnimationProgress=0;
    m_frameSkeleton.update(0, nullptr);
}

// SteamVR 2.17 advances activity timers even after deviceIsConnected=false.
// Its monitor gives Standby priority over the disconnected pose. The driver's
// non-game proximity component holds that timer only while this virtual device
// is offline. No game button/pose or global power setting is synthesized.
void Controller::InitializeFrameOfflineGuard() {
    if (!m_frameController) return;
    const auto error = vr::VRDriverInput()->CreateBooleanComponent(
        prop_container, "/proximity", &m_frameOfflineGuard);
    // This controller has no physical proximity sensor: normal online motion
    // must continue to drive SteamVR's original idle/standby behavior.
    vr::VRProperties()->SetBoolProperty(prop_container, vr::Prop_ContainsProximitySensor_Bool, false);
    if (error != vr::VRInputError_None) Warn("Frame offline activity guard unavailable: %d", error);
    UpdateFrameOfflineGuard(true);
}

void Controller::UpdateFrameOfflineGuard(bool offline) {
    if (m_frameController && m_frameOfflineGuard != vr::k_ulInvalidInputComponentHandle)
        boolean_input_.update(m_frameOfflineGuard, offline, 0.0);
}

void Controller::SetFrameForwarding(bool enabled, uint64_t since) {
    m_frameForwarding = enabled;
    m_frameForwardingSince = since;
    if (!m_frameController || !is_active()) return;
    if (!enabled) {
        ReleaseFrameInputs();
        auto pose = last_pose;
        pose.deviceIsConnected = false;
        pose.poseIsValid = false;
        pose.result = vr::TrackingResult_Uninitialized;
        UpdateFrameOfflineGuard(true);
        submit_pose(pose);
    }
    Info("Frame controller forwarding: hand=%llu enabled=%d since=%llu", device_id, enabled, since);
}
