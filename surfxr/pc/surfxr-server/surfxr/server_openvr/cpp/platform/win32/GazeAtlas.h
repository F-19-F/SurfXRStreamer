#pragma once
#include "surfxr_server/bindings.h"
#include "d3d-render-utils/RenderPipeline.h"
class GazeAtlas {
public:
    GazeAtlas(ID3D11Device* device, ID3D11Texture2D* source);
    void Render(uint64_t timestamp);
    ID3D11Texture2D* Texture() const { return output_.Get(); }
    void Resolution(uint32_t* width, uint32_t* height) const;
private:
    struct Geometry {
        uint32_t eyeSize[2], baseSize[2], focusSize[2], padding[2];
        FfiFrameGazeCrops crops;
    } geometry_{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    d3d_render_utils::RenderPipeline pipeline_;
};
