#pragma once
#include "surfxr/video_decoder.hpp"
#include <libavutil/frame.h>
#include <algorithm>
#include <cstring>

namespace surfxr::decoder_detail {
// Six full packed NV12 images fit the existing 256 MiB presentation budget.
// Pool exhaustion skips a whole output without allocation or codec disruption.
class Nv12FramePool {
    Nv12FrameAllocator allocator_;
    std::array<std::shared_ptr<VideoFrame>,6> frames_{};
    unsigned width_=0,height_=0;std::size_t bytes_=0;
    bool supported_=true;
public:
    explicit Nv12FramePool(Nv12FrameAllocator allocator={}):allocator_(std::move(allocator)){}
    bool enabled()const noexcept{return bool(allocator_)&&supported_;}
    const Nv12FrameAllocator& allocator()const noexcept{return allocator_;}
    void clear(DecoderStats& stats){frames_={};bytes_=0;width_=height_=0;supported_=true;stats.nv12_pool_bytes=0;}
    std::shared_ptr<VideoFrame> acquire(unsigned width,unsigned height,DecoderStats& stats){
        if(width_!=width||height_!=height){frames_={};bytes_=0;stats.nv12_pool_bytes=0;width_=width;height_=height;}
        for(auto& f:frames_)if(!f||f.use_count()==1){
            if(!f){
                auto storage=allocator_?allocator_(width,height):nullptr;if(!storage){supported_=false;return {};}
                if(!valid_nv12_storage(*storage)||storage->width!=width||storage->height!=height||!storage->data(0)||!storage->data(1))
                    throw VideoDecodeError("nv12_allocator_invalid_storage");
                const auto bytes=storage->bytes[0]+storage->bytes[1];if(bytes>256u*1024u*1024u-bytes_)return {};
                f=std::make_shared<VideoFrame>();f->nv12_storage=std::move(storage);bytes_+=bytes;stats.nv12_pool_bytes=bytes_;
            }
            f->width=width;f->height=height;f->stride=width*4;f->packed_source.reset();f->vr_metadata.reset();
            f->has_source_timestamp=false;f->source_timestamp=f->packet_submit_monotonic_ns=0;return f;
        }
        return {};
    }
};
inline std::array<float,4> nv12_coefficients(AVColorSpace space,AVColorRange range){
    float kr=0,kb=0;
    if(space==AVCOL_SPC_BT709){kr=.2126f;kb=.0722f;}
    else if(space==AVCOL_SPC_SMPTE170M||space==AVCOL_SPC_BT470BG){kr=.299f;kb=.114f;}
    else if(space==AVCOL_SPC_BT2020_NCL){kr=.2627f;kb=.0593f;}
    if(!kr||(range!=AVCOL_RANGE_JPEG&&range!=AVCOL_RANGE_MPEG))return {};
    return {kr,kb,range==AVCOL_RANGE_MPEG?16.f/255.f:0.f,range==AVCOL_RANGE_MPEG?255.f/219.f:1.f};
}
using Nv12PlaneCopy=void(*)(std::uint8_t*,const std::uint8_t*,std::size_t);
inline void copy_nv12_plane(std::uint8_t* target,const std::uint8_t* source,std::size_t bytes){
    std::memcpy(target,source,bytes);
}
inline void write_nv12(const AVFrame& source,Nv12FrameStorage& destination,Nv12PlaneCopy copy=copy_nv12_plane){
    if(source.format!=AV_PIX_FMT_NV12||source.width!=int(destination.width)||source.height!=int(destination.height)||
       !valid_nv12_storage(destination)||!destination.data(0)||!destination.data(1)||!source.data[0]||!source.data[1]||
       source.linesize[0]<source.width||source.linesize[1]<source.width)
        throw VideoDecodeError("nv12_output_layout_mismatch");
    for(unsigned plane=0;plane<2;++plane){
        const unsigned rows=plane?destination.height/2:destination.height;
        if(source.linesize[plane]==int(destination.width)&&destination.stride[plane]==destination.width)
            copy(destination.data(plane),source.data[plane],std::size_t{destination.width}*rows);
        else for(unsigned row=0;row<rows;++row)
            copy(destination.data(plane)+std::size_t{row}*destination.stride[plane],
                source.data[plane]+std::size_t{row}*source.linesize[plane],destination.width);
    }
}
}
