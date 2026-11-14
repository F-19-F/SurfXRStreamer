#include "Controller.h"
#include "ControllerBones.h"
#include "Paths.h"
#include "Utils.h"
#include "include/openvr_math.h"

void GetThumbBoneTransform(
    bool withController, bool isLeftHand, bool touch, vr::VRBoneTransform_t outBoneTransform[]
) {
    if (isLeftHand) {
        if (touch) {
            if (withController) {
                outBoneTransform[2] = { { -0.017303f, 0.032567f, 0.025281f, 1.f },
                                        { 0.317609f, 0.528344f, 0.213134f, 0.757991f } };
                outBoneTransform[3] = { { 0.040406f, 0.000000f, -0.000000f, 1.f },
                                        { 0.991742f, 0.085317f, 0.019416f, 0.093765f } };
                outBoneTransform[4] = { { 0.032517f, -0.000000f, 0.000000f, 1.f },
                                        { 0.959385f, -0.012202f, -0.031055f, 0.280120f } };
            } else {
                outBoneTransform[2] = { { -0.016426f, 0.030866f, 0.025118f, 1.f },
                                        { 0.403850f, 0.595704f, 0.082451f, 0.689380f } };
                outBoneTransform[3] = { { 0.040406f, 0.000000f, -0.000000f, 1.f },
                                        { 0.989655f, -0.090426f, 0.028457f, 0.107691f } };
                outBoneTransform[4] = { { 0.032517f, 0.000000f, 0.000000f, 1.f },
                                        { 0.988590f, 0.143978f, 0.041520f, 0.015363f } };
            }
        } else {
            outBoneTransform[2] = { { -0.012083f, 0.028070f, 0.025050f, 1 },
                                    { 0.464112f, 0.567418f, 0.272106f, 0.623374f } };
            outBoneTransform[3] = { { 0.040406f, 0.000000f, -0.000000f, 1 },
                                    { 0.994838f, 0.082939f, 0.019454f, 0.055130f } };
            outBoneTransform[4] = { { 0.032517f, 0.000000f, 0.000000f, 1 },
                                    { 0.974793f, -0.003213f, 0.021867f, -0.222015f } };
        }

        outBoneTransform[5] = { { 0.030464f, -0.000000f, -0.000000f, 1 },
                                { 1.000000f, -0.000000f, 0.000000f, 0.000000f } };
    } else {
        if (touch) {
            if (withController) {
                outBoneTransform[2] = { { 0.017303f, 0.032567f, 0.025281f, 1 },
                                        { 0.528344f, -0.317609f, 0.757991f, -0.213134f } };
                outBoneTransform[3] = { { -0.040406f, -0.000000f, 0.000000f, 1 },
                                        { 0.991742f, 0.085317f, 0.019416f, 0.093765f } };
                outBoneTransform[4] = { { -0.032517f, 0.000000f, -0.000000f, 1 },
                                        { 0.959385f, -0.012202f, -0.031055f, 0.280120f } };
            } else {
                outBoneTransform[2] = { { 0.016426f, 0.030866f, 0.025118f, 1 },
                                        { 0.595704f, -0.403850f, 0.689380f, -0.082451f } };
                outBoneTransform[3] = { { -0.040406f, -0.000000f, 0.000000f, 1 },
                                        { 0.989655f, -0.090426f, 0.028457f, 0.107691f } };
                outBoneTransform[4] = { { -0.032517f, -0.000000f, -0.000000f, 1 },
                                        { 0.988590f, 0.143978f, 0.041520f, 0.015363f } };
            }
        } else {
            outBoneTransform[2] = { { 0.012330f, 0.028661f, 0.025049f, 1 },
                                    { 0.571059f, -0.451277f, 0.630056f, -0.270685f } };
            outBoneTransform[3] = { { -0.040406f, -0.000000f, 0.000000f, 1 },
                                    { 0.994565f, 0.078280f, 0.018282f, 0.066177f } };
            outBoneTransform[4] = { { -0.032517f, -0.000000f, -0.000000f, 1 },
                                    { 0.977658f, -0.003039f, 0.020722f, -0.209156f } };
        }

        outBoneTransform[5] = { { -0.030464f, 0.000000f, 0.000000f, 1 },
                                { 1.000000f, -0.000000f, 0.000000f, 0.000000f } };
    }
}

