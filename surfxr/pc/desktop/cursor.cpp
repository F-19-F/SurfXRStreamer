#include "cursor.hpp"
#include <d3dcompiler.h>
#include <vector>
#include <algorithm>

namespace surfxr::transport::desktop {
Cursor::Cursor(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,ID3D11Texture2D* background,Display display)
    :device_(device),context_(context),display_(display){
    check(device->CreateRenderTargetView(texture,nullptr,&target_),"desktop_cursor_target");
    check(device->CreateShaderResourceView(background,nullptr,&background_),"desktop_cursor_background");
    // Small code-native GPU compositor; no external/generated shader manifest.
    const char source[]=R"(
cbuffer Rect:register(b0){float4 rect;float4 flags;};
struct V{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){float2 uv=float2((id==1||id==3)?1:0,id>=2?1:0);V v;v.uv=uv;v.pos=float4(rect.xy+uv*rect.zw,0,1);return v;}
Texture2D image:register(t0);Texture2D background:register(t1);SamplerState sampleImage:register(s0);
float4 ps(V v):SV_TARGET{float4 c=image.Sample(sampleImage,v.uv);if(flags.x>0&&c.a>.49&&c.a<.51){uint3 b=(uint3)round(background.Load(int3(v.pos.xy,0)).rgb*255);uint3 m=(uint3)round(c.rgb*255);return float4((b^m)/255.,1);}return c;}
)";
    Com<ID3DBlob> vs,ps,error;check(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error),"desktop_cursor_vertex_compile");
    check(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error),"desktop_cursor_pixel_compile");
    check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_),"desktop_cursor_vertex");check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_),"desktop_cursor_pixel");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=32;buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;check(device->CreateBuffer(&buffer,nullptr,&constants_),"desktop_cursor_constants");
    D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_ONE;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    check(device->CreateBlendState(&blend,&blend_),"desktop_cursor_blend");D3D11_SAMPLER_DESC sample{};sample.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sample.MaxLOD=1;check(device->CreateSamplerState(&sample,&sampler_),"desktop_cursor_sampler");
}
void Cursor::update(HCURSOR cursor){
    ICONINFO info{};require(GetIconInfo(cursor,&info),"desktop_cursor_icon");struct Release {ICONINFO& i;~Release(){if(i.hbmColor)DeleteObject(i.hbmColor);if(i.hbmMask)DeleteObject(i.hbmMask);}} release{info};
    BITMAP bitmap{};require(GetObjectW(info.hbmColor?info.hbmColor:info.hbmMask,sizeof(bitmap),&bitmap),"desktop_cursor_bitmap");width_=bitmap.bmWidth;height_=info.hbmColor?bitmap.bmHeight:bitmap.bmHeight/2;require(width_>0&&width_<=256&&height_>0&&height_<=256,"desktop_cursor_size");hot_x_=static_cast<int>(info.xHotspot);hot_y_=static_cast<int>(info.yHotspot);
    BITMAPINFO format{};format.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);format.bmiHeader.biWidth=width_;format.bmiHeader.biHeight=-height_;format.bmiHeader.biPlanes=1;format.bmiHeader.biBitCount=32;format.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HBITMAP dib=CreateDIBSection(nullptr,&format,DIB_RGB_COLORS,&pixels,nullptr,0);require(dib&&pixels,"desktop_cursor_dib");HDC dc=CreateCompatibleDC(nullptr);require(dc,"desktop_cursor_dc");auto old=SelectObject(dc,dib);
    std::memset(pixels,0,static_cast<std::size_t>(width_)*height_*4);const BOOL drawn=DrawIconEx(dc,0,0,cursor,width_,height_,0,nullptr,DI_NORMAL);GdiFlush();
    std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(pixels),static_cast<std::uint8_t*>(pixels)+static_cast<std::size_t>(width_)*height_*4);SelectObject(dc,old);DeleteDC(dc);DeleteObject(dib);require(drawn,"desktop_cursor_draw");
    bool alpha=false;for(std::size_t i=3;i<bytes.size();i+=4)alpha=alpha||bytes[i]!=0;
    xor_mask_=!info.hbmColor||!alpha;
    if(xor_mask_){
        const int mask_height=height_*(info.hbmColor?1:2);
        struct MaskFormat {BITMAPINFOHEADER header;RGBQUAD colors[2];} mask_format{};mask_format.header.biSize=sizeof(BITMAPINFOHEADER);mask_format.header.biWidth=width_;mask_format.header.biHeight=-mask_height;mask_format.header.biPlanes=1;mask_format.header.biBitCount=1;mask_format.header.biCompression=BI_RGB;
        const unsigned stride=(static_cast<unsigned>(width_)+31)/32*4;std::vector<std::uint8_t> mask(static_cast<std::size_t>(stride)*mask_height);HDC mask_dc=CreateCompatibleDC(nullptr);require(mask_dc,"desktop_cursor_mask_dc");const auto rows=GetDIBits(mask_dc,info.hbmMask,0,static_cast<UINT>(mask_height),mask.data(),reinterpret_cast<BITMAPINFO*>(&mask_format),DIB_RGB_COLORS);DeleteDC(mask_dc);require(rows==mask_height,"desktop_cursor_mask_read");
        for(int y=0;y<height_;++y)for(int x=0;x<width_;++x){const auto bit=0x80u>>(x%8);const bool and_bit=(mask[static_cast<std::size_t>(y)*stride+x/8]&bit)!=0;const auto offset=(static_cast<std::size_t>(y)*width_+x)*4;
            if(!info.hbmColor){const bool xor_bit=(mask[static_cast<std::size_t>(y+height_)*stride+x/8]&bit)!=0;bytes[offset]=bytes[offset+1]=bytes[offset+2]=xor_bit?255:0;}
            const bool nonzero=bytes[offset]||bytes[offset+1]||bytes[offset+2];bytes[offset+3]=and_bit?(nonzero?128:0):255;}
    }
    D3D11_TEXTURE2D_DESC desc{};desc.Width=static_cast<UINT>(width_);desc.Height=static_cast<UINT>(height_);desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{bytes.data(),static_cast<UINT>(width_*4),0};Com<ID3D11Texture2D> texture;check(device_->CreateTexture2D(&desc,&data,&texture),"desktop_cursor_texture");image_.Reset();check(device_->CreateShaderResourceView(texture.Get(),nullptr,&image_),"desktop_cursor_view");shape_=cursor;
}
void Cursor::draw(){
    CURSORINFO cursor{sizeof(cursor)};if(!GetCursorInfo(&cursor)||!(cursor.flags&CURSOR_SHOWING)||!cursor.hCursor)return;if(cursor.hCursor!=shape_)try{update(cursor.hCursor);}catch(const std::runtime_error&){return;}
    const float x=static_cast<float>(cursor.ptScreenPos.x-display_.x-hot_x_),y=static_cast<float>(cursor.ptScreenPos.y-display_.y-hot_y_);
    float rect[]{2*x/display_.encoded_width-1,1-2*y/display_.encoded_height,2.f*width_/display_.encoded_width,-2.f*height_/display_.encoded_height,xor_mask_?1.f:0.f,0,0,0};context_->UpdateSubresource(constants_.Get(),0,nullptr,rect,0,0);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(display_.encoded_width),static_cast<float>(display_.encoded_height),0,1};context_->RSSetViewports(1,&viewport);context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->OMSetRenderTargets(1,target_.GetAddressOf(),nullptr);context_->OMSetBlendState(blend_.Get(),nullptr,~0u);context_->VSSetShader(vertex_.Get(),nullptr,0);context_->VSSetConstantBuffers(0,1,constants_.GetAddressOf());context_->PSSetConstantBuffers(0,1,constants_.GetAddressOf());context_->PSSetShader(pixel_.Get(),nullptr,0);context_->PSSetSamplers(0,1,sampler_.GetAddressOf());ID3D11ShaderResourceView* images[]{image_.Get(),background_.Get()};context_->PSSetShaderResources(0,2,images);context_->Draw(4,0);
    ID3D11ShaderResourceView* empty[2]{};context_->PSSetShaderResources(0,2,empty);context_->OMSetRenderTargets(0,nullptr,nullptr);
}
}
