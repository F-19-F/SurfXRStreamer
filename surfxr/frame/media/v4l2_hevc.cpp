#include "v4l2_hevc.hpp"
#include "surfxr/video_decoder.hpp"
#include "nv12_view.hpp"
#include "decoder_wake.hpp"
#include <cerrno>
#ifdef __linux__
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <atomic>
#include <optional>

namespace surfxr {
namespace {
constexpr auto output_type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture_type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
void require_device(bool value,const char* stage){if(!value)throw VideoDecodeError(stage);}
int call(int fd,unsigned long request,void* value){
    int result;do{result=ioctl(fd,request,value);}while(result<0&&errno==EINTR);
    return result<0?AVERROR(errno):result;
}
void checked(int fd,unsigned long request,void* value,const char* stage){
    const auto result=call(fd,request,value);if(result<0)throw VideoDecodeError(stage,result);
}
struct Plane {
    void* data=MAP_FAILED;std::size_t size=0;int export_fd=-1;
    Plane()=default;Plane(const Plane&)=delete;Plane& operator=(const Plane&)=delete;
    ~Plane(){if(data!=MAP_FAILED)munmap(data,size);if(export_fd>=0)close(export_fd);}
};
struct Buffer {
    std::vector<std::shared_ptr<Plane>> planes;
    bool queued=false,held=false,terminal=false,import_attempted=false;
    std::shared_ptr<std::atomic<bool>> released=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<Nv12FrameStorage> imported;
};
struct Queue {
    v4l2_buf_type type;bool streaming=false;std::vector<Buffer> buffers;
    explicit Queue(v4l2_buf_type value):type(value){}
};
struct Request {
    v4l2_buffer buffer{};std::array<v4l2_plane,VIDEO_MAX_PLANES> planes{};
    explicit Request(v4l2_buf_type type,unsigned index=0){
        buffer.type=type;buffer.memory=V4L2_MEMORY_MMAP;buffer.index=index;
        buffer.length=planes.size();buffer.m.planes=planes.data();
    }
};
int open_decoder(){
    // Select the stateful iris decoder, never an encoder/camera/stateless node.
    for(const auto& entry:std::filesystem::directory_iterator("/sys/class/video4linux")){
        std::ifstream source(entry.path()/"name");std::string name;std::getline(source,name);
        if(name!="qcom-iris-decoder")continue;
        const auto path=std::filesystem::path("/dev")/entry.path().filename();
        const auto fd=open(path.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
        if(fd<0)throw VideoDecodeError("hevc_device_open_failed",AVERROR(errno));
        v4l2_capability cap{};const auto result=call(fd,VIDIOC_QUERYCAP,&cap);
        const auto caps=(cap.capabilities&V4L2_CAP_DEVICE_CAPS)?cap.device_caps:cap.capabilities;
        if(result>=0&&(caps&V4L2_CAP_VIDEO_M2M_MPLANE)&&(caps&V4L2_CAP_STREAMING))return fd;
        close(fd);
    }
    throw VideoDecodeError("hevc_stateful_device_unavailable");
}
AVColorSpace color_space(const v4l2_pix_format_mplane& raw){
    switch(raw.colorspace){
    case V4L2_COLORSPACE_REC709:return AVCOL_SPC_BT709;
    case V4L2_COLORSPACE_SMPTE170M:return AVCOL_SPC_SMPTE170M;
    case V4L2_COLORSPACE_SMPTE240M:return AVCOL_SPC_SMPTE240M;
    case V4L2_COLORSPACE_470_SYSTEM_BG:return AVCOL_SPC_BT470BG;
    case V4L2_COLORSPACE_470_SYSTEM_M:return AVCOL_SPC_FCC;
    case V4L2_COLORSPACE_BT2020:return raw.ycbcr_enc==V4L2_YCBCR_ENC_BT2020_CONST_LUM?
        AVCOL_SPC_BT2020_CL:AVCOL_SPC_BT2020_NCL;
    default:return AVCOL_SPC_UNSPECIFIED;
    }
}
}
struct V4l2Hevc::Impl {
    const std::size_t mapping_budget;
    const unsigned input_limit;
    int fd=-1;unsigned width,height;
    Queue output{output_type},capture{capture_type};
    v4l2_format capture_format{};
    bool draining=false,eof=false;
    std::shared_ptr<decoder_detail::DecoderWake> lease_wake=std::make_shared<decoder_detail::DecoderWake>();
    std::optional<unsigned> consuming;
    Nv12DmaBuf current_capture;
    explicit Impl(unsigned w,unsigned h,bool packed_stereo):mapping_budget((packed_stereo?1024u:512u)*1024u*1024u),
        input_limit(packed_stereo?4u:16u),width(w),height(h){
        try{
            fd=open_decoder();
            for(const auto type:{V4L2_EVENT_SOURCE_CHANGE,V4L2_EVENT_EOS}){
                v4l2_event_subscription subscription{};subscription.type=type;
                checked(fd,VIDIOC_SUBSCRIBE_EVENT,&subscription,"hevc_subscribe_failed");
            }
            v4l2_format format{};format.type=output_type;
            auto& image=format.fmt.pix_mp;image.width=w;image.height=h;
            image.pixelformat=V4L2_PIX_FMT_HEVC;image.num_planes=1;
            image.plane_fmt[0].sizeimage=16u*1024u*1024u;
            checked(fd,VIDIOC_S_FMT,&format,"hevc_output_format_failed");
            require_device(image.pixelformat==V4L2_PIX_FMT_HEVC&&image.num_planes==1,"hevc_output_format_mismatch");
            capture_format.type=capture_type;auto& raw=capture_format.fmt.pix_mp;
            raw.width=w;raw.height=h;raw.pixelformat=V4L2_PIX_FMT_NV12;raw.num_planes=1;
            checked(fd,VIDIOC_S_FMT,&capture_format,"hevc_capture_format_failed");
            allocate(output,16);
        }catch(...){release();throw;}
    }
    ~Impl(){release();}
    void release()noexcept{
        if(fd<0)return;
        // STREAMOFF retires decoder writes; imported frames retain their own
        // mapping/export FD after close. No release callback touches this fd.
        for(auto* queue:{&capture,&output}){
            if(queue->streaming){auto type=queue->type;call(fd,VIDIOC_STREAMOFF,&type);queue->streaming=false;}
            queue->buffers.clear();
        }
        close(fd);fd=-1;
    }
    void allocate(Queue& queue,unsigned count){
        v4l2_requestbuffers request{};request.type=queue.type;request.memory=V4L2_MEMORY_MMAP;request.count=count;
        checked(fd,VIDIOC_REQBUFS,&request,"hevc_request_buffers_failed");
        require_device(request.count>=2&&request.count<=32,"hevc_buffer_count_invalid");
        queue.buffers.resize(request.count);std::size_t total=0;
        for(unsigned i=0;i<request.count;++i){
            Request query(queue.type,i);checked(fd,VIDIOC_QUERYBUF,&query.buffer,"hevc_query_buffer_failed");
            require_device(query.buffer.length>=1&&query.buffer.length<=2,"hevc_plane_count_invalid");
            auto& buffer=queue.buffers[i];buffer.planes.reserve(query.buffer.length);
            for(unsigned plane=0;plane<query.buffer.length;++plane){
                const auto bytes=query.planes[plane].length;
                require_device(bytes&&bytes<=64u*1024u*1024u&&bytes<=mapping_budget-total,"hevc_mapping_budget_exceeded");
                total+=bytes;buffer.planes.push_back(std::make_shared<Plane>());auto& mapped=*buffer.planes.back();mapped.size=bytes;
                mapped.data=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,query.planes[plane].m.mem_offset);
                if(mapped.data==MAP_FAILED)throw VideoDecodeError("hevc_mapping_failed",AVERROR(errno));
            }
        }
    }
    void stream_on(Queue& queue){
        auto type=queue.type;checked(fd,VIDIOC_STREAMON,&type,"hevc_stream_on_failed");queue.streaming=true;
    }
    void enqueue(Queue& queue,Request& request){
        auto& mapped=queue.buffers.at(request.buffer.index);request.buffer.length=mapped.planes.size();
        for(unsigned i=0;i<mapped.planes.size();++i)request.planes[i].length=mapped.planes[i]->size;
        checked(fd,VIDIOC_QBUF,&request.buffer,"hevc_queue_failed");mapped.queued=true;
    }
    int dequeue(Queue& queue,Request& request){
        const auto result=call(fd,VIDIOC_DQBUF,&request.buffer);
        if(result<0)return result;
        require_device(request.buffer.index<queue.buffers.size(),"hevc_dequeue_index_invalid");
        queue.buffers[request.buffer.index].queued=false;return 0;
    }
    void events(){
        for(unsigned n=0;n<32;++n){
            v4l2_event event{};const auto result=call(fd,VIDIOC_DQEVENT,&event);
            if(result==AVERROR(EAGAIN)||result==AVERROR(ENOENT))return;
            if(result<0)throw VideoDecodeError("hevc_event_failed",result);
            if(event.type==V4L2_EVENT_EOS){eof=true;continue;}
            if(event.type!=V4L2_EVENT_SOURCE_CHANGE)continue;
            // Initial SOURCE_CHANGE means headers are parsed, even if dimensions
            // equal the configured size. CAPTURE starts now; no premature START.
            if(capture.streaming)throw VideoDecodeError("hevc_unexpected_source_change");
            checked(fd,VIDIOC_G_FMT,&capture_format,"hevc_capture_query_failed");
            const auto& raw=capture_format.fmt.pix_mp;
            require_device(raw.pixelformat==V4L2_PIX_FMT_NV12&&raw.num_planes==1&&
                raw.width>=width&&raw.height>=height&&raw.width<=8192&&raw.height<=8192,
                "hevc_capture_layout_mismatch");
            v4l2_selection crop{};crop.type=capture_type;crop.target=V4L2_SEL_TGT_COMPOSE;
            if(call(fd,VIDIOC_G_SELECTION,&crop)>=0)
                require_device(crop.r.left==0&&crop.r.top==0&&crop.r.width==width&&crop.r.height==height,"hevc_visible_size_mismatch");
            allocate(capture,20);
            for(unsigned i=0;i<capture.buffers.size();++i){Request request(capture_type,i);enqueue(capture,request);}
            stream_on(capture);
        }
        throw VideoDecodeError("hevc_event_budget_exceeded");
    }
    void reclaim_capture(){
        lease_wake->consume();
        for(unsigned i=0;i<capture.buffers.size();++i){auto& b=capture.buffers[i];
            if(b.held&&b.released->load(std::memory_order_acquire)){
                b.held=false;if(!b.terminal){Request request(capture_type,i);enqueue(capture,request);}
            }
        }
    }
    void reclaim(){
        reclaim_capture();
        if(!output.streaming)return;
        for(unsigned n=0;n<output.buffers.size();++n){
            Request request(output_type);const auto result=dequeue(output,request);
            if(result==AVERROR(EAGAIN))return;
            if(result<0)throw VideoDecodeError("hevc_output_dequeue_failed",result);
        }
    }
    int send(const AVPacket* packet){
        events();reclaim();if(eof)return AVERROR_EOF;
        if(!packet){
            if(!draining){v4l2_decoder_cmd command{};command.cmd=V4L2_DEC_CMD_STOP;
                checked(fd,VIDIOC_DECODER_CMD,&command,"hevc_drain_failed");draining=true;}
            return 0;
        }
        if(draining)return AVERROR_EOF;
        // Packed SURFXR frames have a bounded presentation deadline. Retain the
        // allocated buffers, but do not fill all 16 with increasingly old input.
        if(std::count_if(output.buffers.begin(),output.buffers.end(),[](const auto& b){return b.queued;})>=input_limit)
            return AVERROR(EAGAIN);
        const auto found=std::find_if(output.buffers.begin(),output.buffers.end(),[](const auto& buffer){return !buffer.queued;});
        if(found==output.buffers.end())return AVERROR(EAGAIN);
        require_device(packet->size>0&&std::size_t(packet->size)<=found->planes[0]->size,"hevc_packet_exceeds_device_buffer");
        std::memcpy(found->planes[0]->data,packet->data,packet->size);
        Request request(output_type,static_cast<unsigned>(found-output.buffers.begin()));
        request.planes[0].bytesused=packet->size;request.buffer.flags=V4L2_BUF_FLAG_TIMESTAMP_COPY;
        request.buffer.timestamp.tv_sec=packet->pts/1000000;request.buffer.timestamp.tv_usec=packet->pts%1000000;
        enqueue(output,request);if(!output.streaming)stream_on(output);return 0;
    }
    int receive(AVFrame* frame,const std::function<void()>& consume){
        events();reclaim();if(!capture.streaming)return eof?AVERROR_EOF:AVERROR(EAGAIN);
        Request request(capture_type);const auto result=dequeue(capture,request);
        if(result==AVERROR(EAGAIN))return eof?AVERROR_EOF:result;
        if(result<0)throw VideoDecodeError("hevc_capture_dequeue_failed",result);
        auto& mapped=capture.buffers[request.buffer.index];
        mapped.terminal=(request.buffer.flags&V4L2_BUF_FLAG_LAST)!=0;
        if(!request.planes[0].bytesused){
            const bool last=(request.buffer.flags&V4L2_BUF_FLAG_LAST)!=0;
            if(last)eof=true;else enqueue(capture,request);
            return eof?AVERROR_EOF:AVERROR(EAGAIN);
        }
        try{
            require_device(!(request.buffer.flags&V4L2_BUF_FLAG_ERROR),"v4l2_corrupt_decoded_frame");
            const auto& raw=capture_format.fmt.pix_mp;const auto stride=raw.plane_fmt[0].bytesperline;
            const auto offset=request.planes[0].data_offset;const std::size_t uv=std::size_t(stride)*raw.height;
            require_device(stride>=width&&offset<=request.planes[0].bytesused&&
                request.planes[0].bytesused<=mapped.planes[0]->size&&
                uv+std::size_t(stride)*((height+1)/2)<=mapped.planes[0]->size-offset,"hevc_capture_buffer_too_small");
            av_frame_unref(frame);frame->format=AV_PIX_FMT_NV12;frame->width=width;frame->height=height;
            auto* data=static_cast<std::uint8_t*>(mapped.planes[0]->data)+offset;
            frame->pts=std::int64_t(request.buffer.timestamp.tv_sec)*1000000+request.buffer.timestamp.tv_usec;
            frame->color_range=raw.quantization==V4L2_QUANTIZATION_FULL_RANGE?AVCOL_RANGE_JPEG:
                raw.quantization==V4L2_QUANTIZATION_LIM_RANGE?AVCOL_RANGE_MPEG:AVCOL_RANGE_UNSPECIFIED;
            frame->colorspace=color_space(raw);
            if(consume){
                consuming=request.buffer.index;
                current_capture={-1,width,height,mapped.planes[0]->size,stride,offset,offset+uv,
                    static_cast<std::uint8_t*>(mapped.planes[0]->data),mapped.planes[0]};
                decoder_detail::consume_nv12_view(*frame,data,mapped.planes[0]->size-offset,stride,uv,consume);
                consuming.reset();current_capture={};
            }else{
                const auto allocated=av_frame_get_buffer(frame,32);if(allocated<0)throw VideoDecodeError("hevc_frame_allocation_failed",allocated);
                for(unsigned plane=0;plane<2;++plane)for(unsigned y=0;y<(plane?(height+1)/2:height);++y)
                    std::memcpy(frame->data[plane]+std::size_t(y)*frame->linesize[plane],data+(plane?uv:0)+std::size_t(y)*stride,width);
            }
        }catch(...){consuming.reset();current_capture={};if(!mapped.held)enqueue(capture,request);throw;}
        const bool last=(request.buffer.flags&V4L2_BUF_FLAG_LAST)!=0;
        request.buffer.flags=0;for(auto& plane:request.planes){plane.bytesused=plane.data_offset=0;}
        if(last)eof=true;else if(!mapped.held)enqueue(capture,request);
        return 0;
    }
    std::shared_ptr<Nv12FrameStorage> import_capture(const Nv12FrameAllocator& allocator){
        if(!consuming||!allocator.import)return {};
        auto& buffer=capture.buffers[*consuming];
        if(buffer.import_attempted)return buffer.imported;
        buffer.import_attempted=true;
        auto& plane=*buffer.planes[0];
        v4l2_exportbuffer request{};request.type=capture_type;request.index=*consuming;
        request.flags=O_CLOEXEC|O_RDONLY;
        if(call(fd,VIDIOC_EXPBUF,&request)<0)return {};
        plane.export_fd=request.fd;current_capture.fd=request.fd;
        buffer.imported=allocator.import(current_capture);return buffer.imported;
    }
    std::shared_ptr<void> retain_capture(){
        require_device(bool(consuming),"hevc_capture_lease_outside_consumer");
        auto& buffer=capture.buffers[*consuming];
        require_device(!buffer.held,"hevc_capture_already_leased");
        // Bound external retention to six of twenty buffers, preserving codec
        // reference capacity and the existing presentation pool pressure limit.
        if(std::count_if(capture.buffers.begin(),capture.buffers.end(),[](const auto& b){return b.held;})>=6)return {};
        auto released=buffer.released;
        auto wake=lease_wake;
        auto lease=std::shared_ptr<void>(released.get(),[released,wake](void*){released->store(true,std::memory_order_release);wake->notify();});
        released->store(false,std::memory_order_relaxed);buffer.held=true;return lease;
    }
};
V4l2Hevc::V4l2Hevc(unsigned width,unsigned height,bool packed_stereo):impl_(std::make_unique<Impl>(width,height,packed_stereo)){}
V4l2Hevc::~V4l2Hevc()=default;
int V4l2Hevc::send(const AVPacket* packet){return impl_->send(packet);}
int V4l2Hevc::receive(AVFrame* frame,const std::function<void()>& consume){return impl_->receive(frame,consume);}
std::shared_ptr<Nv12FrameStorage> V4l2Hevc::import_capture(const Nv12FrameAllocator& a){return impl_->import_capture(a);}
std::shared_ptr<void> V4l2Hevc::retain_capture(){return impl_->retain_capture();}
void V4l2Hevc::wait(){pollfd descriptor{impl_->fd,POLLIN|POLLPRI,0};::poll(&descriptor,1,1);}
std::array<int,2> V4l2Hevc::readiness_fds()const{
    // Some M2M drivers signal POLLERR before initial SOURCE_CHANGE. Keep the
    // bounded startup poll until CAPTURE is active, then use real readiness.
    return impl_->capture.streaming?std::array<int,2>{impl_->fd,impl_->lease_wake->descriptor()}:std::array<int,2>{-1,-1};
}
bool V4l2Hevc::has_pending_work()const{
    return std::any_of(impl_->output.buffers.begin(),impl_->output.buffers.end(),[](const auto& b){return b.queued;})||
        std::any_of(impl_->capture.buffers.begin(),impl_->capture.buffers.end(),[](const auto& b){return b.held&&b.released->load(std::memory_order_acquire);});
}
int V4l2Hevc::probe_capture_dmabuf_export(){
    if(impl_->capture.buffers.empty())return AVERROR(EAGAIN);
    v4l2_exportbuffer request{};request.type=capture_type;request.index=0;request.plane=0;request.flags=O_CLOEXEC|O_RDONLY;
    const auto result=call(impl_->fd,VIDIOC_EXPBUF,&request);
    if(result>=0)close(request.fd);
    return result;
}
}
#else
namespace surfxr {
struct V4l2Hevc::Impl {};
V4l2Hevc::V4l2Hevc(unsigned,unsigned,bool){throw VideoDecodeError("hevc_stateful_device_unavailable");}
V4l2Hevc::~V4l2Hevc()=default;
int V4l2Hevc::send(const AVPacket*){return AVERROR(ENOSYS);}
int V4l2Hevc::receive(AVFrame*,const std::function<void()>&){return AVERROR(ENOSYS);}
std::shared_ptr<Nv12FrameStorage> V4l2Hevc::import_capture(const Nv12FrameAllocator&){return {};}
std::shared_ptr<void> V4l2Hevc::retain_capture(){return {};}
void V4l2Hevc::wait(){}
std::array<int,2> V4l2Hevc::readiness_fds()const{return {-1,-1};}
bool V4l2Hevc::has_pending_work()const{return false;}
int V4l2Hevc::probe_capture_dmabuf_export(){return AVERROR(ENOSYS);}
}
#endif
