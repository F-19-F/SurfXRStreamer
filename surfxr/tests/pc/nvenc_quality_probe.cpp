// Production NVENC comparison on identical synthetic grayscale detail/motion.
// No capture, network, live settings changes or extra encoder frame buffering.
#include "platform/win32/VideoEncoderNVENC.h"
#include "surfxr_server/Settings.h"
#include "SURFXR-common/packet_types.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned width=3840,height=2160,fps=120,count=72,warmup=24;
void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
unsigned sample(unsigned x,unsigned y,unsigned frame,bool changing){
    const unsigned u=x+frame*3,v=y+frame;
    auto hash=[](unsigned a){a^=a>>16;a*=0x7feb352du;a^=a>>15;a*=0x846ca68bu;return a^(a>>16);};
    // Four regions: textured motion, fine line detail, smooth gradients and
    // glyph-like sharp edges. Deterministic for every preset/bitrate trial.
    switch((y/(height/2))*2+x/(width/2)){
    case 0:return 64+(hash(u+v*4099+(changing?frame*104729:0))%128);
    case 1:return ((u/3+v/5)%2)?208:48;
    case 2:return 32+(u/12+v/16)%192;
    default:return ((u%23<3||v%29<3)&&((u/23+v/29)%3))?216:40;
    }
}
struct Decoder {
    AVCodecContext* context=nullptr;
    AVFrame* frame=av_frame_alloc();
    AVPacket* packet=av_packet_alloc();
    Decoder(){
        const auto* codec=avcodec_find_decoder(AV_CODEC_ID_HEVC);
        context=avcodec_alloc_context3(codec);require(context&&frame&&packet,"quality_decoder_alloc");
        context->thread_count=1;
        require(avcodec_open2(context,codec,nullptr)>=0,"quality_decoder_open");
    }
    ~Decoder(){av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&context);}
};
struct Result {double mse=0,detail_mse=0,mbps=0,mean_ms=0,p95_ms=0;};
Result trial(Settings& settings,const std::function<void(std::uint64_t,unsigned)>& request,
             std::vector<std::vector<std::uint8_t>>& packets,unsigned preset,unsigned passes,unsigned mbps,bool changing){
    settings.m_nvencQualityPreset=preset;settings.m_nvencMultiPass=passes;
    settings.m_refreshRate=fps;settings.m_nvencTuningPreset=NV_ENC_TUNING_INFO_LOW_LATENCY;
    settings.m_nvencAdaptiveQuantizationMode=SpatialAQ;
    settings.m_nvencSplitEncodeMode=NV_ENC_SPLIT_THREE_FORCED_MODE;
    settings.m_fillerData=false;
    auto render=std::make_shared<CD3DRender>();require(render->Initialize(0),"quality_d3d");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(render->GetDevice()->CreateTexture2D(&desc,nullptr,&texture)),"quality_texture");
    std::vector<std::uint32_t> pixels(width*height);Decoder decoder;
    std::uint64_t bytes=0,errors=0,detail_errors=0,samples=0,detail_samples=0;unsigned decoded=0;
    std::vector<double> elapsed;Result result;
    {
        VideoEncoderNVENC encoder(render,width,height);encoder.Initialize();request(std::uint64_t(mbps)*1000000,fps);
        for(unsigned i=0;i<count;++i){
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
                const auto gray=sample(x,y,i,changing);pixels[y*width+x]=0xff000000u|gray|(gray<<8)|(gray<<16);
            }
            render->GetContext()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),width*4,0);
            packets.clear();const auto start=std::chrono::steady_clock::now();
            encoder.Transmit(texture.Get(),i,i,i==0);
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(i>=warmup)elapsed.push_back(ms);
            for(const auto& packet:packets){
                if(i>=warmup)bytes+=packet.size();
                require(av_new_packet(decoder.packet,int(packet.size()))==0,"quality_packet");
                std::copy(packet.begin(),packet.end(),decoder.packet->data);
                require(avcodec_send_packet(decoder.context,decoder.packet)>=0,"quality_decode_send");av_packet_unref(decoder.packet);
                int status;
                while((status=avcodec_receive_frame(decoder.context,decoder.frame))==0){
                    auto& frame=*decoder.frame;
                    require(frame.width==width&&frame.height==height&&
                        (frame.format==AV_PIX_FMT_YUV420P||frame.format==AV_PIX_FMT_YUVJ420P)&&
                        frame.color_range==AVCOL_RANGE_JPEG,"quality_decode_format");
                    require(decoded==i,"quality_zero_delay_source_association");
                    if(i>=warmup){
                        for(unsigned y=0;y<height;y+=2)for(unsigned x=0;x<width;x+=2){
                            const int diff=int(frame.data[0][y*frame.linesize[0]+x])-int(sample(x,y,decoded,changing));
                            errors+=diff*diff;++samples;
                            if(y<height/2){detail_errors+=diff*diff;++detail_samples;}
                        }
                    }
                    ++decoded;av_frame_unref(decoder.frame);
                }
                require(status==AVERROR(EAGAIN),"quality_decode_receive");
            }
        }
        encoder.Shutdown();
    }
    require(decoded==count&&samples&&detail_samples,"quality_frame_count");
    texture.Reset();render->Shutdown();
    result.mse=double(errors)/samples;result.detail_mse=double(detail_errors)/detail_samples;
    result.mbps=double(bytes)*8*fps/(count-warmup)/1e6;
    for(auto ms:elapsed)result.mean_ms+=ms/elapsed.size();
    std::sort(elapsed.begin(),elapsed.end());result.p95_ms=elapsed[elapsed.size()*95/100];
    std::cout<<"{\"scene\":\""<<(changing?"changing-texture":"translated-detail")<<"\",\"preset\":\"P"<<preset<<"\",\"passes\":"<<passes<<",\"target_mbps\":"<<mbps
        <<",\"actual_mbps\":"<<result.mbps<<",\"luma_psnr_db\":"<<10*std::log10(255.*255/result.mse)
        <<",\"detail_mse\":"<<result.detail_mse<<",\"mean_encode_ms\":"<<result.mean_ms
        <<",\"p95_encode_ms\":"<<result.p95_ms<<",\"filler\":false}\n"<<std::flush;
    return result;
}
}
void nvenc_quality_probe(Settings& settings,const std::function<void(std::uint64_t,unsigned)>& request,
                         std::vector<std::vector<std::uint8_t>>& packets){
    for(bool changing:{false,true})for(unsigned mbps:{200u,300u}){
        trial(settings,request,packets,1,0,mbps,changing);
        trial(settings,request,packets,1,1,mbps,changing);
        trial(settings,request,packets,1,2,mbps,changing);
        trial(settings,request,packets,3,0,mbps,changing);
        trial(settings,request,packets,3,2,mbps,changing);
        trial(settings,request,packets,5,0,mbps,changing);
    }
}
