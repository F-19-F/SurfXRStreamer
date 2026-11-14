#pragma once
#include "windows.hpp"
#include <d3d11.h>
#include <dxgi.h>

namespace surfxr::transport::desktop {
// Desktop duplication surfaces are unrotated even for a portrait display.
class Rotation {
public:
    Rotation(ID3D11Device*,ID3D11DeviceContext*,ID3D11Texture2D* target,DXGI_MODE_ROTATION);
    void copy(ID3D11Texture2D* source);
private:
    ID3D11Device* device_;ID3D11DeviceContext* context_;DXGI_MODE_ROTATION rotation_;
    unsigned width_=0,height_=0,source_width_=0,source_height_=0;
    Com<ID3D11RenderTargetView> target_;Com<ID3D11Texture2D> source_;
    Com<ID3D11ShaderResourceView> image_;Com<ID3D11Buffer> constants_;
    Com<ID3D11VertexShader> vertex_;Com<ID3D11PixelShader> pixel_;
};
void rotation_self_test(ID3D11Device*,ID3D11DeviceContext*);
}
