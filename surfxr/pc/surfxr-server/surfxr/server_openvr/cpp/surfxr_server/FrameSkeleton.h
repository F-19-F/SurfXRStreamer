#pragma once
#include "bindings.h"
#include "openvr_driver_wrap.h"
#include <array>
#include <chrono>
#include <cmath>

class FrameSkeleton {
    using Clock = std::chrono::steady_clock;
    std::array<vr::VRBoneTransform_t,31> bones{};
    Clock::time_point received{};
    uint64_t sampled = 0;
public:
    void update(uint64_t timestamp, const FfiHandSkeleton* value) {
        if (!value || !timestamp) { sampled=0; received={}; return; }
        if (timestamp < sampled) return;
        std::array<vr::VRBoneTransform_t,31> next{};
        for (unsigned i=0;i<31;++i) {
            const auto q=value->jointRotations[i]; const auto* p=value->jointPositions[i];
            const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
            if (!std::isfinite(norm)||norm<.5f||norm>1.5f) return;
            for (unsigned j=0;j<3;++j) {if (!std::isfinite(p[j])||std::abs(p[j])>2) return;next[i].position.v[j]=p[j];}
            next[i].position.v[3]=1;next[i].orientation={q.w,q.x,q.y,q.z};
        }
        bones=next;sampled=timestamp;received=Clock::now();
    }
    bool apply(vr::IVRDriverInput* input, vr::VRInputComponentHandle_t handle) const {
        if (!input || !handle || !sampled || Clock::now()-received>std::chrono::milliseconds(250)) return false;
        input->UpdateSkeletonComponent(handle,vr::VRSkeletalMotionRange_WithController,bones.data(),bones.size());
        input->UpdateSkeletonComponent(handle,vr::VRSkeletalMotionRange_WithoutController,bones.data(),bones.size());
        return true;
    }
};
