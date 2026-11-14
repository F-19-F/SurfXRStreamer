#include "Controller.h"
#include "Logger.h"
#include "Paths.h"
#include "FrameInput.h"
#include "Utils.h"
#include "bindings.h"
#include "include/openvr_math.h"
#include <algorithm>
#include <cstring>
#include <string_view>

Controller::Controller(uint64_t deviceID, vr::EVRSkeletalTrackingLevel skeletonLevel)
    : TrackedDevice(
          deviceID,
          Settings_Instance()->m_controllerIsTracker ? vr::TrackedDeviceClass_GenericTracker
                                                     : vr::TrackedDeviceClass_Controller
      )
    , m_skeletonLevel(skeletonLevel) {
    Debug("Controller::constructor deviceID=%llu", deviceID);
}

bool Controller::activate() {
    Debug("Controller::Activate deviceID=%llu", this->device_id);

    auto vr_driver_input = vr::VRDriverInput();

    SetOpenvrProps((void*)this, this->device_id);

    m_frameController = vr::VRProperties()->GetStringProperty(this->prop_container, vr::Prop_ControllerType_String) == "frame_controller";
    if (m_frameController) m_skeletonLevel = vr::VRSkeletalTracking_Partial;
    RegisterButtons((void*)this, this->device_id);

    vr_driver_input->CreateHapticComponent(this->prop_container, "/output/haptic", &m_compHaptic);

    vr_driver_input->CreateScalarComponent(
        this->prop_container,
        "/input/finger/index",
        &m_buttonHandles[SURFXR_INPUT_FINGER_INDEX],
        vr::VRScalarType_Absolute,
        vr::VRScalarUnits_NormalizedOneSided
    );
    vr_driver_input->CreateScalarComponent(
        this->prop_container,
        "/input/finger/middle",
        &m_buttonHandles[SURFXR_INPUT_FINGER_MIDDLE],
        vr::VRScalarType_Absolute,
        vr::VRScalarUnits_NormalizedOneSided
    );
    vr_driver_input->CreateScalarComponent(
        this->prop_container,
        "/input/finger/ring",
        &m_buttonHandles[SURFXR_INPUT_FINGER_RING],
        vr::VRScalarType_Absolute,
        vr::VRScalarUnits_NormalizedOneSided
    );
    vr_driver_input->CreateScalarComponent(
        this->prop_container,
        "/input/finger/pinky",
        &m_buttonHandles[SURFXR_INPUT_FINGER_PINKY],
        vr::VRScalarType_Absolute,
        vr::VRScalarUnits_NormalizedOneSided
    );

    if (this->device_id == HAND_LEFT_ID || this->device_id == HAND_TRACKER_LEFT_ID) {
        vr_driver_input->CreateSkeletonComponent(
            this->prop_container,
            "/input/skeleton/left",
            "/skeleton/hand/left",
            "/pose/raw",
            m_skeletonLevel,
            nullptr,
            0U,
            &m_compSkeleton
        );
    } else {
        vr_driver_input->CreateSkeletonComponent(
            this->prop_container,
            "/input/skeleton/right",
            "/skeleton/hand/right",
            "/pose/raw",
            m_skeletonLevel,
            nullptr,
            0U,
            &m_compSkeleton
        );
    }

    // NB: here we set some initial values for the hand skeleton to fix the frozen hand bug
    {
        vr::VRBoneTransform_t boneTransforms[SKELETON_BONE_COUNT];
        GetBoneTransform(false, boneTransforms);

        vr_driver_input->UpdateSkeletonComponent(
            m_compSkeleton,
            vr::VRSkeletalMotionRange_WithController,
            boneTransforms,
            SKELETON_BONE_COUNT
        );
        vr_driver_input->UpdateSkeletonComponent(
            m_compSkeleton,
            vr::VRSkeletalMotionRange_WithoutController,
            boneTransforms,
            SKELETON_BONE_COUNT
        );
    }

    InitializeFrameOfflineGuard();
    return true;
}

vr::VRInputComponentHandle_t Controller::getHapticComponent() { return m_compHaptic; }

