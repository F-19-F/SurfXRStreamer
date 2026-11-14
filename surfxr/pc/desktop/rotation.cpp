#include "rotation.hpp"
#include <d3dcompiler.h>

namespace surfxr::transport::desktop {
Rotation::Rotation(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* target,DXGI_MODE_ROTATION rotation)
    :device_(device),context_(context),rotation_(rotation){
    D3D11_TEXTURE2D_DESC d{};target->GetDesc(&d);width_=d.Width;height_=d.Height;
    check(device_->CreateRenderTargetView(target,nullptr,&target_),"desktop_rotation_target");
    const char shader[]=R"(
cbuffer Shape:register(b0){uint4 shape;};
float4 vs(uint id:SV_VertexID):SV_POSITION{float2 p=float2((id==1||id==3)?1:0,id>=2?1:0);return float4(p.x*2-1,1-p.y*2,0,1);}
Texture2D image:register(t0);
float4 ps(float4 position:SV_POSITION):SV_TARGET{
    int2 p=int2(position.xy);
    if(shape.z==2)p=int2(p.y,int(shape.y)-1-p.x);
    else if(shape.z==3)p=int2(int(shape.x)-1-p.x,int(shape.y)-1-p.y);
    else if(shape.z==4)p=int2(int(shape.x)-1-p.y,p.x);
    return image.Load(int3(p,0));
})";
    Com<ID3DBlob> vs,ps,error;check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error),"desktop_rotation_vertex_compile");
    check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error),"desktop_rotation_pixel_compile");
    check(device_->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_),"desktop_rotation_vertex");
    check(device_->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_),"desktop_rotation_pixel");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=16;buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check(device_->CreateBuffer(&buffer,nullptr,&constants_),"desktop_rotation_constants");
}
void Rotation::copy(ID3D11Texture2D* source){
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
    if(d.Width!=source_width_||d.Height!=source_height_){
        const bool swap=rotation_==DXGI_MODE_ROTATION_ROTATE90||rotation_==DXGI_MODE_ROTATION_ROTATE270;
        require((swap?d.Height:d.Width)>=width_&&(swap?d.Width:d.Height)>=height_,"desktop_rotation_dimensions");
        source_.Reset();image_.Reset();d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=0;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;
        check(device_->CreateTexture2D(&d,nullptr,&source_),"desktop_rotation_texture");check(device_->CreateShaderResourceView(source_.Get(),nullptr,&image_),"desktop_rotation_view");
        source_width_=d.Width;source_height_=d.Height;
    }
    context_->CopyResource(source_.Get(),source);
    const unsigned shape[]{source_width_,source_height_,static_cast<unsigned>(rotation_),0};context_->UpdateSubresource(constants_.Get(),0,nullptr,shape,0,0);
    D3D11_VIEWPORT viewport{0,0,static_cast<float>(width_),static_cast<float>(height_),0,1};context_->RSSetViewports(1,&viewport);context_->RSSetState(nullptr);
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->OMSetRenderTargets(1,target_.GetAddressOf(),nullptr);context_->OMSetBlendState(nullptr,nullptr,~0u);
    context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);context_->PSSetConstantBuffers(0,1,constants_.GetAddressOf());
    context_->PSSetShaderResources(0,1,image_.GetAddressOf());context_->Draw(4,0);
    ID3D11ShaderResourceView* empty=nullptr;context_->PSSetShaderResources(0,1,&empty);context_->OMSetRenderTargets(0,nullptr,nullptr);
}
void rotation_self_test(ID3D11Device* device,ID3D11DeviceContext* context){
    // Asymmetric synthetic color cells make transposition and mirroring visible
    // to a numeric check; no desktop pixels are acquired or saved.
    const std::uint32_t cells[]{0xff000001,0xff000002,0xff000003,0xff000004,0xff000005,0xff000006};
    constexpr unsigned expected[4][6]{{1,2,3,4,5,6},{4,1,5,2,6,3},{6,5,4,3,2,1},{3,6,2,5,1,4}};
    D3D11_TEXTURE2D_DESC d{};d.Width=3;d.Height=2;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{cells,12,0};Com<ID3D11Texture2D> source;check(device->CreateTexture2D(&d,&data,&source),"desktop_rotation_test_source");
    for(unsigned i=0;i<4;++i){d.Width=i%2?2:3;d.Height=i%2?3:2;d.BindFlags=D3D11_BIND_RENDER_TARGET;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;Com<ID3D11Texture2D> target,readback;
        check(device->CreateTexture2D(&d,nullptr,&target),"desktop_rotation_test_target");Rotation rotate(device,context,target.Get(),static_cast<DXGI_MODE_ROTATION>(i+1));rotate.copy(source.Get());
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&d,nullptr,&readback),"desktop_rotation_test_readback");context->CopyResource(readback.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"desktop_rotation_test_map");bool correct=true;
        for(unsigned y=0;y<d.Height;++y)for(unsigned x=0;x<d.Width;++x){const auto* row=reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(mapped.pData)+y*mapped.RowPitch);correct=correct&&row[x]==(0xff000000|expected[i][y*d.Width+x]);}
        context->Unmap(readback.Get(),0);require(correct,"desktop_rotation_test_orientation");
    }
}
}
