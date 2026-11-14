#pragma once
#include "surfxr_server/openvr_driver_wrap.h"
#include <directxmath.h>

namespace surfxr {
// Row-vector form of the head orientation, with the existing image Y basis.
// No depth is available here: this transform must not invent translation.
inline DirectX::XMMATRIX frame_render_orientation(const vr::HmdMatrix34_t& pose) {
    return DirectX::XMMatrixSet(
        pose.m[0][0], pose.m[1][0], pose.m[2][0], 0,
        -pose.m[0][1], -pose.m[1][1], -pose.m[2][1], 0,
        pose.m[0][2], pose.m[1][2], pose.m[2][2], 0,
        0, 0, 0, 1);
}

// Every layer (including layer zero) is reprojected to the pose associated
// with the encoded timestamp. A nearest history match is an identity lookup,
// not proof that SteamVR's submitted render orientation is identical to it.
inline DirectX::XMMATRIX frame_render_rotation(
    const vr::HmdMatrix34_t& source, const vr::HmdMatrix34_t& target
) {
    const auto source_inverse = DirectX::XMMatrixInverse(nullptr, frame_render_orientation(source));
    return DirectX::XMMatrixInverse(nullptr, frame_render_orientation(target) * source_inverse);
}
} // namespace surfxr