void Controller::RegisterButton(uint64_t id) {
    Debug("Controller::RegisterButton deviceID=%llu", this->device_id);

    ButtonInfo buttonInfo;
    if (device_id == HAND_LEFT_ID) {
        buttonInfo = LEFT_CONTROLLER_BUTTON_MAPPING[id];
    } else {
        buttonInfo = RIGHT_CONTROLLER_BUTTON_MAPPING[id];
    }

    if (m_frameController) buttonInfo = FrameInput(id, device_id == HAND_LEFT_ID);
    if (buttonInfo.steamvr_paths.empty()) return;
    for (auto path : buttonInfo.steamvr_paths)
        m_buttonScalar[PathStringToHash(path)] = buttonInfo.type != ButtonType::Binary;
    if (buttonInfo.type == ButtonType::Binary) {
        for (auto path : buttonInfo.steamvr_paths) {
            vr::VRDriverInput()->CreateBooleanComponent(
                this->prop_container, path, &m_buttonHandles[PathStringToHash(path)]
            );
        }
    } else {
        auto scalarType = buttonInfo.type == ButtonType::ScalarOneSided
            ? vr::VRScalarUnits_NormalizedOneSided
            : vr::VRScalarUnits_NormalizedTwoSided;

        for (auto path : buttonInfo.steamvr_paths) {
            vr::VRDriverInput()->CreateScalarComponent(
                this->prop_container,
                path,
                &m_buttonHandles[PathStringToHash(path)],
                vr::VRScalarType_Absolute,
                scalarType
            );
        }
    }
}

void Controller::SetButton(uint64_t id, FfiButtonValue value) {
    Debug("Controller::SetButton deviceID=%llu buttonID=%llu", this->device_id, id);

    if (!is_active() || !this->last_pose.poseIsValid) {
        return;
    }

    std::vector<uint64_t> paths;
    if (m_frameController) {
        for (const auto* path : FrameInput(id, device_id == HAND_LEFT_ID).steamvr_paths) paths.push_back(PathStringToHash(path));
    } else paths = SURFXR_TO_STEAMVR_PATH_IDS[id];
    for (auto id : paths) {
        if (value.type == BUTTON_TYPE_BINARY) {
            boolean_input_.update(
                m_buttonHandles[id], (bool)value.binary, 0.0
            );
        } else {
            vr::VRDriverInput()->UpdateScalarComponent(m_buttonHandles[id], value.scalar, 0.0);
        }
    }

    // todo: remove when moving inferred controller hand skeleton to rust
    if (m_frameController) {
        for (const auto* path : FrameInput(id, device_id == HAND_LEFT_ID).steamvr_paths) {
            const std::string component(path);
            if (component.find("/touch") != std::string::npos && component.find("/trigger/") == std::string::npos &&
                component.find("/grip/") == std::string::npos && component.find("/bumper/") == std::string::npos) {
                m_frameThumbTouches[id] = value.binary;
                m_currentThumbTouch = std::any_of(m_frameThumbTouches.begin(),m_frameThumbTouches.end(),[](const auto& entry){return entry.second;});
                return;
            }
        }
    }
    if (id == LEFT_A_TOUCH_ID || id == LEFT_B_TOUCH_ID || id == LEFT_X_TOUCH_ID
        || id == LEFT_Y_TOUCH_ID || id == LEFT_TRACKPAD_TOUCH_ID || id == LEFT_THUMBSTICK_TOUCH_ID
        || id == LEFT_THUMBREST_TOUCH_ID || id == RIGHT_A_TOUCH_ID || id == RIGHT_B_TOUCH_ID
        || id == RIGHT_TRACKPAD_TOUCH_ID || id == RIGHT_THUMBSTICK_TOUCH_ID
        || id == RIGHT_THUMBREST_TOUCH_ID) {
        m_currentThumbTouch = value.binary;
    } else if (id == LEFT_TRIGGER_TOUCH_ID || id == RIGHT_TRIGGER_TOUCH_ID) {
        m_currentTriggerTouch = value.binary;
    } else if (id == LEFT_TRIGGER_VALUE_ID || id == RIGHT_TRIGGER_VALUE_ID) {
        m_triggerValue = value.scalar;
    } else if (id == LEFT_SQUEEZE_VALUE_ID || id == RIGHT_SQUEEZE_VALUE_ID) {
        m_gripValue = value.scalar;
    }
}

