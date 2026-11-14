// Offscreen WARP check of the production GazeAtlas and RenderPipeline.
// Run with the repository's existing QuadVertexShader.cso as argv[1].
#include "platform/win32/GazeAtlas.h"
#include "platform/win32/EncoderFrameMailbox.h"
#include "frame_worker_test.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>
using Microsoft::WRL::ComPtr;
void check_pose_history();
extern "C" {
const unsigned char* QUAD_SHADER_CSO_PTR=nullptr;
unsigned int QUAD_SHADER_CSO_LEN=0;
}
namespace {
Settings settings{};
FfiFrameGazeCrops crops{};
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
double linear(unsigned byte){const auto v=byte/255.;return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);}
unsigned srgb(double value){return unsigned(std::lround(255*(value<=.0031308?12.92*value:1.055*std::pow(value,1/2.4)-.055)));}
double original(double e){return e<.125?std::sqrt(e)*std::sqrt(.5):e<.875?(e-.125)*2/3+.25:1-std::sqrt(1-e)*std::sqrt(.5);}
unsigned code(unsigned eye,int x,int y,unsigned c,unsigned revision){
    x=std::clamp(x,0,191);y=std::clamp(y,0,159);
    return 20+((unsigned(x*3+y*7)+eye*47+c*23+revision*19)%210);
}
void check_encoder_handoff(ID3D11Device* capture, ID3D11DeviceContext* context) {
    ComPtr<ID3D11Device> encoder;
    ComPtr<ID3D11DeviceContext> encoderContext;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &encoder, nullptr, &encoderContext)), "encoder_device");
    ComPtr<ID3D11Texture2D> source;
    source.Attach(d3d_render_utils::CreateTexture(capture, 32, 16, DXGI_FORMAT_R8G8B8A8_UNORM));
    EncoderFrameMailbox mailbox(capture, encoder.Get(), source.Get());
    auto submit = [&](unsigned value, uint64_t timestamp) {
        auto slot = mailbox.TryBeginCapture();
        require(slot.has_value(), "capture_slot_required");
        std::vector<unsigned char> pixels(32 * 16 * 4, static_cast<unsigned char>(value));
        context->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), 32 * 4, 0);
        mailbox.EndCapture(*slot, source.Get(), timestamp + 1000, timestamp);
    };
    auto receive = [&] {
        EncoderFrameMailbox::Frame frame{};
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!mailbox.TryBeginEncode(frame)) {
            require(std::chrono::steady_clock::now() < end, "gpu_handoff_timeout");
            std::this_thread::yield();
        }
        return frame;
    };
    auto check = [&](const EncoderFrameMailbox::Frame& frame, unsigned value) {
        D3D11_TEXTURE2D_DESC desc{};
        frame.texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        require(SUCCEEDED(encoder->CreateTexture2D(&desc, nullptr, &readback)), "mailbox_readback");
        encoderContext->CopyResource(readback.Get(), frame.texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(encoderContext->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "mailbox_map");
        for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 32 * 4; ++x)
            require(static_cast<unsigned char*>(mapped.pData)[y * mapped.RowPitch + x] == value,
                "immutable_mailbox_pixels");
        encoderContext->Unmap(readback.Get(), 0);
    };
    // Keep one texture leased to a stalled encoder. Capture can fill both other
    // slots and reject the fourth image without waiting or altering the lease.
    submit(21, 101);
    auto held = receive();
    require(held.targetTimestampNs == 101 && held.presentationTime == 1101, "first_metadata");
    submit(43, 202);
    submit(65, 303);
    require(!mailbox.TryBeginCapture().has_value(), "bounded_mailbox_during_encoder_stall");
    check(held, 21);
    mailbox.EndEncode(held.slot);
    auto latest = receive();
    require(latest.targetTimestampNs == 303 && latest.presentationTime == 1303, "latest_exact_metadata");
    check(latest, 65);
    mailbox.EndEncode(latest.slot);
    require(mailbox.TakeDiscarded() == 1, "only_unencoded_old_image_discarded");
    auto cancel = mailbox.TryBeginCapture();
    require(cancel.has_value(), "cancel_slot");
    mailbox.CancelCapture(*cancel);
    submit(87, 404);
    auto recycled = receive();
    require(recycled.targetTimestampNs == 404, "recycled_metadata");
    check(recycled, 87);
    mailbox.EndEncode(recycled.slot);
}
}
const Settings* Settings_Instance(){return &settings;}
extern "C" FfiFrameGazeCrops GetFrameGazeCrops(unsigned long long){return crops;}
Exception MakeException(const char*,...){return Exception("D3D11 gaze fixture failed");}
void Debug(const char*,...){}
void Info(const char*,...){}
int main(int argc,char** argv){try {
    if (argc == 4 && std::string(argv[1]) == "--frame-worker-fixture")
        return frame_worker_fixture(std::stoull(argv[2]), std::stoull(argv[3]));
    require(argc==2,"quad_shader_path_required");
    std::ifstream file(argv[1],std::ios::binary);std::vector<unsigned char> quad{std::istreambuf_iterator<char>(file),{}};
    require(!quad.empty(),"quad_shader_missing");QUAD_SHADER_CSO_PTR=quad.data();QUAD_SHADER_CSO_LEN=unsigned(quad.size());
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"warp_device");
    settings.m_renderWidth=384;settings.m_renderHeight=160;settings.m_frameGaze={true,64,64,32,32};
    ComPtr<ID3D11Texture2D> source=d3d_render_utils::CreateTexture(device.Get(),384,160,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    GazeAtlas atlas(device.Get(),source.Get());unsigned w=0,h=0;atlas.Resolution(&w,&h);require(w==128&&h==96,"atlas_resolution");
    D3D11_TEXTURE2D_DESC desc{};atlas.Texture()->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)),"readback_allocation");
    unsigned checked=0,maximum=0;
    for(unsigned revision=0;revision<3;++revision){
        crops={{{revision*40,revision*32},{160-revision*40,128-revision*32}}};
        std::vector<unsigned char> pixels(384*160*4);
        for(unsigned eye=0;eye<2;++eye)for(unsigned y=0;y<160;++y)for(unsigned x=0;x<192;++x){auto* p=pixels.data()+(y*384+eye*192+x)*4;
            for(unsigned c=0;c<3;++c)p[c]=static_cast<unsigned char>(code(eye,x,y,c,revision));p[3]=255;}
        ID3D11ShaderResourceView* nullViews[2]{};context->PSSetShaderResources(0,2,nullViews);
        context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),384*4,0);atlas.Render(revision);
        context->CopyResource(readback.Get(),atlas.Texture());D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"readback_map");
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<4;++c){
            const unsigned eye=x/64,local=x%64;unsigned expected=255;
            if(c<3){
                if(y>=64)expected=local>=32?0:code(eye,crops.topLeft[eye][0]+local,crops.topLeft[eye][1]+y-64,c,revision);
                else {
                    const double sx=std::clamp(original((local+.5)/64)*192-.5,0.,191.),sy=std::clamp(original((y+.5)/64)*160-.5,0.,159.);
                    const int ix=int(sx),iy=int(sy);const double fx=sx-ix,fy=sy-iy;double value=0;
                    for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)value+=(dx?fx:1-fx)*(dy?fy:1-fy)*linear(code(eye,ix+dx,iy+dy,c,revision));
                    expected=srgb(value);
                }
            }
            const auto actual=static_cast<unsigned char*>(mapped.pData)[y*mapped.RowPitch+x*4+c];
            const unsigned error=unsigned(std::abs(int(actual)-int(expected)));maximum=std::max(maximum,error);++checked;
            require(error<=3,"atlas_pixel_mapping_seam_crop_or_gamma_error");
        }
        context->Unmap(readback.Get(),0);
    }
    check_encoder_handoff(device.Get(), context.Get());
    check_pose_history();
    check_process_handoff(device.Get(), context.Get());
    std::cout<<"{\"d3d11_gaze_atlas\":\"passed\",\"independent_encoder_handoff\":\"passed\",\"process_handoff_and_teardown\":\"passed\",\"warp\":true,\"pixel_channel_checks\":"<<checked<<",\"max_code_error\":"<<maximum<<",\"headset\":false}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
