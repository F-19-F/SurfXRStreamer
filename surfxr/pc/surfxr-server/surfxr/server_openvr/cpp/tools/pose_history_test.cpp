// Extend the existing WARP fixture with actual production pose matching.
#include "surfxr_server/PoseHistory.h"
#include <stdexcept>
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("pose_history_association"); }
FfiDeviceMotion motion(float x) {
    FfiDeviceMotion value{}; value.pose.orientation.w = 1;
    value.pose.position[0] = x; return value;
}
vr::HmdMatrix34_t matrix(float x) {
    return {{{1,0,0,x},{0,1,0,0},{0,0,1,0}}};
}
}
void check_pose_history() {
    PoseHistory history;
    require(!history.GetBestPoseMatch(matrix(0)));
    history.OnPoseUpdated(1'000'000'000, motion(0));
    history.OnPoseUpdated(1'006'944'444, motion(1));
    // Identical orientations with distinct translations must remain exact.
    require(history.GetBestPoseMatch(matrix(0))->targetTimestampNs == 1'000'000'000);
    require(history.GetBestPoseMatch(matrix(1))->targetTimestampNs == 1'006'944'444);
    history.OnPoseUpdated(1'013'888'888, motion(1));
    require(history.GetBestPoseMatch(matrix(1))->targetTimestampNs == 1'013'888'888);
    require(history.GetPoseAt(1'000'000'000)->motion.pose.position[0] == 0);
    history.OnPoseUpdated(2'000'000'000, motion(2));
    require(history.GetBestPoseMatch(matrix(0))->targetTimestampNs == 2'000'000'000);
    // Translation-only universe transforms cannot be classified as identity.
    history.SetTransform(matrix(10));
    history.OnPoseUpdated(3'000'000'000, motion(3));
    require(history.GetBestPoseMatch(matrix(13))->targetTimestampNs == 3'000'000'000);
    require(history.GetLatestPose()->poseMatrix.m[0][3] == 13);
    const vr::HmdMatrix34_t rotated{{{0,-1,0,10},{1,0,0,20},{0,0,1,30}}};
    history.SetTransform(rotated);
    history.OnPoseUpdated(4'000'000'000, motion(3));
    auto expected = rotated; expected.m[1][3] = 23;
    require(history.GetBestPoseMatch(expected)->targetTimestampNs == 4'000'000'000);
    require(history.GetLatestPose()->poseMatrix.m[1][3] == 23);
    const auto driver = history.ToDriverPose(expected);
    require(driver.m[0][3] == 3 && driver.m[1][3] == 0 && driver.m[2][3] == 0);
    require(driver.m[0][0] == 1 && driver.m[1][1] == 1 && driver.m[2][2] == 1);
    auto timed = motion(4); timed.poseEpoch = 1;
    history.OnPoseUpdated(5'000'000'000, timed);
    require(!history.GetPoseAt(4'000'000'000));
    const auto actual = history.GetFramePose(matrix(9));
    require(actual->targetTimestampNs == 5'000'000'000 && actual->poseMatrix.m[0][3] == 9);
    history.Clear(); require(!history.GetLatestPose());
}