bool Controller::OnPoseUpdate(uint64_t targetTimestampNs, float predictionS, FfiHandData handData) {
    if (!is_active() || this->object_id == vr::k_unTrackedDeviceIndexInvalid) {
        return false;
    }

    if (m_frameController && !m_frameForwarding) {
        handData.controllerMotion = nullptr;
        handData.handSkeleton = nullptr;
    }
    auto controllerMotion = handData.controllerMotion;
    auto handSkeleton = handData.handSkeleton;

    bool enabledAsHandTracker = handData.isHandTracker
        && (device_id == HAND_TRACKER_LEFT_ID || device_id == HAND_TRACKER_RIGHT_ID);
    bool enabledAsController
        = !handData.isHandTracker && (device_id == HAND_LEFT_ID || device_id == HAND_RIGHT_ID);
    bool enabled = (controllerMotion != nullptr || handSkeleton != nullptr)
        && (enabledAsHandTracker || enabledAsController);

    Debug(
        "%s %s: enabled: %d, ctrl: %d, hand: %d",
        (device_id == HAND_TRACKER_LEFT_ID || device_id == HAND_TRACKER_RIGHT_ID) ? "hand tracker"
                                                                                  : "controller",
        (device_id == HAND_TRACKER_LEFT_ID || device_id == HAND_LEFT_ID) ? "left" : "right",
        enabled,
        handData.controllerMotion != nullptr,
        handData.handSkeleton != nullptr
    );

    auto vr_driver_input = vr::VRDriverInput();

    auto pose = vr::DriverPose_t {};
    if (m_frameController && !enabled && this->last_pose.deviceIsConnected) ReleaseFrameInputs();
    UpdateFrameOfflineGuard(!enabled);
    pose.poseIsValid = enabled;
    pose.deviceIsConnected = enabled;
    pose.result = enabled ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;

    pose.qDriverFromHeadRotation = HmdQuaternion_Init(1, 0, 0, 0);
    pose.qWorldFromDriverRotation = HmdQuaternion_Init(1, 0, 0, 0);

    if (controllerMotion != nullptr) {
        auto m = controllerMotion;

        pose.qRotation = HmdQuaternion_Init(
            m->pose.orientation.w,
            m->pose.orientation.x,
            m->pose.orientation.y,
            m->pose.orientation.z
        );

        pose.vecPosition[0] = m->pose.position[0];
        pose.vecPosition[1] = m->pose.position[1];
        pose.vecPosition[2] = m->pose.position[2];

        pose.vecVelocity[0] = m->linearVelocity[0];
        pose.vecVelocity[1] = m->linearVelocity[1];
        pose.vecVelocity[2] = m->linearVelocity[2];

        pose.vecAngularVelocity[0] = m->angularVelocity[0];
        pose.vecAngularVelocity[1] = m->angularVelocity[1];
        pose.vecAngularVelocity[2] = m->angularVelocity[2];
    } else if (handSkeleton != nullptr) {
        auto r = handSkeleton->jointRotations[0];
        pose.qRotation = HmdQuaternion_Init(r.w, r.x, r.y, r.z);

        auto p = handSkeleton->jointPositions[0];
        pose.vecPosition[0] = p[0];
        pose.vecPosition[1] = p[1];
        pose.vecPosition[2] = p[2];

        // If possible, use the last stored m_pose and timestamp
        // to calculate the velocities of the current pose.
        double linearVelocity[3] = { 0.0, 0.0, 0.0 };
        vr::HmdVector3d_t angularVelocity = { 0.0, 0.0, 0.0 };

        if (handData.predictHandSkeleton && this->last_pose.poseIsValid) {
            double dt = ((double)targetTimestampNs - (double)m_poseTargetTimestampNs) / NS_PER_S;

            if (dt > 0.0) {
                linearVelocity[0] = (pose.vecPosition[0] - this->last_pose.vecPosition[0]) / dt;
                linearVelocity[1] = (pose.vecPosition[1] - this->last_pose.vecPosition[1]) / dt;
                linearVelocity[2] = (pose.vecPosition[2] - this->last_pose.vecPosition[2]) / dt;
                angularVelocity
                    = AngularVelocityBetweenQuats(this->last_pose.qRotation, pose.qRotation, dt);
            }
        }

        pose.vecVelocity[0] = linearVelocity[0];
        pose.vecVelocity[1] = linearVelocity[1];
        pose.vecVelocity[2] = linearVelocity[2];

        pose.vecAngularVelocity[0] = angularVelocity.v[0];
        pose.vecAngularVelocity[1] = angularVelocity.v[1];
        pose.vecAngularVelocity[2] = angularVelocity.v[2];
    }

    pose.poseTimeOffset = predictionS;

    this->submit_pose(pose);

    m_poseTargetTimestampNs = targetTimestampNs;

    // Early return to skip updating the skeleton
    if (!enabled) {
        return false;
    } else if (handSkeleton != nullptr) {
        vr::VRBoneTransform_t boneTransform[SKELETON_BONE_COUNT] = {};

        boneTransform[0].orientation.w = 1.0;
        boneTransform[0].orientation.x = 0.0;
        boneTransform[0].orientation.y = 0.0;
        boneTransform[0].orientation.z = 0.0;
        boneTransform[0].position.v[0] = 0.0;
        boneTransform[0].position.v[1] = 0.0;
        boneTransform[0].position.v[2] = 0.0;
        boneTransform[0].position.v[3] = 1.0;

        // NB: start from index 1 to skip the root bone
        for (int j = 1; j < 31; j++) {
            boneTransform[j].orientation.w = handSkeleton->jointRotations[j].w;
            boneTransform[j].orientation.x = handSkeleton->jointRotations[j].x;
            boneTransform[j].orientation.y = handSkeleton->jointRotations[j].y;
            boneTransform[j].orientation.z = handSkeleton->jointRotations[j].z;
            boneTransform[j].position.v[0] = handSkeleton->jointPositions[j][0];
            boneTransform[j].position.v[1] = handSkeleton->jointPositions[j][1];
            boneTransform[j].position.v[2] = handSkeleton->jointPositions[j][2];
            boneTransform[j].position.v[3] = 1.0;
        }

        vr_driver_input->UpdateSkeletonComponent(
            m_compSkeleton,
            vr::VRSkeletalMotionRange_WithController,
            boneTransform,
            SKELETON_BONE_COUNT
        );
        vr_driver_input->UpdateSkeletonComponent(
            m_compSkeleton,
            vr::VRSkeletalMotionRange_WithoutController,
            boneTransform,
            SKELETON_BONE_COUNT
        );

        float rotThumb = (handSkeleton->jointRotations[2].z + handSkeleton->jointRotations[2].y
                          + handSkeleton->jointRotations[3].z + handSkeleton->jointRotations[3].y
                          + handSkeleton->jointRotations[4].z + handSkeleton->jointRotations[4].y)
            * 0.67f;
        float rotIndex = (handSkeleton->jointRotations[7].z + handSkeleton->jointRotations[8].z
                          + handSkeleton->jointRotations[9].z)
            * 0.67f;
        float rotMiddle = (handSkeleton->jointRotations[12].z + handSkeleton->jointRotations[13].z
                           + handSkeleton->jointRotations[14].z)
            * 0.67f;
        float rotRing = (handSkeleton->jointRotations[17].z + handSkeleton->jointRotations[18].z
                         + handSkeleton->jointRotations[19].z)
            * 0.67f;
        float rotPinky = (handSkeleton->jointRotations[22].z + handSkeleton->jointRotations[23].z
                          + handSkeleton->jointRotations[24].z)
            * 0.67f;

        vr_driver_input->UpdateScalarComponent(
            m_buttonHandles[SURFXR_INPUT_FINGER_INDEX], rotIndex, 0.0
        );
        vr_driver_input->UpdateScalarComponent(
            m_buttonHandles[SURFXR_INPUT_FINGER_MIDDLE], rotMiddle, 0.0
        );
        vr_driver_input->UpdateScalarComponent(
            m_buttonHandles[SURFXR_INPUT_FINGER_RING], rotRing, 0.0
        );
        vr_driver_input->UpdateScalarComponent(
            m_buttonHandles[SURFXR_INPUT_FINGER_PINKY], rotPinky, 0.0
        );
    } else if (m_frameController && controllerMotion && m_frameSkeleton.apply(vr_driver_input,m_compSkeleton)) {
        // Frame supplies the measured parent-relative bones; pose stays on its existing controller path.
    } else if (controllerMotion != nullptr) {
        if (m_lastThumbTouch != m_currentThumbTouch) {
            m_thumbTouchAnimationProgress += 1.f / ANIMATION_FRAME_COUNT;
            if (m_thumbTouchAnimationProgress > 1.f) {
                m_thumbTouchAnimationProgress = 0;
                m_lastThumbTouch = m_currentThumbTouch;
            }
        } else {
            m_thumbTouchAnimationProgress = 0;
        }

        if (m_lastTriggerTouch != m_currentTriggerTouch) {
            m_indexTouchAnimationProgress += 1.f / ANIMATION_FRAME_COUNT;
            if (m_indexTouchAnimationProgress > 1.f) {
                m_indexTouchAnimationProgress = 0;
                m_lastTriggerTouch = m_currentTriggerTouch;
            }
        } else {
            m_indexTouchAnimationProgress = 0;
        }

        float indexCurl = 0.0;
        if (m_triggerValue > 0.0) {
            indexCurl = 0.5 + m_triggerValue * 0.5;
        } else if (m_lastTriggerTouch == 0) {
            indexCurl = m_indexTouchAnimationProgress * 0.5;
        } else {
            indexCurl = 0.5 - m_indexTouchAnimationProgress * 0.5;
        }
        vr_driver_input->UpdateScalarComponent(
            m_buttonHandles[SURFXR_INPUT_FINGER_INDEX], indexCurl, 0.0
        );

        vr_driver_input->UpdateScalarComponent(
            m_buttonHandles[SURFXR_INPUT_FINGER_MIDDLE], m_gripValue, 0.0
        );

        // Ring and pinky fingers are not tracked. Infer a more natural pose.
        if (m_currentThumbTouch) {
            vr_driver_input->UpdateScalarComponent(m_buttonHandles[SURFXR_INPUT_FINGER_RING], 1, 0.0);
            vr_driver_input->UpdateScalarComponent(
                m_buttonHandles[SURFXR_INPUT_FINGER_PINKY], 1, 0.0
            );
        } else {
            vr_driver_input->UpdateScalarComponent(
                m_buttonHandles[SURFXR_INPUT_FINGER_RING], m_gripValue, 0.0
            );
            vr_driver_input->UpdateScalarComponent(
                m_buttonHandles[SURFXR_INPUT_FINGER_PINKY], m_gripValue, 0.0
            );
        }

        vr::VRBoneTransform_t boneTransforms[SKELETON_BONE_COUNT];

        // Perform whatever logic is necessary to convert your device's input into a
        // skeletal pose, first to create a pose "With Controller", that is as close to the
        // pose of the user's real hand as possible
        GetBoneTransform(true, boneTransforms);

        // Then update the WithController pose on the component with those transforms
        vr::EVRInputError err = vr_driver_input->UpdateSkeletonComponent(
            m_compSkeleton,
            vr::VRSkeletalMotionRange_WithController,
            boneTransforms,
            SKELETON_BONE_COUNT
        );
        if (err != vr::VRInputError_None) {
            // Handle failure case
            Error("UpdateSkeletonComponentfailed.  Error: %i\n", err);
        }

        GetBoneTransform(false, boneTransforms);

        // Then update the WithoutController pose on the component
        err = vr_driver_input->UpdateSkeletonComponent(
            m_compSkeleton,
            vr::VRSkeletalMotionRange_WithoutController,
            boneTransforms,
            SKELETON_BONE_COUNT
        );
        if (err != vr::VRInputError_None) {
            // Handle failure case
            Error("UpdateSkeletonComponentfailed.  Error: %i\n", err);
        }
    }

    return false;
}