void GetGripClickBoneTransform(
    bool withController, bool isLeftHand, vr::VRBoneTransform_t outBoneTransform[]
) {
    if (withController) {
        if (isLeftHand) {
            outBoneTransform[11] = { { 0.002177f, 0.007120f, 0.016319f, 1 },
                                     { 0.529359f, 0.540512f, -0.463783f, 0.461011f } };
            outBoneTransform[12] = { { 0.070953f, 0.000779f, 0.000997f, 1 },
                                     { -0.831727f, 0.270927f, 0.175647f, -0.451638f } };
            outBoneTransform[13] = { { 0.043108f, 0.000000f, 0.000000f, 1 },
                                     { -0.854886f, -0.008231f, -0.028107f, -0.517990f } };
            outBoneTransform[14] = { { 0.033266f, -0.000000f, 0.000000f, 1 },
                                     { -0.825759f, 0.085208f, 0.086456f, -0.550805f } };
            outBoneTransform[15] = { { 0.025892f, -0.000000f, 0.000000f, 1 },
                                     { 0.999195f, -0.000000f, 0.000000f, 0.040126f } };
            outBoneTransform[16] = { { 0.000513f, -0.006545f, 0.016348f, 1 },
                                     { 0.500244f, 0.530784f, -0.516215f, 0.448939f } };
            outBoneTransform[17] = { { 0.065876f, 0.001786f, 0.000693f, 1 },
                                     { 0.831617f, -0.242931f, -0.139695f, 0.479461f } };
            outBoneTransform[18] = { { 0.040697f, 0.000000f, 0.000000f, 1 },
                                     { 0.769163f, -0.001746f, 0.001363f, 0.639049f } };
            outBoneTransform[19] = { { 0.028747f, -0.000000f, -0.000000f, 1 },
                                     { 0.968615f, -0.064537f, -0.046586f, 0.235477f } };
            outBoneTransform[20] = { { 0.022430f, -0.000000f, 0.000000f, 1 },
                                     { 1.000000f, 0.000000f, -0.000000f, -0.000000f } };
            outBoneTransform[21] = { { -0.002478f, -0.018981f, 0.015214f, 1 },
                                     { 0.474671f, 0.434670f, -0.653212f, 0.398827f } };
            outBoneTransform[22] = { { 0.062878f, 0.002844f, 0.000332f, 1 },
                                     { 0.798788f, -0.199577f, -0.094418f, 0.559636f } };
            outBoneTransform[23] = { { 0.030220f, 0.000002f, -0.000000f, 1 },
                                     { 0.853087f, 0.001644f, -0.000913f, 0.521765f } };
            outBoneTransform[24] = { { 0.018187f, -0.000002f, 0.000000f, 1 },
                                     { 0.974249f, 0.052491f, 0.003591f, 0.219249f } };
            outBoneTransform[25] = { { 0.018018f, 0.000000f, -0.000000f, 1 },
                                     { 1.000000f, 0.000000f, 0.000000f, 0.000000f } };

            outBoneTransform[28] = { { 0.016642f, -0.029992f, 0.083200f, 1 },
                                     { -0.094577f, 0.694550f, 0.702845f, 0.121100f } };
            outBoneTransform[29] = { { 0.011144f, -0.028727f, 0.108366f, 1 },
                                     { -0.076328f, 0.788280f, 0.605097f, 0.081527f } };
            outBoneTransform[30] = { { 0.011333f, -0.026044f, 0.128585f, 1 },
                                     { -0.144791f, 0.737451f, 0.656958f, -0.060069f } };
        } else {
            outBoneTransform[11] = { { 0.002177f, 0.007120f, 0.016319f, 1 },
                                     { 0.529359f, 0.540512f, -0.463783f, 0.461011f } };
            outBoneTransform[12] = { { 0.070953f, 0.000779f, 0.000997f, 1 },
                                     { -0.831727f, 0.270927f, 0.175647f, -0.451638f } };
            outBoneTransform[13] = { { 0.043108f, 0.000000f, 0.000000f, 1 },
                                     { -0.854886f, -0.008231f, -0.028107f, -0.517990f } };
            outBoneTransform[14] = { { 0.033266f, -0.000000f, 0.000000f, 1 },
                                     { -0.825759f, 0.085208f, 0.086456f, -0.550805f } };
            outBoneTransform[15] = { { 0.025892f, -0.000000f, 0.000000f, 1 },
                                     { 0.999195f, -0.000000f, 0.000000f, 0.040126f } };
            outBoneTransform[16] = { { 0.000513f, -0.006545f, 0.016348f, 1 },
                                     { 0.500244f, 0.530784f, -0.516215f, 0.448939f } };
            outBoneTransform[17] = { { 0.065876f, 0.001786f, 0.000693f, 1 },
                                     { 0.831617f, -0.242931f, -0.139695f, 0.479461f } };
            outBoneTransform[18] = { { 0.040697f, 0.000000f, 0.000000f, 1 },
                                     { 0.769163f, -0.001746f, 0.001363f, 0.639049f } };
            outBoneTransform[19] = { { 0.028747f, -0.000000f, -0.000000f, 1 },
                                     { 0.968615f, -0.064537f, -0.046586f, 0.235477f } };
            outBoneTransform[20] = { { 0.022430f, -0.000000f, 0.000000f, 1 },
                                     { 1.000000f, 0.000000f, -0.000000f, -0.000000f } };
            outBoneTransform[21] = { { -0.002478f, -0.018981f, 0.015214f, 1 },
                                     { 0.474671f, 0.434670f, -0.653212f, 0.398827f } };
            outBoneTransform[22] = { { 0.062878f, 0.002844f, 0.000332f, 1 },
                                     { 0.798788f, -0.199577f, -0.094418f, 0.559636f } };
            outBoneTransform[23] = { { 0.030220f, 0.000002f, -0.000000f, 1 },
                                     { 0.853087f, 0.001644f, -0.000913f, 0.521765f } };
            outBoneTransform[24] = { { 0.018187f, -0.000002f, 0.000000f, 1 },
                                     { 0.974249f, 0.052491f, 0.003591f, 0.219249f } };
            outBoneTransform[25] = { { 0.018018f, 0.000000f, -0.000000f, 1 },
                                     { 1.000000f, 0.000000f, 0.000000f, 0.000000f } };

            outBoneTransform[28] = { { 0.016642f, -0.029992f, 0.083200f, 1 },
                                     { -0.094577f, 0.694550f, 0.702845f, 0.121100f } };
            outBoneTransform[29] = { { 0.011144f, -0.028727f, 0.108366f, 1 },
                                     { -0.076328f, 0.788280f, 0.605097f, 0.081527f } };
            outBoneTransform[30] = { { 0.011333f, -0.026044f, 0.128585f, 1 },
                                     { -0.144791f, 0.737451f, 0.656958f, -0.060069f } };
        }

    } else {
        if (isLeftHand) {
            outBoneTransform[11] = { { 0.005787f, 0.006806f, 0.016534f, 1 },
                                     { 0.514203f, 0.522315f, -0.478348f, 0.483700f } };
            outBoneTransform[12] = { { 0.070953f, 0.000779f, 0.000997f, 1 },
                                     { 0.723653f, -0.097901f, 0.048546f, 0.681458f } };
            outBoneTransform[13] = { { 0.043108f, 0.000000f, 0.000000f, 1 },
                                     { 0.637464f, -0.002366f, -0.002831f, 0.770472f } };
            outBoneTransform[14] = { { 0.033266f, 0.000000f, 0.000000f, 1 },
                                     { 0.658008f, 0.002610f, 0.003196f, 0.753000f } };
            outBoneTransform[15] = { { 0.025892f, -0.000000f, 0.000000f, 1 },
                                     { 0.999195f, 0.000000f, 0.000000f, 0.040126f } };
            outBoneTransform[16] = { { 0.004123f, -0.006858f, 0.016563f, 1 },
                                     { 0.489609f, 0.523374f, -0.520644f, 0.463997f } };
            outBoneTransform[17] = { { 0.065876f, 0.001786f, 0.000693f, 1 },
                                     { 0.759970f, -0.055609f, 0.011571f, 0.647471f } };
            outBoneTransform[18] = { { 0.040331f, 0.000000f, 0.000000f, 1 },
                                     { 0.664315f, 0.001595f, 0.001967f, 0.747449f } };
            outBoneTransform[19] = { { 0.028489f, -0.000000f, -0.000000f, 1 },
                                     { 0.626957f, -0.002784f, -0.003234f, 0.779042f } };
            outBoneTransform[20] = { { 0.022430f, -0.000000f, 0.000000f, 1 },
                                     { 1.000000f, 0.000000f, 0.000000f, 0.000000f } };
            outBoneTransform[21] = { { 0.001131f, -0.019295f, 0.015429f, 1 },
                                     { 0.479766f, 0.477833f, -0.630198f, 0.379934f } };
            outBoneTransform[22] = { { 0.062878f, 0.002844f, 0.000332f, 1 },
                                     { 0.827001f, 0.034282f, 0.003440f, 0.561144f } };
            outBoneTransform[23] = { { 0.029874f, 0.000000f, 0.000000f, 1 },
                                     { 0.702185f, -0.006716f, -0.009289f, 0.711903f } };
            outBoneTransform[24] = { { 0.017979f, 0.000000f, 0.000000f, 1 },
                                     { 0.676853f, 0.007956f, 0.009917f, 0.736009f } };
            outBoneTransform[25] = { { 0.018018f, 0.000000f, -0.000000f, 1 },
                                     { 1.000000f, -0.000000f, -0.000000f, -0.000000f } };

            outBoneTransform[28] = { { 0.000448f, 0.001536f, 0.116543f, 1 },
                                     { -0.039357f, 0.105143f, -0.928833f, -0.353079f } };
            outBoneTransform[29] = { { 0.003949f, -0.014869f, 0.130608f, 1 },
                                     { -0.055071f, 0.068695f, -0.944016f, -0.317933f } };
            outBoneTransform[30] = { { 0.003263f, -0.034685f, 0.139926f, 1 },
                                     { 0.019690f, -0.100741f, -0.957331f, -0.270149f } };
        } else {
            outBoneTransform[11] = { { -0.005787f, 0.006806f, 0.016534f, 1 },
                                     { 0.522315f, -0.514203f, 0.483700f, 0.478348f } };
            outBoneTransform[12] = { { -0.070953f, -0.000779f, -0.000997f, 1 },
                                     { 0.723653f, -0.097901f, 0.048546f, 0.681458f } };
            outBoneTransform[13] = { { -0.043108f, -0.000000f, -0.000000f, 1 },
                                     { 0.637464f, -0.002366f, -0.002831f, 0.770472f } };
            outBoneTransform[14] = { { -0.033266f, -0.000000f, -0.000000f, 1 },
                                     { 0.658008f, 0.002610f, 0.003196f, 0.753000f } };
            outBoneTransform[15] = { { -0.025892f, 0.000000f, -0.000000f, 1 },
                                     { 0.999195f, 0.000000f, 0.000000f, 0.040126f } };
            outBoneTransform[16] = { { -0.004123f, -0.006858f, 0.016563f, 1 },
                                     { 0.523374f, -0.489609f, 0.463997f, 0.520644f } };
            outBoneTransform[17] = { { -0.065876f, -0.001786f, -0.000693f, 1 },
                                     { 0.759970f, -0.055609f, 0.011571f, 0.647471f } };
            outBoneTransform[18] = { { -0.040331f, -0.000000f, -0.000000f, 1 },
                                     { 0.664315f, 0.001595f, 0.001967f, 0.747449f } };
            outBoneTransform[19] = { { -0.028489f, 0.000000f, 0.000000f, 1 },
                                     { 0.626957f, -0.002784f, -0.003234f, 0.779042f } };
            outBoneTransform[20] = { { -0.022430f, 0.000000f, -0.000000f, 1 },
                                     { 1.000000f, 0.000000f, 0.000000f, 0.000000f } };
            outBoneTransform[21] = { { -0.001131f, -0.019295f, 0.015429f, 1 },
                                     { 0.477833f, -0.479766f, 0.379935f, 0.630198f } };
            outBoneTransform[22] = { { -0.062878f, -0.002844f, -0.000332f, 1 },
                                     { 0.827001f, 0.034282f, 0.003440f, 0.561144f } };
            outBoneTransform[23] = { { -0.029874f, -0.000000f, -0.000000f, 1 },
                                     { 0.702185f, -0.006716f, -0.009289f, 0.711903f } };
            outBoneTransform[24] = { { -0.017979f, -0.000000f, -0.000000f, 1 },
                                     { 0.676853f, 0.007956f, 0.009917f, 0.736009f } };
            outBoneTransform[25] = { { -0.018018f, -0.000000f, 0.000000f, 1 },
                                     { 1.000000f, -0.000000f, -0.000000f, -0.000000f } };

            outBoneTransform[28] = { { -0.000448f, 0.001536f, 0.116543f, 1 },
                                     { -0.039357f, 0.105143f, 0.928833f, 0.353079f } };
            outBoneTransform[29] = { { -0.003949f, -0.014869f, 0.130608f, 1 },
                                     { -0.055071f, 0.068695f, 0.944016f, 0.317933f } };
            outBoneTransform[30] = { { -0.003263f, -0.034685f, 0.139926f, 1 },
                                     { 0.019690f, -0.100741f, 0.957331f, 0.270149f } };
        }
    }
}

