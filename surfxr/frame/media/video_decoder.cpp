#include "decoder_private.hpp"

namespace surfxr {
namespace decoder_detail {
const char* v4l2_decoder_name(std::uint32_t code){
    switch(code){
    case 1:return "h264_v4l2m2m";
    case 2:return "hevc_v4l2m2m";
    default:throw VideoDecodeError("v4l2_requires_h264_or_hevc8");
    }
}
const AVCodec* select_codec(std::uint32_t code,DecoderBackend backend){
    if(backend==DecoderBackend::V4l2){
        const auto* codec=avcodec_find_decoder_by_name(v4l2_decoder_name(code));
        require(codec!=nullptr,"v4l2_decoder_unavailable");return codec;
    }
    const char* name=nullptr;
    switch(code){
        case 1:name="h264";break;
        case 2:name="hevc";break;
        case 3:name="vp8";break;
        case 4:name="vp9";break;
        case 10:{
            for(const auto* candidate:{"libdav1d","libaom-av1","av1"})
                if(const auto* codec=avcodec_find_decoder_by_name(candidate))return codec;
            throw VideoDecodeError("software_av1_decoder_unavailable");
        }
        default:throw VideoDecodeError("unsupported_or_automatic_codec");
    }
    const auto* codec=avcodec_find_decoder_by_name(name);
    require(codec!=nullptr,"software_decoder_unavailable");return codec;
}

} // namespace decoder_detail
const char* decoder_backend_name(DecoderBackend backend) noexcept{
    switch(backend){case DecoderBackend::Software:return "software";case DecoderBackend::V4l2:return "v4l2";}
    return "invalid";
}
VideoDecoder::VideoDecoder(LatestFrameSlot& output,DecoderBackend backend,FrameSink sink,unsigned rgba_threads,RgbaFrameAllocator allocator,FrameDemand demand,DecoderFrameLayout layout,Nv12FrameAllocator nv12_allocator):
    impl_(new Impl(output,backend,std::move(sink),rgba_threads,std::move(allocator),std::move(demand),layout,std::move(nv12_allocator))){}
VideoDecoder::~VideoDecoder()=default;
void VideoDecoder::configure(std::uint32_t code,std::uint32_t width,std::uint32_t height,bool restart){
    auto& s=*impl_;
    s.guarded([&]{
        require(!s.finished,"decoder_already_finished");
        require(width>0 && height>0 && width<=8192 && height<=8192 && std::int64_t{width}*height<=s.pixel_limit,"configured_dimensions_exceed_policy");
        const bool same=code==s.config_codec&&width==s.config_width&&height==s.config_height;
        if((s.codec||s.hevc) && same && !restart)return;
        if(!s.hardware_recovery_pending)s.busy_recoveries_in_configuration=0;
        if(width!=s.config_width||height!=s.config_height){s.rgba_pool.clear(s.stats);s.nv12_pool.clear(s.stats);}
        const auto* decoder=select_codec(code,s.stats.backend);
        if(!s.frame)s.frame.reset(av_frame_alloc());
        if(!s.packet)s.packet.reset(av_packet_alloc());
        require(s.frame && s.packet,"decoder_allocation_failed");
        if(s.stats.backend==DecoderBackend::V4l2){
            require(width>=128 && height>=128,"v4l2_dimensions_below_128");
            // STREAMOFF closes the old session before opening another iris owner.
            // Published DMA-BUFs retain their backing allocation through GPU completion.
            av_frame_unref(s.frame.get());av_packet_unref(s.packet.get());
            s.codec.reset();s.hevc.reset();if(!restart||!same)s.output.clear();
        }
        s.stats.selected_decoder=decoder->name;s.stats.pixel_format="none";
        if(s.stats.backend==DecoderBackend::V4l2&&code==2){
            s.hevc=std::make_unique<V4l2Hevc>(width,height,s.pixel_limit>kMaxPixels);s.stats.selected_decoder="hevc_v4l2";
        }else{
            Codec next(avcodec_alloc_context3(decoder));require(bool(next),"codec_allocation_failed");
            next->width=static_cast<int>(width);next->height=static_cast<int>(height);
            next->max_pixels=s.pixel_limit;next->thread_count=2;next->thread_type=FF_THREAD_SLICE;
            next->err_recognition=AV_EF_EXPLODE;next->flags|=AV_CODEC_FLAG_COPY_OPAQUE;
            if(s.stats.backend==DecoderBackend::V4l2){
                next->coded_width=next->width;next->coded_height=next->height;
                next->pix_fmt=AV_PIX_FMT_NV12;
                // PTS is an ordinal in this client, not a server/OpenXR clock. A
                // nonzero timebase is required by the V4L2 timeval conversion.
                next->pkt_timebase={1,1000000};
                next->thread_count=1;next->thread_type=0;
            }
            ffcheck(avcodec_open2(next.get(),decoder,nullptr),s.stats.backend==DecoderBackend::V4l2?
                    "v4l2_decoder_open_failed":"software_decoder_open_failed");
            s.codec=std::move(next);
        }
        s.config_codec=code;s.config_width=width;s.config_height=height;
        s.source_stamps.clear();
        s.sequence.reset();s.h264_sequence.reset();s.h264_started=s.hevc_started=false;s.sequence_pending=s.keyframe_request=false;
        s.hardware_recovery_pending=false;
        s.decoder_eof=false;++s.stats.configuration_changes;
        av_frame_unref(s.frame.get());av_packet_unref(s.packet.get());if(!restart||!same)s.output.clear();
    });
}
void VideoDecoder::submit(const std::uint8_t* data,std::size_t size,std::uint64_t source_timestamp,
                          std::shared_ptr<const VrVideoMetadata> vr_metadata,std::uint64_t original_submit_monotonic_ns,
                          std::optional<std::uint64_t> original_output_epoch){
    auto& s=*impl_;
    const auto start=monotonic_ns();
    s.guarded([&]{
        require((s.codec||s.hevc||s.hardware_recovery_pending) && !s.finished,"decoder_not_configured_or_finished");
        require(data && size>0 && size<=kMaxPacket,"invalid_encoded_packet");
        const auto epoch=s.demand?(original_output_epoch?*original_output_epoch:s.demand()):0;
        VideoColorInfo color;
        std::vector<std::uint8_t> restarted_packet;
        if(s.stats.backend==DecoderBackend::V4l2&&s.config_codec==1){
            const auto inspection=s.h264_sequence.inspect_packet(data,size);
            require(inspection.valid,"invalid_h264_annexb_sequence");
            if(inspection.has_active_color)color=inspection.active_color;
            if(inspection.parameter_sets_changed){
                if(!s.sequence_pending)s.keyframe_request=true;
                s.sequence_pending=true;++s.stats.sequence_changes;
            }
            if(inspection.random_access&&!inspection.random_access_ready){
                if(!s.sequence_pending)s.keyframe_request=true;
                s.sequence_pending=true;
            }
            if(inspection.random_access_ready)
                require(inspection.width==s.config_width&&inspection.height==s.config_height,"h264_sps_configuration_dimensions_mismatch");
            if(!s.h264_started||s.sequence_pending){
                if(!inspection.random_access_ready){
                    if(!s.sequence_pending)s.keyframe_request=true;
                    s.sequence_pending=true;++s.stats.waiting_for_keyframe_packets;return;
                }
                // A fresh V4L2 DPB must start at an IDR with its exact SPS/PPS.
                // Cache standalone headers while waiting; do not submit an old
                // predicted picture merely because its dimensions still match.
                restarted_packet=s.h264_sequence.parameter_sets_annexb();
                require(restarted_packet.size()+size<=kMaxPacket,"h264_restart_packet_too_large");
                restarted_packet.insert(restarted_packet.end(),data,data+size);
                if(s.h264_started||s.hardware_recovery_pending){
                    auto next_sequence=std::move(s.h264_sequence);
                    configure(s.config_codec,s.config_width,s.config_height,true);
                    s.h264_sequence=std::move(next_sequence);++s.stats.sequence_restarts;
                }
                s.h264_started=true;s.sequence_pending=s.keyframe_request=false;
                data=restarted_packet.data();size=restarted_packet.size();
            }
        }else if(s.stats.backend==DecoderBackend::V4l2){
            const auto inspection=s.sequence.inspect_packet(data,size);
            require(inspection.valid,"invalid_hevc_annexb_sequence");
            if(inspection.has_active_color)color=inspection.active_color;
            if(inspection.parameter_sets_changed){
                if(!s.sequence_pending)s.keyframe_request=true;
                s.sequence_pending=true;++s.stats.sequence_changes;
            }
            if(!s.hevc_started||s.sequence_pending){
                if(!inspection.random_access_ready){
                    if(!s.sequence_pending)s.keyframe_request=true;
                    s.sequence_pending=true;++s.stats.waiting_for_keyframe_packets;return;
                }
                // A same-size VUI change can put iris in DRC while FFmpeg's
                // same-size SOURCE_CHANGE path sends START prematurely. Reopen
                // before this access unit reaches the hardware; never EOS/flush.
                if(!(inspection.has_vps&&inspection.has_sps&&inspection.has_pps))
                    restarted_packet=s.sequence.parameter_sets_annexb();
                require(restarted_packet.size()+size<=kMaxPacket,"hevc_restart_packet_too_large");
                restarted_packet.insert(restarted_packet.end(),data,data+size);
                // Both pieces were validated before touching the old context.
                // Keep that canonical cache: reparsing the concatenation would
                // count repeated parameter sets twice against its cache budget.
                if(s.hevc_started||s.hardware_recovery_pending){
                    auto next_sequence=std::move(s.sequence);
                    configure(s.config_codec,s.config_width,s.config_height,true);
                    s.sequence=std::move(next_sequence);++s.stats.sequence_restarts;
                }
                // Every reopened HEVC DPB needs IRAP plus its cached VPS/SPS/PPS.
                // A configuration can arrive midway through predicted input;
                // keep draining it and request a keyframe before touching iris.
                s.hevc_started=true;s.sequence_pending=s.keyframe_request=false;
                data=restarted_packet.data();size=restarted_packet.size();
            }
        }
        av_packet_unref(s.packet.get());
        if(s.hevc){
            // Our HEVC device copies exact bytes into OUTPUT MMAP inside send;
            // it never parses or retains this pointer. submit and its EAGAIN
            // retries finish before the caller's packet storage is released.
            s.packet->data=const_cast<std::uint8_t*>(data);s.packet->size=static_cast<int>(size);
        }else{
            // FFmpeg codecs can read padding and retain a packet reference.
            ffcheck(av_new_packet(s.packet.get(),static_cast<int>(size)),"packet_allocation_failed");
            std::memcpy(s.packet->data,data,size);
        }
        auto* stamp=new SourceStamp{source_timestamp,original_submit_monotonic_ns?original_submit_monotonic_ns:start,std::move(vr_metadata),epoch,color};
        s.packet->opaque_ref=av_buffer_create(reinterpret_cast<std::uint8_t*>(stamp),sizeof(SourceStamp),
            [](void*,std::uint8_t* value){delete reinterpret_cast<SourceStamp*>(value);},nullptr,AV_BUFFER_FLAG_READONLY);
        if(!s.packet->opaque_ref)delete stamp;
        require(s.packet->opaque_ref,"timestamp_allocation_failed");
        s.packet->pts=static_cast<std::int64_t>(s.stats.packets)+1;
        s.source_stamps.emplace_back(s.packet->pts,*stamp);
        while(s.source_stamps.size()>256)s.source_stamps.pop_front();
        try{s.send(s.packet.get());}
        catch(const VideoDecodeError& error){if(!s.recover_busy(error))throw;}
        av_packet_unref(s.packet.get());
    });
    s.stats.decode_time_ns+=monotonic_ns()-start;
}
void VideoDecoder::deactivate(){
    auto& s=*impl_;
    // Release frame/packet references before the context: V4L2 frames can own
    // MMAP buffer references. No send(nullptr), flush, or receive is performed.
    s.frame.reset();s.packet.reset();s.codec.reset();s.hevc.reset();
    s.source_stamps.clear();
    s.sequence.reset();s.h264_sequence.reset();s.h264_started=s.hevc_started=false;s.sequence_pending=s.keyframe_request=false;
    s.hardware_recovery_pending=false;s.busy_recoveries_in_configuration=0;
    s.converter.reset();
    s.config_codec=s.config_width=s.config_height=0;s.decoder_eof=false;
    s.output.clear();
    s.rgba_pool.clear(s.stats);
    s.nv12_pool.clear(s.stats);
    // Keep finished/failed and historical stats, including the frame counter
    // used as generation. Reappearing streams must not regress generation.
}
std::size_t VideoDecoder::poll(){
    auto& s=*impl_;const auto start=monotonic_ns();std::size_t produced=0;
    s.guarded([&]{
        if((s.codec||s.hevc)&&!s.finished&&!s.decoder_eof){
            try{produced=s.receive();}
            catch(const VideoDecodeError& error){if(!s.recover_busy(error))throw;}
        }
    });
    s.stats.decode_time_ns+=monotonic_ns()-start;return produced;
}
std::array<int,2> VideoDecoder::readiness_fds()const{return impl_->hevc?impl_->hevc->readiness_fds():std::array<int,2>{-1,-1};}
bool VideoDecoder::idle_poll_needed()const{
    const auto& s=*impl_;
    return !s.finished&&!s.decoder_eof&&s.stats.backend==DecoderBackend::V4l2&&
        (s.codec||s.hevc)&&(!s.source_stamps.empty()||(s.hevc&&s.hevc->has_pending_work()));
}
void VideoDecoder::finish(){
    auto& s=*impl_;
    const auto start=monotonic_ns();
    s.guarded([&]{
        if(s.finished)return;
        s.finished=true;
        if(s.codec||s.hevc){s.send(nullptr);require(s.decoder_eof,"decoder_flush_did_not_finish");}
    });
    s.stats.decode_time_ns+=monotonic_ns()-start;
}
DecoderStats VideoDecoder::statistics()const{return impl_->stats;}
bool VideoDecoder::needs_keyframe() const{return impl_->keyframe_request;}
void VideoDecoder::keyframe_request_queued(){impl_->keyframe_request=false;}


} // namespace surfxr
