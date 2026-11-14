#include "capture.hpp"
#include "../color_bars.hpp"
extern "C" {
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace surfxr::transport::desktop {
namespace {
using namespace surfxr::tests::color_bars;
struct Decoder {
    AVCodecContext* context=nullptr;
    AVFrame* frame=av_frame_alloc();
    AVPacket* packet=av_packet_alloc();
    SwsContext* scaler=nullptr;
    ~Decoder(){sws_freeContext(scaler);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&context);}
};
void verify(const Bytes& configuration,const std::vector<Bytes>& packets){
    Decoder decoder;
    decoder.context=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));
    require(decoder.context&&decoder.frame&&decoder.packet,"desktop_color_decoder_allocation");
    decoder.context->extradata=static_cast<std::uint8_t*>(av_mallocz(configuration.size()+AV_INPUT_BUFFER_PADDING_SIZE));
    require(decoder.context->extradata,"desktop_color_configuration_allocation");
    std::memcpy(decoder.context->extradata,configuration.data(),configuration.size());
    decoder.context->extradata_size=static_cast<int>(configuration.size());
    require(avcodec_open2(decoder.context,decoder.context->codec,nullptr)>=0,"desktop_color_decoder_open");
    unsigned frames=0,max_error=0;
    const auto receive=[&]{
        int status;
        while((status=avcodec_receive_frame(decoder.context,decoder.frame))==0){
            auto& frame=*decoder.frame;
            require(frame.width==int(width)&&frame.height==int(height),"desktop_color_decoded_extent");
            // FFmpeg's packed RGB NVENC path signals its fixed BT.601 limited conversion.
            require(frame.colorspace==AVCOL_SPC_BT470BG&&frame.color_range==AVCOL_RANGE_MPEG,
                "desktop_color_unexpected_bitstream_matrix_or_range");
            require(frame.color_trc==AVCOL_TRC_IEC61966_2_1,"desktop_color_transfer_not_srgb");
            decoder.scaler=sws_getCachedContext(decoder.scaler,width,height,AVPixelFormat(frame.format),
                width,height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            require(decoder.scaler,"desktop_color_scaler_allocation");
            const auto* coefficients=sws_getCoefficients(SWS_CS_ITU601);
            require(sws_setColorspaceDetails(decoder.scaler,coefficients,0,coefficients,1,0,1<<16,1<<16)>=0,
                "desktop_color_scaler_configuration");
            std::vector<std::uint8_t> rgba(width*height*4);
            std::uint8_t* planes[]{rgba.data(),nullptr,nullptr,nullptr};
            int strides[]{int(width*4),0,0,0};
            require(sws_scale(decoder.scaler,frame.data,frame.linesize,0,height,planes,strides)==int(height),
                "desktop_color_conversion_rows");
            std::cout<<"{\"desktop_hevc_frame\":"<<frames<<",\"matrix\":"<<frame.colorspace
                <<",\"range\":"<<frame.color_range<<",\"transfer\":"<<frame.color_trc<<",\"patches\":[";
            for(unsigned bar=0;bar<colors.size();++bar){
                if(bar)std::cout<<',';std::cout<<'[';
                for(unsigned channel=0;channel<3;++channel){
                    unsigned sum=0;
                    for(unsigned y=height/2-4;y<height/2+4;++y)
                        for(unsigned x=bar*bar_width+bar_width/2-4;x<bar*bar_width+bar_width/2+4;++x)
                            sum+=rgba[(y*width+x)*4+channel];
                    const unsigned value=(sum+32)/64;
                    const auto error=unsigned(std::abs(int(value)-int(colors[bar][channel])));
                    max_error=std::max(max_error,error);
                    if(channel)std::cout<<',';std::cout<<value;
                }
                std::cout<<']';
            }
            std::cout<<"]}\n";++frames;av_frame_unref(decoder.frame);
        }
        require(status==AVERROR(EAGAIN)||status==AVERROR_EOF,"desktop_color_decoder_receive");
    };
    for(const auto& bytes:packets){
        require(av_new_packet(decoder.packet,static_cast<int>(bytes.size()))>=0,"desktop_color_packet_allocation");
        std::memcpy(decoder.packet->data,bytes.data(),bytes.size());
        require(avcodec_send_packet(decoder.context,decoder.packet)>=0,"desktop_color_decoder_send");
        av_packet_unref(decoder.packet);receive();
    }
    require(avcodec_send_packet(decoder.context,nullptr)>=0,"desktop_color_decoder_flush");receive();
    const bool passed=frames==4&&max_error<=6;
    std::cout<<"{\"desktop_color_passed\":"<<(passed?"true":"false")<<",\"frames\":"<<frames
        <<",\"max_code_error\":"<<max_error<<",\"limit\":6,\"synthetic_only\":true,\"network_used\":false}\n";
    require(passed,"desktop_color_pixels_mismatch");
}
}
void desktop_color_self_test(unsigned monitor,const std::string& output_directory){
    Capture capture(monitor,true,width,height);
    std::vector<std::uint8_t> bgra(width*height*4);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const auto& color=colors[x/bar_width];const auto offset=(y*width+x)*4;
        bgra[offset]=color[2];bgra[offset+1]=color[1];bgra[offset+2]=color[0];bgra[offset+3]=255;
    }
    capture.context()->UpdateSubresource(capture.texture(),0,nullptr,bgra.data(),width*4,0);
    Encoder encoder(capture,60,60);const auto configuration=encoder.configuration();
    std::vector<Bytes> packets;
    for(unsigned frame=0;frame<4;++frame){
        auto encoded=encoder.encode(capture,frame==0||frame==2,frame+1);
        require(!encoded.bytes.empty()&&((frame!=0&&frame!=2)||encoded.idr),"desktop_color_encoded_frame");
        packets.push_back(std::move(encoded.bytes));
    }
    if(!output_directory.empty()){
        std::filesystem::create_directories(output_directory);
        const auto path=std::filesystem::path(output_directory)/"desktop-bars.hevc";
        std::ofstream output(path,std::ios::binary);require(bool(output),"desktop_color_fixture_open");
        output.write(reinterpret_cast<const char*>(configuration.data()),configuration.size());
        for(const auto& packet:packets)output.write(reinterpret_cast<const char*>(packet.data()),packet.size());
        output.close();require(bool(output),"desktop_color_fixture_write");
    }
    verify(configuration,packets);
}
}