void Controller::GetBoneTransform(bool withController, vr::VRBoneTransform_t outBoneTransform[]) {
    auto isLeftHand = device_id == HAND_LEFT_ID;

    vr::VRBoneTransform_t boneTransform1[SKELETON_BONE_COUNT];
    vr::VRBoneTransform_t boneTransform2[SKELETON_BONE_COUNT];

    // root and wrist
    outBoneTransform[0] = { { 0.000000f, 0.000000f, 0.000000f, 1 },
                            { 1.000000f, -0.000000f, -0.000000f, 0.000000f } };
    if (isLeftHand) {
        outBoneTransform[1] = { { -0.034038f, 0.036503f, 0.164722f, 1 },
                                { -0.055147f, -0.078608f, -0.920279f, 0.379296f } };
    } else {
        outBoneTransform[1] = { { 0.034038f, 0.036503f, 0.164722f, 1 },
                                { -0.055147f, -0.078608f, 0.920279f, -0.379296f } };
    }

    // thumb
    GetThumbBoneTransform(withController, isLeftHand, m_lastThumbTouch, boneTransform1);
    GetThumbBoneTransform(withController, isLeftHand, m_currentThumbTouch, boneTransform2);
    for (int boneIdx = 2; boneIdx < 6; boneIdx++) {
        outBoneTransform[boneIdx].position = Lerp(
            boneTransform1[boneIdx].position,
            boneTransform2[boneIdx].position,
            m_thumbTouchAnimationProgress
        );
        outBoneTransform[boneIdx].orientation = Slerp(
            boneTransform1[boneIdx].orientation,
            boneTransform2[boneIdx].orientation,
            m_thumbTouchAnimationProgress
        );
    }

    // trigger (index to pinky)
    if (m_triggerValue > 0) {
        GetTriggerBoneTransform(withController, isLeftHand, true, false, boneTransform1);
        GetTriggerBoneTransform(withController, isLeftHand, true, true, boneTransform2);
        for (int boneIdx = 6; boneIdx < SKELETON_BONE_COUNT; boneIdx++) {
            outBoneTransform[boneIdx].position = Lerp(
                boneTransform1[boneIdx].position, boneTransform2[boneIdx].position, m_triggerValue
            );
            outBoneTransform[boneIdx].orientation = Slerp(
                boneTransform1[boneIdx].orientation,
                boneTransform2[boneIdx].orientation,
                m_triggerValue
            );
        }
    } else {
        GetTriggerBoneTransform(
            withController, isLeftHand, m_lastTriggerTouch, false, boneTransform1
        );
        GetTriggerBoneTransform(
            withController, isLeftHand, m_currentTriggerTouch, false, boneTransform2
        );
        for (int boneIdx = 6; boneIdx < SKELETON_BONE_COUNT; boneIdx++) {
            outBoneTransform[boneIdx].position = Lerp(
                boneTransform1[boneIdx].position,
                boneTransform2[boneIdx].position,
                m_indexTouchAnimationProgress
            );
            outBoneTransform[boneIdx].orientation = Slerp(
                boneTransform1[boneIdx].orientation,
                boneTransform2[boneIdx].orientation,
                m_indexTouchAnimationProgress
            );
        }
    }

    // grip (middle to pinky)
    if (m_gripValue > 0) {
        GetGripClickBoneTransform(withController, isLeftHand, boneTransform2);
        for (int boneIdx = 11; boneIdx < 26; boneIdx++) {
            outBoneTransform[boneIdx].position = Lerp(
                outBoneTransform[boneIdx].position, boneTransform2[boneIdx].position, m_gripValue
            );
            outBoneTransform[boneIdx].orientation = Slerp(
                outBoneTransform[boneIdx].orientation,
                boneTransform2[boneIdx].orientation,
                m_gripValue
            );
        }
        for (int boneIdx = 28; boneIdx < SKELETON_BONE_COUNT; boneIdx++) {
            outBoneTransform[boneIdx].position = Lerp(
                outBoneTransform[boneIdx].position, boneTransform2[boneIdx].position, m_gripValue
            );
            outBoneTransform[boneIdx].orientation = Slerp(
                outBoneTransform[boneIdx].orientation,
                boneTransform2[boneIdx].orientation,
                m_gripValue
            );
        }
    }
}
