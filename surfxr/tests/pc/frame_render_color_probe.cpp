// Offscreen regressions using production input preparation and embedded CSOs.
// This isolates shader/color/blend behavior, not SteamVR's choice of input format.
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "FrameRenderInput.hpp"

namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT value,const char* stage){if(FAILED(value))throw std::runtime_error(stage);}
std::vector<char> shader(const char* name){
    std::ifstream input(std::filesystem::path(SURFXR_RENDER_SHADER_DIR)/name,std::ios::binary);
    if(!input)throw std::runtime_error("production_shader_unavailable");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
struct Vertex {float position[4],uv[2];std::uint32_t view;};
struct Probe {
    ComPtr<ID3D11Device> device,producer;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Buffer> constants;ComPtr<ID3D11BlendState> blend;
    std::string adapter_name;
    surfxr::FrameRenderInput input;
    Probe(){
        checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11CreateDevice");
        ComPtr<IDXGIDevice> dxgi;checked(device.As(&dxgi),"dxgi_device");ComPtr<IDXGIAdapter> adapter;
        checked(dxgi->GetAdapter(&adapter),"dxgi_adapter");DXGI_ADAPTER_DESC description{};
        checked(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &producer,nullptr,nullptr),"producer_device");
        checked(adapter->GetDesc(&description),"adapter_description");
        adapter_name=std::filesystem::path(description.Description).string();
        const auto vertex=shader("FrameRenderVS.cso"),pixel=shader("FrameRenderPS.cso");
        checked(device->CreateVertexShader(vertex.data(),vertex.size(),nullptr,&vs),"production_vertex_shader");
        checked(device->CreatePixelShader(pixel.data(),pixel.size(),nullptr,&ps),"production_pixel_shader");
        const D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"VIEW",0,DXGI_FORMAT_R32_UINT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};
        checked(device->CreateInputLayout(elements,3,vertex.data(),vertex.size(),&layout),"input_layout");
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_ANISOTROPIC;sd.MaxAnisotropy=D3D11_REQ_MAXANISOTROPY;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        checked(device->CreateSamplerState(&sd,&sampler),"sampler");
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        checked(device->CreateTexture2D(&td,nullptr,&target),"target");
        checked(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"target_view");
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(device->CreateTexture2D(&td,nullptr,&staging),"staging");
        const std::array<float,4> gamma{1,0,0,0};D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{gamma.data(),0,0};checked(device->CreateBuffer(&bd,&initial,&constants),"constants");
        D3D11_BLEND_DESC bs{};auto& b=bs.RenderTarget[0];b.BlendEnable=true;
        b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_ZERO;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;checked(device->CreateBlendState(&bs,&blend),"layer_blend");
    }
    std::array<unsigned,4> run(DXGI_FORMAT format,const void* bytes,unsigned stride,unsigned width,bool overlay,
                               bool hdr=false,float gamma=1){
        D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=1;td.MipLevels=td.ArraySize=1;
        td.Format=format;
        td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
        D3D11_SUBRESOURCE_DATA initial{bytes,stride,0};ComPtr<ID3D11Texture2D> texture;
        checked(device->CreateTexture2D(&td,&initial,&texture),"source");ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_RENDER_TARGET_VIEW_DESC render_view{};render_view.Format=format;render_view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> producer_view;
        ComPtr<IDXGIResource> shared;checked(texture.As(&shared),"shared_resource");HANDLE handle{};
        checked(shared->GetSharedHandle(&handle),"shared_handle");ComPtr<ID3D11Texture2D> opened;
        checked(producer->OpenSharedResource(handle,IID_PPV_ARGS(&opened)),"producer_open_shared_resource");
        checked(producer->CreateRenderTargetView(opened.Get(),&render_view,&producer_view),"requested_format_render_view");
        std::uint32_t correction=0;
        checked(input.Prepare(device.Get(),texture.Get(),hdr,false,false,srv,correction),"production_source_view");
        D3D11_SHADER_RESOURCE_VIEW_DESC actual_view{};srv->GetDesc(&actual_view);
        if(actual_view.Format!=format)throw std::runtime_error("input_format_reinterpretation");
        ComPtr<ID3D11Resource> sampled;srv->GetResource(&sampled);
        if(sampled.Get()!=texture.Get())throw std::runtime_error("input_preparation_must_not_copy");
        const std::array<float,4> parameters{gamma,0,0,0};context->UpdateSubresource(constants.Get(),0,nullptr,parameters.data(),0,0);
        // A constant coordinate isolates the 50/50 filtering footprint.
        const std::array<Vertex,3> vertices{{{{-1,-1,0,1},{.5f,.5f},correction<<1},
            {{-1,3,0,1},{.5f,.5f},correction<<1},{{3,-1,0,1},{.5f,.5f},correction<<1}}};
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(vertices);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        initial={vertices.data(),0,0};ComPtr<ID3D11Buffer> vb;checked(device->CreateBuffer(&bd,&initial,&vb),"vertices");
        const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);
        context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);context->OMSetBlendState(overlay?blend.Get():nullptr,nullptr,~0u);
        D3D11_VIEWPORT viewport{0,0,4,4,0,1};context->RSSetViewports(1,&viewport);
        context->IASetInputLayout(layout.Get());const UINT step=sizeof(Vertex),offset=0;
        context->IASetVertexBuffers(0,1,vb.GetAddressOf(),&step,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        context->PSSetConstantBuffers(0,1,constants.GetAddressOf());context->PSSetSamplers(0,1,sampler.GetAddressOf());
        ID3D11ShaderResourceView* views[]{srv.Get(),srv.Get()};context->PSSetShaderResources(0,2,views);context->Draw(3,0);
        context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"readback");
        const auto* p=static_cast<const std::uint8_t*>(mapped.pData)+2*mapped.RowPitch+8;
        const std::array<unsigned,4> result{p[0],p[1],p[2],p[3]};context->Unmap(staging.Get(),0);
        ID3D11ShaderResourceView* none[]{nullptr,nullptr};context->PSSetShaderResources(0,2,none);return result;
    }
};
}
void frame_render_color_probe(){
    Probe gpu;
    const std::array<std::uint8_t,4> overlay{255,255,255,128};
    const std::array<std::uint8_t,8> edge{0,0,0,255,255,255,255,255};
    const std::array<std::uint16_t,4> linear{0x3800,0x3800,0x3800,0x3c00}; // FP16 .5, .5, .5, 1
    const auto report=[](const char* name,const std::array<unsigned,4>& pixel,unsigned expected){
        std::cout<<"{\"case\":\""<<name<<"\",\"rgba\":["<<pixel[0]<<','<<pixel[1]<<','<<pixel[2]<<','<<pixel[3]
            <<"],\"reference_srgb_code\":"<<expected<<",\"rgb_max_error\":"<<std::max({std::abs(int(pixel[0])-int(expected)),std::abs(int(pixel[1])-int(expected)),std::abs(int(pixel[2])-int(expected))})<<"}\n";
        for(unsigned c=0;c<3;++c)if(std::abs(int(pixel[c])-int(expected))>1)
            throw std::runtime_error("render_input_must_preserve_linear_light");
    };
    const auto alpha_control=gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,overlay.data(),4,1,true);
    const auto edge_control=gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,edge.data(),8,2,false);
    for(unsigned c=0;c<3;++c)if(std::abs(int(alpha_control[c])-188)>1||std::abs(int(edge_control[c])-188)>1)
        throw std::runtime_error("render_probe_linear_reference_failed");
    if(alpha_control[3]!=128||edge_control[3]!=255)throw std::runtime_error("render_probe_alpha_reference_failed");
    report("srgb_overlay_alpha_control",alpha_control,188);
    const auto alpha=gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM,overlay.data(),4,1,true);
    if(alpha[3]!=128)throw std::runtime_error("render_alpha_must_remain_linear");
    report("unorm_overlay_alpha",alpha,188);
    report("srgb_edge_filter_control",edge_control,188);
    // Preserve the existing sampling domains. These edge controls prevent this
    // transfer fix from silently introducing a different filtering algorithm.
    report("unorm_filter_unchanged",gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM,edge.data(),8,2,false),128);
    report("bgra_filter_unchanged",gpu.run(DXGI_FORMAT_B8G8R8A8_UNORM,edge.data(),8,2,false),128);
    report("bgrx_filter_unchanged",gpu.run(DXGI_FORMAT_B8G8R8X8_UNORM,edge.data(),8,2,false),128);
    report("linear_fp16_half_intensity",gpu.run(DXGI_FORMAT_R16G16B16A16_FLOAT,linear.data(),8,1,false),188);
    for(bool hdr:{false,true}){
        const auto pixel=gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,overlay.data(),4,1,true,hdr,hdr?1.f:2.f);
        if(pixel[3]!=128)throw std::runtime_error("shader_transfer_changed_alpha");
        report(hdr?"hdr_transfer_alpha":"encoding_gamma_alpha",pixel,188);
    }
    // Distinct layers and format/size replacement must never retain pixels
    // from another layer. BGRA red also catches channel reinterpretation.
    for(const auto code:{0u,64u,128u,255u,0u}){
        const std::array<std::uint8_t,4> gray{std::uint8_t(code),std::uint8_t(code),std::uint8_t(code),255};
        report("independent_layer",gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM,gray.data(),4,1,false),code);
    }
    const std::array<std::uint8_t,4> red{0,0,255,255};
    const auto pixel=gpu.run(DXGI_FORMAT_B8G8R8A8_UNORM,red.data(),4,1,false);
    if(pixel!=std::array<unsigned,4>{255,0,0,255})throw std::runtime_error("bgra_channel_order");
    const std::array<std::uint8_t,4> encoded_color{64,128,192,255};
    const auto rgb=gpu.run(DXGI_FORMAT_R8G8B8A8_UNORM,encoded_color.data(),4,1,false);
    const std::array<std::uint16_t,4> linear_color{0x3000,0x3800,0x3a00,0x3c00}; // .125, .5, .75, 1
    const auto linear_rgb=gpu.run(DXGI_FORMAT_R16G16B16A16_FLOAT,linear_color.data(),8,1,false);
    const std::array<unsigned,4> expected_linear{99,188,225,255};
    for(unsigned c=0;c<4;++c){
        if(std::abs(int(rgb[c])-int(encoded_color[c]))>1)throw std::runtime_error("encoded_rgb_roundtrip");
        if(std::abs(int(linear_rgb[c])-int(expected_linear[c]))>1)throw std::runtime_error("linear_rgb_must_not_decode_srgb_again");
    }
    std::cout<<"{\"case\":\"colored_rgb_transfer\",\"unorm\":["<<rgb[0]<<','<<rgb[1]<<','<<rgb[2]
        <<"],\"linear_fp16\":["<<linear_rgb[0]<<','<<linear_rgb[1]<<','<<linear_rgb[2]<<"],\"passed\":true}\n";
    std::cout<<"{\"adapter\":\""<<gpu.adapter_name<<"\",\"production_cso\":true,\"production_input_owner\":true,\"same_input_resource\":true,\"cross_device_typed_render_view\":true,\"passed\":true,\"network_used\":false,\"steamvr_started\":false}\n";
}
