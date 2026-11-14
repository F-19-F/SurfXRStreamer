#pragma once
// Compiled once by the Windows D3D compiler when this explicitly negotiated
inline constexpr char GazeAtlasShader[] = R"hlsl(
Texture2D<float4> source : register(t0);
SamplerState linearSampler : register(s0);
cbuffer Geometry : register(b0) {
    uint2 eyeSize;
    uint2 baseSize;
    uint2 focusSize;
    uint2 padding;
    uint2 leftCrop;
    uint2 rightCrop;
};
float originalUV(float encoded) {
    if(encoded < 0.125) return sqrt(encoded) * 0.7071067811865475;
    if(encoded < 0.875) return (encoded - 0.125) * (2.0 / 3.0) + 0.25;
    return 1.0 - sqrt(1.0 - encoded) * 0.7071067811865475;
}
float4 main(float4 position : SV_POSITION) : SV_TARGET {
    uint eye = uint(position.x) / baseSize.x;
    float2 p = float2(position.x - float(eye * baseSize.x), position.y);
    float2 uv;
    if(position.y < float(baseSize.y)) {
        float2 encoded = p / float2(baseSize);
        uv = float2(originalUV(encoded.x), originalUV(encoded.y));
    } else {
        p.y -= float(baseSize.y);
        if(p.x >= float(focusSize.x)) return float4(0,0,0,1);
        uv = (float2(eye == 0u ? leftCrop : rightCrop) + p) / float2(eyeSize);
    }
    // Clamp each original eye separately; never bleed the stereo seam.
    uv = clamp(uv, 0.5 / float2(eyeSize), 1.0 - 0.5 / float2(eyeSize));
    uv.x = (uv.x + float(eye)) * 0.5;
    return float4(source.SampleLevel(linearSampler, uv, 0).rgb, 1);
}
)hlsl";
