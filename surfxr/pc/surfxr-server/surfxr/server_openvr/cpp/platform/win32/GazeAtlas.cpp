#include "GazeAtlas.h"
#include "GazeAtlasShader.hpp"
#include <d3dcompiler.h>
#include <stdexcept>
using namespace d3d_render_utils;
using Microsoft::WRL::ComPtr;
GazeAtlas::GazeAtlas(ID3D11Device* device, ID3D11Texture2D* source) : pipeline_(device) {
    static_assert(sizeof(Geometry) == 48);
    const auto* settings = Settings_Instance();
    const auto a = settings->m_frameGaze;
    geometry_ = {{settings->m_renderWidth / 2, settings->m_renderHeight}, {a.baseWidth, a.baseHeight},
        {a.focusWidth, a.focusHeight}, {0, 0}, {}};
    device->GetImmediateContext(&context_);
    constants_ = CreateBuffer(device, geometry_, D3D11_USAGE_DEFAULT);
    output_ = CreateTexture(device, 2 * a.baseWidth, a.baseHeight + a.focusHeight, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    // Existing SURFXR embeds compiled shaders. This new optional pass embeds its
    // source and uses the OS compiler once, without a second build-time helper.
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) throw std::runtime_error("Frame gaze shader compiler unavailable");
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
    ComPtr<ID3DBlob> code, errors;
    const HRESULT result = compile ? compile(GazeAtlasShader, sizeof(GazeAtlasShader)-1, "FrameGazeAtlas", nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors) : E_FAIL;
    if (FAILED(result)) {
        std::string message="Frame gaze shader compilation failed";
        if(errors)message.append(": ").append(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());
        code.Reset(); errors.Reset(); FreeLibrary(compiler); throw std::runtime_error(message);
    }
    std::vector<uint8_t> shader(static_cast<const uint8_t*>(code->GetBufferPointer()),
        static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
    code.Reset(); errors.Reset(); FreeLibrary(compiler);
    std::vector<uint8_t> quad(QUAD_SHADER_CSO_PTR, QUAD_SHADER_CSO_PTR + QUAD_SHADER_CSO_LEN);
    ComPtr<ID3D11VertexShader> vertex = CreateVertexShader(device, quad);
    pipeline_.Initialize({source}, vertex.Get(), shader, output_.Get(), constants_.Get());
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    OK_OR_THROW(device->CreateSamplerState(&desc, &sampler_), "Frame gaze sampler");
}
void GazeAtlas::Render(uint64_t timestamp) {
    geometry_.crops = GetFrameGazeCrops(timestamp);
    UpdateBuffer(context_.Get(), constants_.Get(), &geometry_);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    pipeline_.Render();
}
void GazeAtlas::Resolution(uint32_t* width, uint32_t* height) const {
    *width = geometry_.baseSize[0] * 2;
    *height = geometry_.baseSize[1] + geometry_.focusSize[1];
}
