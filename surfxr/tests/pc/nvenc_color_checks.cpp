// Offscreen production NVENC -> software decoder color contract check.
// Generates only synthetic bars under the configured repository build tree.
#include "platform/win32/VideoEncoderNVENC.h"
#include "platform/win32/VideoEncoderSW.h"
#include "surfxr_server/Logger.h"
#include "surfxr_server/Settings.h"
#include "SURFXR-common/packet_types.h"
#include "../color_bars.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <wrl/client.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace surfxr::tests::color_bars;
Settings settings{};
std::vector<std::vector<std::uint8_t>> packets;
bool reconfigure=false;
std::uint64_t requested_bitrate=60000000;
unsigned requested_fps=60;
bool measure_bitrate=false;
std::uint64_t encoded_bytes=0;
void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
void defaults(int codec){
    settings={};settings.m_codec=codec;settings.m_refreshRate=60;
    settings.m_nvencQualityPreset=1;settings.m_nvencTuningPreset=NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    settings.m_rateControlMode=SURFXR_CBR;settings.m_entropyCoding=SURFXR_CABAC;
    settings.m_nvencRefreshRate=settings.m_nvencIntraRefreshPeriod=settings.m_nvencIntraRefreshCount=-1;
    settings.m_nvencMaxNumRefFrames=settings.m_nvencGopLength=settings.m_nvencPFrameStrategy=-1;
    settings.m_nvencRateControlMode=settings.m_nvencRcBufferSize=settings.m_nvencRcInitialDelay=-1;
    settings.m_nvencRcMaxBitrate=settings.m_nvencRcAverageBitrate=settings.m_nvencLowDelayKeyFrameScale=-1;
    packets.clear();reconfigure=false;
    requested_bitrate=60000000;requested_fps=60;
}
struct Decoder {
    AVCodecContext* context=nullptr;
    AVFrame* frame=av_frame_alloc();
    AVPacket* packet=av_packet_alloc();
    SwsContext* scaler=nullptr;
    ~Decoder(){sws_freeContext(scaler);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&context);}
};
bool decode(int codec){
    Decoder d;d.context=avcodec_alloc_context3(avcodec_find_decoder(codec==SURFXR_CODEC_H264?AV_CODEC_ID_H264:AV_CODEC_ID_HEVC));
    require(d.context&&d.frame&&d.packet,"decoder_allocation");
    require(avcodec_open2(d.context,d.context->codec,nullptr)>=0,"decoder_open");
    unsigned frames=0,max_error=0;bool ok=true;
    auto receive=[&]{
        int result;
        while((result=avcodec_receive_frame(d.context,d.frame))==0){
            auto& f=*d.frame;require(f.width==int(width)&&f.height==int(height),"decoded_extent");
            const int matrix=f.colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_ITU601;
            require(f.colorspace==AVCOL_SPC_BT709||f.colorspace==AVCOL_SPC_BT470BG||f.colorspace==AVCOL_SPC_SMPTE170M,"missing_or_unsupported_matrix");
            require(f.color_range==AVCOL_RANGE_JPEG||f.color_range==AVCOL_RANGE_MPEG,"missing_range");
            require(f.color_trc==AVCOL_TRC_IEC61966_2_1&&f.color_primaries==AVCOL_PRI_BT709,"incorrect_sdr_transfer_or_primaries");
            d.scaler=sws_getCachedContext(d.scaler,width,height,AVPixelFormat(f.format),width,height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            require(d.scaler,"scaler_allocation");const auto* coefficients=sws_getCoefficients(matrix);
            require(sws_setColorspaceDetails(d.scaler,coefficients,f.color_range==AVCOL_RANGE_JPEG,coefficients,1,0,1<<16,1<<16)>=0,"scaler_colorspace");
            std::vector<std::uint8_t> rgba(width*height*4);std::uint8_t* target[]{rgba.data(),nullptr,nullptr,nullptr};int stride[]{int(width*4),0,0,0};
            require(sws_scale(d.scaler,f.data,f.linesize,0,height,target,stride)==int(height),"scale_rows");
            std::cout<<"{\"codec\":\""<<(codec==SURFXR_CODEC_H264?"h264":"hevc")<<"\",\"frame\":"<<frames
                <<",\"matrix\":"<<f.colorspace<<",\"range\":"<<f.color_range<<",\"transfer\":"<<f.color_trc<<",\"patches\":[";
            for(unsigned bar=0;bar<colors.size();++bar){if(bar)std::cout<<',';std::cout<<'[';
                for(unsigned c=0;c<3;++c){unsigned sum=0;
                    for(unsigned y=height/2-4;y<height/2+4;++y)for(unsigned x=bar*bar_width+bar_width/2-4;x<bar*bar_width+bar_width/2+4;++x)
                        sum+=rgba[(y*width+x)*4+c];
                    const unsigned value=(sum+32)/64,error=unsigned(std::abs(int(value)-colors[bar][c]));
                    max_error=std::max(max_error,error);if(c)std::cout<<',';std::cout<<value;
                }std::cout<<']';
            }
            std::cout<<"]}\n";++frames;av_frame_unref(d.frame);
        }
        require(result==AVERROR(EAGAIN)||result==AVERROR_EOF,"decode_receive");
    };
    for(const auto& bytes:packets){require(av_new_packet(d.packet,int(bytes.size()))==0,"packet_allocation");
        std::memcpy(d.packet->data,bytes.data(),bytes.size());require(avcodec_send_packet(d.context,d.packet)>=0,"decode_send");av_packet_unref(d.packet);receive();}
    require(avcodec_send_packet(d.context,nullptr)>=0,"decode_flush");receive();
    ok=frames==4&&max_error<=6;
    std::cout<<"{\"codec\":\""<<(codec==SURFXR_CODEC_H264?"h264":"hevc")<<"\",\"passed\":"<<(ok?"true":"false")
        <<",\"frames\":"<<frames<<",\"max_code_error\":"<<max_error<<",\"limit\":6}\n";
    return ok;
}
bool run(int codec,bool software=false){
    defaults(codec);auto render=std::make_shared<CD3DRender>();require(render->Initialize(0),"d3d_device");
    std::vector<std::uint8_t> rgba(width*height*4);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const auto& color=colors[x/bar_width];
        std::copy(color.begin(),color.end(),rgba.begin()+(y*width+x)*4);rgba[(y*width+x)*4+3]=255;}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba.data(),width*4,0};ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(render->GetDevice()->CreateTexture2D(&desc,&data,&texture)),"color_texture");
    auto encode=[&](VideoEncoder& encoder){encoder.Initialize();
        for(unsigned frame=0;frame<4;++frame){reconfigure=frame==2;encoder.Transmit(texture.Get(),frame,frame,frame==0||frame==2);}
        encoder.Shutdown();};
    if(software){VideoEncoderSW encoder(render,width,height);encode(encoder);}
    else {VideoEncoderNVENC encoder(render,width,height);encode(encoder);}
    texture.Reset();render->Shutdown();
    require(!packets.empty(),"no_encoded_packets");std::filesystem::create_directories(SURFXR_COLOR_OUTPUT);
    const auto name=software?"software-bars.h264":codec==SURFXR_CODEC_H264?"nvenc-bars.h264":"nvenc-bars.hevc";
    const auto path=std::filesystem::path(SURFXR_COLOR_OUTPUT)/name;
    std::ofstream out(path,std::ios::binary);require(bool(out),"fixture_output");
    for(const auto& packet:packets)out.write(reinterpret_cast<const char*>(packet.data()),packet.size());out.close();require(bool(out),"fixture_write");
    return decode(codec);
}
void bitrate_probe(){
    defaults(SURFXR_CODEC_HEVC);settings.m_refreshRate=120;requested_fps=120;
    settings.m_nvencTuningPreset=NV_ENC_TUNING_INFO_LOW_LATENCY;
    settings.m_nvencAdaptiveQuantizationMode=SpatialAQ;
    settings.m_nvencSplitEncodeMode=NV_ENC_SPLIT_THREE_FORCED_MODE;
    measure_bitrate=true;
    auto render=std::make_shared<CD3DRender>();require(render->Initialize(0),"d3d_device");
    constexpr unsigned w=1920,h=1080,count=90,warmup=30;
    std::vector<std::uint32_t> pixels(w*h);
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(render->GetDevice()->CreateTexture2D(&desc,nullptr,&texture)),"bitrate_texture");
    std::uint32_t rng=0x53757266;std::uint64_t frame=0;
    {
        VideoEncoderNVENC encoder(render,w,h);encoder.Initialize();
        for(unsigned mbps:{100u,200u,300u,400u,100u,300u}){
            requested_bitrate=std::uint64_t(mbps)*1000000;
            std::uint64_t bytes=0;
            for(unsigned i=0;i<count;++i,++frame){
                // Moving, high-entropy synthetic input makes the bitrate budget
                // meaningful without recording a user's desktop or game.
                for(auto& pixel:pixels){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;pixel=rng|0xff000000u;}
                render->GetContext()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),w*4,0);
                reconfigure=i==0;encoded_bytes=0;
                encoder.Transmit(texture.Get(),frame,frame,frame==0);
                if(i>=warmup)bytes+=encoded_bytes;
            }
            const double actual=double(bytes)*8*requested_fps/(count-warmup)/1000000;
            std::cout<<"{\"requested_mbps\":"<<mbps<<",\"actual_mbps\":"<<actual
                <<",\"filler\":false,\"network_used\":false}\n";
            require(actual>mbps*.75&&actual<mbps*1.25,"dynamic_bitrate_outside_budget");
        }
        // CBR without filler is a content budget, not a promise to generate
        // padding. A stable image must remain well below the same 300 Mbps.
        std::fill(pixels.begin(),pixels.end(),0xff807060u);
        render->GetContext()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),w*4,0);
        std::uint64_t bytes=0;
        for(unsigned i=0;i<count;++i,++frame){
            encoded_bytes=0;encoder.Transmit(texture.Get(),frame,frame,i==0);
            if(i>=warmup)bytes+=encoded_bytes;
        }
        const double actual=double(bytes)*8*requested_fps/(count-warmup)/1000000;
        std::cout<<"{\"requested_mbps\":300,\"actual_mbps\":"<<actual
            <<",\"content\":\"static\",\"filler\":false,\"network_used\":false}\n";
        require(actual<150,"static_content_unexpected_padding");
        encoder.Shutdown();
    }
    texture.Reset();render->Shutdown();measure_bitrate=false;
}
}
const Settings* Settings_Instance(){return &settings;}
extern "C" FfiDynamicEncoderParams GetDynamicEncoderParams(){
    const bool updated=reconfigure;reconfigure=false;return {updated?1u:0u,requested_bitrate,static_cast<float>(requested_fps)};
}
void ParseFrameNals(int,unsigned char* data,int length,unsigned long long,bool){
    if(measure_bitrate)encoded_bytes+=length;else packets.emplace_back(data,data+length);
}
Exception MakeException(const char* format,...){char text[2048];va_list args;va_start(args,format);std::vsnprintf(text,sizeof(text),format,args);va_end(args);return Exception(text);}
void Debug(const char*,...){}
void Info(const char*,...){}
void Warn(const char*,...){}
void Error(const char*,...){}
void frame_render_color_probe();
void frame_render_pose_probe(Settings& settings);
void nvenc_quality_probe(Settings&,const std::function<void(std::uint64_t,unsigned)>&,
                         std::vector<std::vector<std::uint8_t>>&);
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--quality-probe"){
        defaults(SURFXR_CODEC_HEVC);
        nvenc_quality_probe(settings,[](std::uint64_t b,unsigned fps){requested_bitrate=b;requested_fps=fps;reconfigure=true;},packets);
        return 0;
    }
    if(argc==2&&std::string(argv[1])=="--bitrate-probe"){bitrate_probe();return 0;}
    if(argc==2&&std::string(argv[1])=="--render-color-probe"){frame_render_color_probe();return 0;}
    if(argc==2&&std::string(argv[1])=="--render-pose-probe"){frame_render_pose_probe(settings);return 0;}
    require(argc==2,"use_--gpu-test_--bitrate-probe_--software-test_--render-color-probe_or_--render-pose-probe");
    if(std::string(argv[1])=="--software-test"){
        const bool passed=run(SURFXR_CODEC_H264,true);
        std::cout<<"{\"production_software_encoder\":true,\"d3d_staging_used\":true,\"network_used\":false,\"passed\":"<<(passed?"true":"false")<<"}\n";
        return passed?0:1;
    }
    require(std::string(argv[1])=="--gpu-test","use_--gpu-test_for_offscreen_hardware_encoding");
    const bool h264=run(SURFXR_CODEC_H264),hevc=run(SURFXR_CODEC_HEVC);
    std::cout<<"{\"production_nvenc\":true,\"hardware_used\":true,\"network_used\":false,\"reconfigure_checked\":true,\"passed\":"<<(h264&&hevc?"true":"false")<<"}\n";
    return h264&&hevc?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
