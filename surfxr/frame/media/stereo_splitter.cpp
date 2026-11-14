#include "surfxr/stereo_splitter.hpp"
#include "surfxr/vr_wire.hpp"
#include <cstring>
#include <stdexcept>

namespace surfxr {
std::array<std::shared_ptr<const VideoFrame>,4> StereoSplitter::split_gaze(std::shared_ptr<const VideoFrame> source){
    if(!source||!source->vr_metadata||!source->vr_metadata->gaze_atlas)throw std::runtime_error("invalid_gaze_atlas");
    const auto a=source->vr_metadata->gaze_atlas;
    if(source->packed_source||source->width!=2*a.base_width||source->height!=a.base_height+a.focus_height||
       !a.focus_width||a.focus_width>a.base_width||source->stride<std::uint64_t{source->width}*4||
       (source->nv12_storage?(!valid_nv12_storage(*source->nv12_storage)||source->nv12_storage->width!=source->width||source->nv12_storage->height!=source->height):
        (source->rgba_size()!=std::uint64_t{source->stride}*source->height||!source->rgba_data())))throw std::runtime_error("invalid_gaze_atlas");
    std::array<std::shared_ptr<const VideoFrame>,4> output;
    for(unsigned id=0;id<4;++id){auto f=std::make_shared<VideoFrame>();
        f->width=id%2?a.focus_width:a.base_width;f->height=id%2?a.focus_height:a.base_height;f->stride=f->width*4;
        f->generation=source->generation;f->source_timestamp=source->source_timestamp;f->output_epoch=source->output_epoch;
        f->decoded_monotonic_ns=source->decoded_monotonic_ns;f->packet_submit_monotonic_ns=source->packet_submit_monotonic_ns;
        f->conversion_begin_monotonic_ns=source->conversion_begin_monotonic_ns;f->decoder_output_monotonic_ns=source->decoder_output_monotonic_ns;
        f->has_source_timestamp=source->has_source_timestamp;f->vr_metadata=source->vr_metadata;f->yuv_coefficients=source->yuv_coefficients;
        f->packed_source=source;f->packed_region=true;f->packed_x=(id/2)*a.base_width;f->packed_y=id%2?a.base_height:0;
        if(!f->valid_packed_region())throw std::runtime_error("invalid_gaze_atlas");
        output[id]=std::move(f);
    }
    return output;
}
std::array<std::shared_ptr<const VideoFrame>,2> StereoSplitter::split(std::shared_ptr<const VideoFrame> source){
    if(!source)throw std::runtime_error("invalid_packed_stereo_frame");
    if(!source->rgba_storage&&!source->nv12_storage)return split(*source);
    if(source->packed_source||!source->width||source->width%2||source->width>8192||!source->height||source->height>8192||
       source->stride!=std::uint64_t{source->width}*4||
       (source->nv12_storage?(!valid_nv12_storage(*source->nv12_storage)||source->nv12_storage->width!=source->width||source->nv12_storage->height!=source->height):
        (source->rgba_size()!=std::uint64_t{source->stride}*source->height||!source->rgba_data())))
        throw std::runtime_error("invalid_packed_stereo_frame");
    std::array<std::shared_ptr<const VideoFrame>,2> eyes;
    for(unsigned eye=0;eye<2;++eye){
        auto f=std::make_shared<VideoFrame>();
        f->width=source->width/2;f->height=source->height;f->stride=f->width*4;
        f->generation=source->generation;f->source_timestamp=source->source_timestamp;f->output_epoch=source->output_epoch;
        f->decoded_monotonic_ns=source->decoded_monotonic_ns;f->packet_submit_monotonic_ns=source->packet_submit_monotonic_ns;
        f->conversion_begin_monotonic_ns=source->conversion_begin_monotonic_ns;f->decoder_output_monotonic_ns=source->decoder_output_monotonic_ns;
        f->has_source_timestamp=source->has_source_timestamp;f->vr_metadata=source->vr_metadata;
        f->yuv_coefficients=source->yuv_coefficients;
        f->packed_source=source;f->packed_eye=eye;eyes[eye]=std::move(f);
    }
    return eyes;
}
std::array<std::shared_ptr<const VideoFrame>,2> StereoSplitter::split(const VideoFrame& source){
    const auto width=source.width/2,stride=width*4;
    if(!width||source.width%2||source.width>8192||!source.height||source.height>8192||
       source.stride<std::uint64_t{source.width}*4||source.rgba_size()!=std::uint64_t{source.stride}*source.height||!source.rgba_data())
        throw std::runtime_error("invalid_packed_stereo_frame");
    const auto size=std::size_t{stride}*source.height;
    std::array<std::shared_ptr<VideoFrame>,2> output{};
    // Acquire both eyes before writing either; pool pressure drops a complete
    // decoded pair without changing its generation or encoded reference chain.
    for(unsigned eye=0;eye<2;++eye){
        for(auto& entry:pool_[eye])if(!entry||entry.use_count()==1){
            if(!entry)entry=std::make_shared<VideoFrame>();
            output[eye]=entry;break;
        }
        if(!output[eye])return {};
    }
    for(unsigned eye=0;eye<2;++eye){auto& f=*output[eye];
        if(f.rgba_size()!=size){f.rgba.clear();f.rgba_storage=allocator_?allocator_(size):nullptr;
            if(f.rgba_storage&&f.rgba_storage->size()!=size)throw std::runtime_error("invalid_stereo_allocation");
            if(!f.rgba_storage)f.rgba.resize(size);
        }
        f.width=width;f.height=source.height;f.stride=stride;
        f.generation=source.generation;f.source_timestamp=source.source_timestamp;f.output_epoch=source.output_epoch;
        f.decoded_monotonic_ns=source.decoded_monotonic_ns;f.packet_submit_monotonic_ns=source.packet_submit_monotonic_ns;
        f.conversion_begin_monotonic_ns=source.conversion_begin_monotonic_ns;f.decoder_output_monotonic_ns=source.decoder_output_monotonic_ns;
        f.has_source_timestamp=source.has_source_timestamp;f.vr_metadata=source.vr_metadata;
        for(unsigned row=0;row<f.height;++row)std::memcpy(f.rgba_data()+std::size_t{row}*stride,
            source.rgba_data()+std::size_t{row}*source.stride+eye*stride,stride);
    }
    return {output[0],output[1]};
}
}
