#pragma once
// Bounded RGBA ownership and exact packet-ID stamps survive hardware reorder.
#include "surfxr/video_decoder.hpp"
#include "surfxr/hevc_sequence.hpp"
#include "surfxr/h264_sequence.hpp"
#include "surfxr/video_color.hpp"
#include "surfxr/vr_wire.hpp"
#include "v4l2_hevc.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <chrono>
#include <cstring>
#include <utility>
#include <deque>
#include <algorithm>
#include <cmath>
#include "nv12_output.hpp"

namespace surfxr {
namespace decoder_detail {
constexpr std::size_t kMaxPacket=16u*1024u*1024u;
constexpr std::int64_t kMaxPixels=16ll*1024ll*1024ll;
constexpr std::size_t kRgbaPoolBuffers=6,kRgbaPoolBytes=128u*1024u*1024u;
inline void require(bool value,const char* stage){if(!value)throw VideoDecodeError(stage);}
inline void ffcheck(int value,const char* stage){if(value<0)throw VideoDecodeError(stage,value);}
inline std::uint64_t monotonic_ns(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct CodecDelete {void operator()(AVCodecContext* p)const{avcodec_free_context(&p);}};
struct FrameDelete {void operator()(AVFrame* p)const{av_frame_free(&p);}};
struct PacketDelete {void operator()(AVPacket* p)const{av_packet_free(&p);}};
using Codec=std::unique_ptr<AVCodecContext,CodecDelete>;
using Frame=std::unique_ptr<AVFrame,FrameDelete>;
using Packet=std::unique_ptr<AVPacket,PacketDelete>;
struct SourceStamp {
    std::uint64_t timestamp;
    std::uint64_t packet_submit_monotonic_ns;
    std::shared_ptr<const VrVideoMetadata> vr_metadata;
    std::uint64_t output_epoch=0;
    VideoColorInfo color{};
};
struct RgbaColor {
    AVColorSpace space;
    AVColorRange range;
    int matrix;
};
inline RgbaColor rgba_color(const AVFrame& source,DecoderBackend backend,const VideoColorInfo& signaled={}){
    auto space=source.colorspace;
    auto range=source.color_range;
    // Iris can report REC709 for decoded BT.601 pixels. A known SPS field is
    // authoritative and travels with this output's exact packet; reordered
    // frames never inherit a newer sequence. Keep raw AVFrame diagnostics and
    // software decoding unchanged. Missing SPS fields use the decoder metadata.
    if(backend==DecoderBackend::V4l2){
        if(signaled.color_space!=AVCOL_SPC_UNSPECIFIED)space=static_cast<AVColorSpace>(signaled.color_space);
        else if(space==AVCOL_SPC_UNSPECIFIED)space=AVCOL_SPC_BT709;
        if(signaled.color_range==AVCOL_RANGE_MPEG||signaled.color_range==AVCOL_RANGE_JPEG)
            range=static_cast<AVColorRange>(signaled.color_range);
    }
    int matrix=SWS_CS_DEFAULT;
    if(space==AVCOL_SPC_BT709)matrix=SWS_CS_ITU709;
    else if(space==AVCOL_SPC_BT2020_NCL||space==AVCOL_SPC_BT2020_CL)matrix=SWS_CS_BT2020;
    else if(space!=AVCOL_SPC_BT470BG&&space!=AVCOL_SPC_SMPTE170M)space=AVCOL_SPC_SMPTE170M;
    return {space,range==AVCOL_RANGE_JPEG?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG,matrix};
}
// Only the decoder producer touches this pool. Slot/sink consumers receive
// const shared ownership. A retained frame is writable only after all such
// ownership is released; the pool's single reference is then the only owner.
class RgbaFramePool {
public:
    explicit RgbaFramePool(std::size_t byte_limit=kRgbaPoolBytes,RgbaFrameAllocator allocator={},std::int64_t pixel_limit=kMaxPixels):
        byte_limit_(byte_limit),allocator_(std::move(allocator)),pixel_limit_(pixel_limit){}
    void clear(DecoderStats& stats){
        frames_.clear();width_=height_=0;stats.rgba_pool_buffers=stats.rgba_pool_bytes=0;
    }
    std::shared_ptr<VideoFrame> acquire(std::uint32_t width,std::uint32_t height,DecoderStats& stats){
        require(width>0&&height>0&&std::uint64_t{width}*height<=static_cast<std::uint64_t>(pixel_limit_),"rgba_pool_dimensions_exceed_policy");
        if(width!=width_||height!=height_){clear(stats);width_=width;height_=height;}
        const auto bytes=static_cast<std::size_t>(width)*height*4;
        std::shared_ptr<VideoFrame> result;
        for(const auto& candidate:frames_)if(candidate.use_count()==1){result=candidate;++stats.rgba_buffer_reuses;break;}
        if(!result){
            result=std::make_shared<VideoFrame>();
            // Never allocate an unbounded series of external GPU buffers when
            // every retained frame is still held by pairing/rendering. The
            // existing temporary vector path remains the pressure fallback.
            const bool retain=frames_.size()<kRgbaPoolBuffers&&bytes<=byte_limit_-static_cast<std::size_t>(stats.rgba_pool_bytes);
            if(retain&&allocator_)result->rgba_storage=allocator_(bytes);
            if(result->rgba_storage)require(result->rgba_data()&&result->rgba_size()==bytes,"rgba_allocator_invalid_storage");
            else result->rgba.resize(bytes);
            ++stats.rgba_buffer_allocations;
            if(retain&&result->rgba_capacity()<=byte_limit_-static_cast<std::size_t>(stats.rgba_pool_bytes)){
                frames_.push_back(result);stats.rgba_pool_buffers=frames_.size();stats.rgba_pool_bytes+=result->rgba_capacity();
            }else ++stats.rgba_temporary_allocations;
        }
        // Do not resize/clear the pixel vector on reuse: sws_scale overwrites
        // all rows. Packet metadata must not survive into an unmatched frame.
        result->width=width;result->height=height;result->stride=width*4;
        result->generation=result->source_timestamp=result->decoded_monotonic_ns=result->output_epoch=0;
        result->packet_submit_monotonic_ns=result->conversion_begin_monotonic_ns=0;
        result->decoder_output_monotonic_ns=0;
        result->has_source_timestamp=false;result->vr_metadata.reset();
        return result;
    }
private:
    std::vector<std::shared_ptr<VideoFrame>> frames_;
    std::uint32_t width_=0,height_=0;
    std::size_t byte_limit_;
    RgbaFrameAllocator allocator_;
    std::int64_t pixel_limit_;
};
// Conversion stays synchronous: no decoder/MMAP frame or writable RGBA storage
// escapes into a worker queue. sws_scale_frame's bounded slice workers finish
// before the immutable VideoFrame is published to consumers.
class RgbaConverter {
public:
    explicit RgbaConverter(unsigned threads=1):threads_(threads){require(threads==1||threads==4,"invalid_rgba_threads");}
    ~RgbaConverter(){reset();}
    RgbaConverter(const RgbaConverter&)=delete;
    RgbaConverter& operator=(const RgbaConverter&)=delete;
    void reset(){sws_freeContext(context_);context_=nullptr;destination_.reset();width_=height_=0;format_=AV_PIX_FMT_NONE;}
    void convert(const AVFrame& source,VideoFrame& output,DecoderBackend backend=DecoderBackend::Software,const VideoColorInfo& signaled={}){
        const auto format=static_cast<AVPixelFormat>(source.format);
        require(source.width>0&&source.height>0&&output.width==static_cast<unsigned>(source.width)
                &&output.height==static_cast<unsigned>(source.height)&&output.stride==output.width*4
                &&output.rgba_size()==std::size_t{output.stride}*output.height&&output.rgba_data(),"rgba_conversion_layout_mismatch");
        if(threads_==1){
            context_=sws_getCachedContext(context_,source.width,source.height,format,source.width,source.height,
                AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            require(context_,"rgba_converter_failed");
        }else{
            // av_frame_ref must borrow the decoded V4L2 frame, not allocate and
            // copy unreferenced source pixels behind this path's timing.
            require(source.buf[0],"rgba_source_not_refcounted");
            if(!context_||width_!=source.width||height_!=source.height||format_!=format){
                std::unique_ptr<SwsContext,decltype(&sws_freeContext)> next(sws_alloc_context(),sws_freeContext);
                require(bool(next),"rgba_converter_failed");
                const auto set=[&](const char* key,std::int64_t value){ffcheck(av_opt_set_int(next.get(),key,value,0),"rgba_converter_option_failed");};
                set("srcw",source.width);set("srch",source.height);set("dstw",source.width);set("dsth",source.height);
                set("src_format",format);set("dst_format",AV_PIX_FMT_RGBA);set("sws_flags",SWS_BILINEAR);set("threads",threads_);
                ffcheck(sws_init_context(next.get(),nullptr,nullptr),"rgba_converter_init_failed");
                sws_freeContext(context_);context_=next.release();width_=source.width;height_=source.height;format_=format;
            }
            if(!destination_)destination_.reset(av_frame_alloc());
            require(bool(destination_),"rgba_destination_allocation_failed");
        }
        const auto color=rgba_color(source,backend,signaled);
        ffcheck(sws_setColorspaceDetails(context_,sws_getCoefficients(color.matrix),color.range==AVCOL_RANGE_JPEG?1:0,
            sws_getCoefficients(color.matrix),1,0,1<<16,1<<16),"rgba_colorspace_failed");
        if(threads_==1){
            std::uint8_t* planes[4]={output.rgba_data(),nullptr,nullptr,nullptr};
            const int strides[4]={static_cast<int>(output.stride),0,0,0};
            require(sws_scale(context_,source.data,source.linesize,0,source.height,planes,strides)==source.height,"rgba_conversion_incomplete");
            return;
        }
        auto& target=*destination_;
        // The caller retains output for this whole synchronous call. This
        // AVBufferRef owns only a borrowed view, never the vector's allocation.
        // Explicit frame_end also clears any refs left by frame_start failure.
        struct Cleanup {SwsContext* context;AVFrame* target;~Cleanup(){sws_frame_end(context);av_frame_unref(target);}} cleanup{context_,&target};
        target.width=source.width;target.height=source.height;target.format=AV_PIX_FMT_RGBA;
        target.colorspace=AVCOL_SPC_RGB;target.color_range=AVCOL_RANGE_JPEG;
        target.buf[0]=av_buffer_create(output.rgba_data(),output.rgba_size(),[](void*,std::uint8_t*){},nullptr,0);
        require(target.buf[0],"rgba_destination_reference_failed");
        target.data[0]=output.rgba_data();target.linesize[0]=static_cast<int>(output.stride);
        // Borrow the source buffers only for this synchronous call. Keep the
        // frame API's metadata consistent with the chosen coefficients without
        // replacing the original AVFrame's diagnostic metadata.
        AVFrame interpreted=source;interpreted.colorspace=color.space;interpreted.color_range=color.range;
        if(source.extended_data==source.data)interpreted.extended_data=interpreted.data;
        ffcheck(sws_scale_frame(context_,&target,&interpreted),"rgba_frame_conversion_failed");
        require(target.data[0]==output.rgba_data()&&target.linesize[0]==static_cast<int>(output.stride),"rgba_destination_reallocated");
    }
private:
    unsigned threads_=1;
    SwsContext* context_=nullptr;
    Frame destination_;
    int width_=0,height_=0;
    AVPixelFormat format_=AV_PIX_FMT_NONE;
};
template<class F>void rejects(F fn){bool failed=false;try{fn();}catch(const VideoDecodeError&){failed=true;}require(failed,"self_test_expected_decoder_error");}
const char* v4l2_decoder_name(std::uint32_t);
} // namespace decoder_detail

using namespace decoder_detail;
struct VideoDecoder::Impl {
    LatestFrameSlot& output;
    FrameSink sink;
    FrameDemand demand;
    const std::int64_t pixel_limit;
    DecoderStats stats;
    RgbaFramePool rgba_pool;
    Nv12FramePool nv12_pool;
    Codec codec;
    std::unique_ptr<V4l2Hevc> hevc;
    Frame frame{av_frame_alloc()};
    Packet packet{av_packet_alloc()};
    RgbaConverter converter;
    std::uint32_t config_codec=0,config_width=0,config_height=0;
    bool finished=false,decoder_eof=false;
    HevcSequenceTracker sequence;
    H264SequenceTracker h264_sequence;
    bool h264_started=false,hevc_started=false;
    bool sequence_pending=false,keyframe_request=false;
    bool hardware_recovery_pending=false;
    unsigned busy_recoveries_in_configuration=0;
    // V4L2 M2M carries PTS through its buffer timestamp but drops opaque_ref.
    // A bounded packet-ID map preserves the exact source metadata through
    // hardware reordering; never substitute the most recently received pose.
    std::deque<std::pair<std::int64_t,SourceStamp>> source_stamps;
    explicit Impl(LatestFrameSlot& slot,DecoderBackend backend,FrameSink frame_sink,unsigned rgba_threads,RgbaFrameAllocator allocator,FrameDemand frame_demand,DecoderFrameLayout layout,Nv12FrameAllocator nv12_allocator={}):
        output(slot),sink(std::move(frame_sink)),demand(std::move(frame_demand)),pixel_limit(layout==DecoderFrameLayout::PackedStereo?2*kMaxPixels:kMaxPixels),
        rgba_pool(layout==DecoderFrameLayout::PackedStereo?2*kRgbaPoolBytes:kRgbaPoolBytes,std::move(allocator),pixel_limit),
        nv12_pool(std::move(nv12_allocator)),converter(rgba_threads){
        require(layout==DecoderFrameLayout::Single||layout==DecoderFrameLayout::PackedStereo,"invalid_decoder_frame_layout");
        require(backend==DecoderBackend::Software || backend==DecoderBackend::V4l2,"invalid_decoder_backend");
        require(rgba_threads==1||rgba_threads==4,"invalid_rgba_threads");
        stats.backend=backend;
        stats.rgba_conversion_threads=rgba_threads;
        require(frame && packet,"decoder_allocation_failed");
        // Protocol reports never include library log text or encoded payloads.
        av_log_set_level(AV_LOG_QUIET);
    }
    template<class F>void guarded(F fn){
        require(!stats.failed,"decoder_failed_state");
        try{fn();}
        catch(const VideoDecodeError& error){stats.failed=true;stats.ffmpeg_code=error.ffmpeg_code;throw;}
        catch(...){stats.failed=true;throw;}
    }
    bool recover_busy(const VideoDecodeError& error){
        if(stats.backend!=DecoderBackend::V4l2||error.ffmpeg_code!=AVERROR(EBUSY)||
           (!codec&&!hevc)||finished||decoder_eof||busy_recoveries_in_configuration>=2)return false;
        // send_packet can already have consumed input before reporting a driver
        // error. Never resubmit that packet or continue its reference chain.
        // Retire MMAP refs now; reopen only at a fresh validated IDR/IRAP using
        // the cached parameter sets and that packet's own timestamp/metadata.
        av_frame_unref(frame.get());av_packet_unref(packet.get());codec.reset();hevc.reset();
        source_stamps.clear();h264_started=hevc_started=false;
        hardware_recovery_pending=sequence_pending=keyframe_request=true;
        ++busy_recoveries_in_configuration;
        ++stats.hardware_busy_recoveries;
        return true;
    }
    void publish(){
        const auto decoder_output_ns=monotonic_ns();
        const auto w=frame->width,h=frame->height;
        require(w>0 && h>0 && w<=8192 && h<=8192 && std::int64_t{w}*h<=pixel_limit,"decoded_dimensions_exceed_policy");
        const auto format=static_cast<AVPixelFormat>(frame->format);
        const auto* description=av_pix_fmt_desc_get(format);
        require(description && !(description->flags&AV_PIX_FMT_FLAG_HWACCEL),"unexpected_hardware_frame");
        if(stats.backend==DecoderBackend::V4l2){
            require(format==AV_PIX_FMT_NV12,"v4l2_output_not_nv12");
            // The wrapper reports V4L2_BUF_FLAG_ERROR through frame flags, even
            // when receive_frame itself succeeds; AV_EF_EXPLODE is insufficient.
            require(!frame->decode_error_flags && !(frame->flags&AV_FRAME_FLAG_CORRUPT),"v4l2_corrupt_decoded_frame");
        }
        stats.pixel_format=description->name;
        stats.color_space=static_cast<int>(frame->colorspace);stats.color_range=static_cast<int>(frame->color_range);
        const auto found=std::find_if(source_stamps.begin(),source_stamps.end(),[&](const auto& value){return value.first==frame->pts;});
        std::optional<SourceStamp> stamp;
        if(frame->opaque_ref && frame->opaque_ref->size==sizeof(SourceStamp)){
            stamp=*reinterpret_cast<const SourceStamp*>(frame->opaque_ref->data);++stats.metadata_from_opaque;
        }else if(found!=source_stamps.end()){stamp=found->second;++stats.metadata_from_pts;}
        else ++stats.metadata_unmatched;
        if(found!=source_stamps.end())source_stamps.erase(found);
        const auto signaled=stamp?stamp->color:VideoColorInfo{};
        const auto color=rgba_color(*frame,stats.backend,signaled);
        stats.effective_color_space=static_cast<int>(color.space);stats.effective_color_range=static_cast<int>(color.range);
        const auto generation=++stats.frames;
        stats.width=static_cast<std::uint32_t>(w);stats.height=static_cast<std::uint32_t>(h);
        const auto epoch=demand?demand():0;
        // Metadata is consumed even when presentation is paused. Dropping an
        // AVFrame here leaves codec reference pictures and packet order intact.
        // The submitted epoch also excludes queued/reordered pre-resume input.
        if(demand&&(!epoch||!stamp||stamp->output_epoch!=epoch)){
            ++stats.rgba_skipped_frames;output.clear();return;
        }
        if(stamp&&stamp->vr_metadata&&stamp->vr_metadata->rgba_deadline_monotonic_ns&&
           decoder_output_ns>stamp->vr_metadata->rgba_deadline_monotonic_ns){
            ++stats.rgba_skipped_frames;return;
        }
        const auto conversion_start=monotonic_ns();
        const auto coefficients=nv12_coefficients(color.space,color.range);
        const bool nv12=nv12_pool.enabled()&&format==AV_PIX_FMT_NV12&&w%4==0&&h%2==0&&coefficients[0]>0;
        std::shared_ptr<VideoFrame> result;
        bool zero_copy=false;
        if(nv12&&hevc&&nv12_pool.allocator().import){
            if(auto storage=hevc->import_capture(nv12_pool.allocator())){
                if(auto lease=hevc->retain_capture()){
                    result=std::make_shared<VideoFrame>();result->width=w;result->height=h;result->stride=w*4;
                    result->nv12_storage=std::move(storage);result->capture_lease=std::move(lease);zero_copy=true;
                }
            }
            stats.nv12_zero_copy_time_ns+=monotonic_ns()-conversion_start;
            if(!zero_copy)++stats.nv12_zero_copy_fallback_frames;
        }
        if(!result)result=nv12?nv12_pool.acquire(w,h,stats):rgba_pool.acquire(w,h,stats);
        if(!result){++stats.rgba_skipped_frames;return;}
        result->conversion_begin_monotonic_ns=monotonic_ns();
        if(nv12){
            if(!zero_copy){const auto copy_start=monotonic_ns();write_nv12(*frame,*result->nv12_storage);
                stats.nv12_copy_time_ns+=monotonic_ns()-copy_start;}
            result->yuv_coefficients=coefficients;
        }
        else{converter.convert(*frame,*result,stats.backend,signaled);
            stats.rgba_conversion_time_ns+=monotonic_ns()-conversion_start;++stats.rgba_converted_frames;}
        if(demand&&demand()!=epoch){++stats.rgba_suppressed_publications;output.clear();return;}
        if(stamp){result->source_timestamp=stamp->timestamp;result->has_source_timestamp=true;result->vr_metadata=stamp->vr_metadata;
            result->packet_submit_monotonic_ns=stamp->packet_submit_monotonic_ns;}
        result->generation=generation;result->output_epoch=epoch;result->decoded_monotonic_ns=monotonic_ns();
        result->decoder_output_monotonic_ns=decoder_output_ns;
        if(nv12)++stats.nv12_published_frames;
        if(zero_copy)++stats.nv12_zero_copy_frames;
        output.publish(result);
        if(sink)sink(std::move(result));
    }
    std::size_t receive(){
        std::size_t produced=0;
        while(true){
            const auto before=monotonic_ns();
            std::uint64_t consumed_ns=0;
            const int result=hevc?hevc->receive(frame.get(),[&]{
                const auto started=monotonic_ns();publish();consumed_ns=monotonic_ns()-started;
            }):avcodec_receive_frame(codec.get(),frame.get());
            // The callback converts before QBUF. Keep that work in conversion
            // telemetry rather than counting it twice as device receive time.
            stats.avcodec_receive_time_ns+=monotonic_ns()-before-consumed_ns;
            if(result==AVERROR(EAGAIN))return produced;
            if(result==AVERROR_EOF){decoder_eof=true;return produced;}
            ffcheck(result,"video_receive_failed");
            if(!hevc){publish();av_frame_unref(frame.get());}
            ++produced;
        }
    }
    void send(AVPacket* value){
        const auto deadline=monotonic_ns()+2'000'000'000ull;
        while(true){
            const auto before=monotonic_ns();
            const int result=hevc?hevc->send(value):avcodec_send_packet(codec.get(),value);
            stats.avcodec_send_time_ns+=monotonic_ns()-before;
            if(result==AVERROR(EAGAIN)){
                const auto produced=receive();
                if(hevc){require(!decoder_eof&&monotonic_ns()<deadline,"hevc_device_progress_timeout");hevc->wait();}
                else require(produced!=0 && !decoder_eof,"decoder_made_no_progress");
                continue;
            }
            if(result==AVERROR_EOF && value==nullptr){decoder_eof=true;return;}
            ffcheck(result,"video_send_failed");
            if(value)++stats.packets;
            receive();
            if(hevc&&!value)while(!decoder_eof){
                require(monotonic_ns()<deadline,"hevc_drain_timeout");hevc->wait();receive();
            }
            return;
        }

    }
};
} // namespace surfxr
