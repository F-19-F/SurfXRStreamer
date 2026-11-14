#pragma once
#include "windows.hpp"
#include <d3d11.h>

namespace surfxr::transport::desktop {
class Cursor {
public:
    Cursor(ID3D11Device*,ID3D11DeviceContext*,ID3D11Texture2D*,ID3D11Texture2D* background,Display);
    void draw();
private:
    ID3D11Device* device_;ID3D11DeviceContext* context_;Display display_;
    Com<ID3D11RenderTargetView> target_;Com<ID3D11VertexShader> vertex_;Com<ID3D11PixelShader> pixel_;
    Com<ID3D11BlendState> blend_;Com<ID3D11SamplerState> sampler_;Com<ID3D11Buffer> constants_;
    Com<ID3D11ShaderResourceView> image_,background_;HCURSOR shape_=nullptr;int width_=0,height_=0,hot_x_=0,hot_y_=0;
    bool xor_mask_=false;
    void update(HCURSOR);
};
}
