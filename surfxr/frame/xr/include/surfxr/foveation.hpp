#pragma once
#include <array>
#include <cmath>

namespace surfxr {
// Quest fixed foveation, independently applied to top-left-origin X and Y.
// Forward: an encoded output texel samples this coordinate in the original eye.
inline float foveation_original_uv(float encoded) noexcept {
    if(encoded<.125f)return std::sqrt(encoded)*.7071067811865475f;
    if(encoded<.875f)return (encoded-.125f)*(2.f/3.f)+.25f;
    return 1.f-std::sqrt(1.f-encoded)*.7071067811865475f;
}
// Inverse image reconstruction: an original output texel samples this encoded
// coordinate. Domain [0,1]; no flip, FOV, pose or world-scale adjustment.
inline float foveation_encoded_uv(float original) noexcept {
    if(original<.25f)return 2.f*original*original;
    if(original<.75f)return 1.5f*original-.25f;
    const float remaining=1.f-original;
    return 1.f-2.f*remaining*remaining;
}
inline std::array<float,2> foveation_encoded_uv(std::array<float,2> original) noexcept {
    return {foveation_encoded_uv(original[0]),foveation_encoded_uv(original[1])};
}
} // namespace surfxr
