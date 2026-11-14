#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace surfxr {
// Interpret each eye's existing texture format; never allocate or copy pixels.
class FrameRenderInput {
public:
    HRESULT Prepare(ID3D11Device* device,
                    ID3D11Texture2D* source, bool hdr, bool forceCorrection, bool clamp,
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view,
                    std::uint32_t& correction);
};
}
