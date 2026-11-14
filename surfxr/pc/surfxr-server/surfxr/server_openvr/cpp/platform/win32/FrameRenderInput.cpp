#include "FrameRenderInput.hpp"

namespace surfxr {
namespace {
bool is_srgb(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
bool is_linear_float(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT
        || format == DXGI_FORMAT_R32G32B32A32_FLOAT
        || format == DXGI_FORMAT_R11G11B10_FLOAT;
}
}
HRESULT FrameRenderInput::Prepare(ID3D11Device* device,
                                 ID3D11Texture2D* source, bool hdr, bool forceCorrection, bool clamp,
                                 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view,
                                 std::uint32_t& correction) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const auto format = desc.Format;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    view.Reset();
    const auto result = device->CreateShaderResourceView(source, &srv, &view);
    if (FAILED(result)) return result;
    correction = hdr ? (is_srgb(format) || forceCorrection ? 1u : 0u)
                     : (!is_srgb(format) && !is_linear_float(format) && !forceCorrection ? 2u : 0u);
    if (clamp) correction |= 0x10u;
    return S_OK;
}
}
