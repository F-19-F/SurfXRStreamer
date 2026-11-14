#pragma once
#include "openvr_driver_wrap.h"

void GetTriggerBoneTransform(bool withController, bool isLeftHand, bool touch, float click, vr::VRBoneTransform_t outBoneTransform[]);
void GetTriggerClickBoneTransform(bool withController, bool isLeftHand, vr::VRBoneTransform_t outBoneTransform[]);
