#include "PoseHistory.h"
#include "Logger.h"
#include "Utils.h"
#include "include/openvr_math.h"
#include <mutex>
#include <optional>

// A submitted frame was rendered from a recent pose, so matches older than this
// are stale no matter how well the rotation agrees.
static constexpr uint64_t MATCH_WINDOW_NS = 250000000;

void PoseHistory::OnPoseUpdated(uint64_t targetTimestampNs, FfiDeviceMotion motion) {
    // Put pose history buffer
    TrackingHistoryFrame history;
    history.targetTimestampNs = targetTimestampNs;
    history.motion = motion;

    HmdMatrix_QuatToMat(
        motion.pose.orientation.w,
        motion.pose.orientation.x,
        motion.pose.orientation.y,
        motion.pose.orientation.z,
        &history.poseMatrix
    );
    for (int i = 0; i < 3; ++i) history.poseMatrix.m[i][3] = motion.pose.position[i];

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_poseBuffer.empty() && m_poseBuffer.back().motion.poseEpoch != motion.poseEpoch)
        m_poseBuffer.clear();
    if (!m_transformIdentity) {
        auto transformed = vrmath::matMul33(m_transform, history.poseMatrix);
        for (int i = 0; i < 3; ++i) {
            transformed.m[i][3] = m_transform.m[i][3];
            for (int j = 0; j < 3; ++j)
                transformed.m[i][3] += m_transform.m[i][j] * history.poseMatrix.m[j][3];
        }
        history.poseMatrix = transformed;
    }

    if (m_poseBuffer.size() == 0) {
        m_poseBuffer.push_back(history);
    } else {
        if (m_poseBuffer.back().targetTimestampNs != targetTimestampNs) {
            // New track info
            m_poseBuffer.push_back(history);
        }
    }
    // The value should match with the client's MAXIMUM_TRACKING_FRAMES in ovr_context.cpp
    if (m_poseBuffer.size() > 120 * 3) {
        m_poseBuffer.pop_front();
    }
}

std::optional<PoseHistory::TrackingHistoryFrame>
PoseHistory::GetBestPoseMatch(const vr::HmdMatrix34_t& pose) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    return BestPoseMatchLocked(pose);
}

std::optional<PoseHistory::TrackingHistoryFrame>
PoseHistory::BestPoseMatchLocked(const vr::HmdMatrix34_t& pose) const {
    if (m_poseBuffer.empty()) {
        Debug("PoseHistory::GetBestPoseMatch: No pose matched.");
        return {};
    }

    // Search newest first within the bounded recent history. Rotation alone
    // aliases translations and can associate a new image with an older pose.
    const uint64_t newestTimestampNs = m_poseBuffer.back().targetTimestampNs;
    float minDiff = 100000;
    auto minIt = m_poseBuffer.rbegin();
    for (auto it = m_poseBuffer.rbegin(); it != m_poseBuffer.rend(); ++it) {
        if (newestTimestampNs - it->targetTimestampNs > MATCH_WINDOW_NS) {
            break;
        }
        float distance = 0;
        // mHmdPose is a complete head-to-world 3x4 transform for both eyes.
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++) {
                const auto delta = it->poseMatrix.m[j][i] - pose.m[j][i];
                distance += delta * delta;
            }
        }
        if (minDiff > distance) {
            minIt = it;
            minDiff = distance;
        }
    }
    return *minIt;
}

std::optional<PoseHistory::TrackingHistoryFrame> PoseHistory::GetPoseAt(uint64_t timestampNs
) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (auto it = m_poseBuffer.rbegin(), end = m_poseBuffer.rend(); it != end; ++it) {
        if (it->targetTimestampNs == timestampNs)
            return *it;
    }

    Debug("PoseHistory::GetPoseAt: No pose matched.");
    return {};
}

std::optional<PoseHistory::TrackingHistoryFrame> PoseHistory::GetLatestPose() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_poseBuffer.empty()) {
        return {};
    }
    return m_poseBuffer.back();
}

void PoseHistory::SetTransform(const vr::HmdMatrix34_t& transform) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_transform = transform;

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (transform.m[i][j] != (i == j ? 1 : 0)) {
                m_transformIdentity = false;
                return;
            }
        }
        if (transform.m[i][3] != 0) { m_transformIdentity = false; return; }
    }
    m_transformIdentity = true;
}

vr::HmdMatrix34_t PoseHistory::ToDriverPose(const vr::HmdMatrix34_t& worldPose) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    vr::HmdMatrix34_t result{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col)
            for (int k = 0; k < 3; ++k)
                result.m[row][col] += m_transform.m[k][row] * worldPose.m[k][col];
        for (int k = 0; k < 3; ++k)
            result.m[row][3] += m_transform.m[k][row] * (worldPose.m[k][3] - m_transform.m[k][3]);
    }
    return result;
}

void PoseHistory::Clear() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_poseBuffer.clear();
}

std::optional<PoseHistory::TrackingHistoryFrame>
PoseHistory::GetFramePose(const vr::HmdMatrix34_t& submitted) const {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_poseBuffer.empty() && m_poseBuffer.back().motion.poseEpoch) {
        // Identity comes from a real tracking sample, image pose comes from
        // SteamVR's prediction. Never quantize the image to nearest history.
        auto latest = m_poseBuffer.back();
        latest.poseMatrix = submitted;
        return latest;
    }
    return BestPoseMatchLocked(submitted);
}
