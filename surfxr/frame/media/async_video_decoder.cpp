#include "async_decoder_private.hpp"

namespace surfxr {
namespace {
struct RealDecoder final:DecoderOwner {
    VideoDecoder decoder;
    RealDecoder(LatestFrameSlot& output,DecoderBackend backend,VideoDecoder::FrameSink sink,unsigned threads,RgbaFrameAllocator allocator,VideoDecoder::FrameDemand demand,DecoderFrameLayout layout,Nv12FrameAllocator nv12_allocator)
        :decoder(output,backend,std::move(sink),threads,std::move(allocator),std::move(demand),layout,std::move(nv12_allocator)){}
    void configure(std::uint32_t codec,std::uint32_t width,std::uint32_t height,bool restart)override{decoder.configure(codec,width,height,restart);}
    void deactivate()override{decoder.deactivate();}
    void submit(const std::uint8_t* data,std::size_t size,std::uint64_t timestamp,std::shared_ptr<const VrVideoMetadata> metadata,std::uint64_t original,
                std::optional<std::uint64_t> epoch)override{
        decoder.submit(data,size,timestamp,std::move(metadata),original,epoch);
    }
    std::size_t poll()override{return decoder.poll();}
    std::array<int,2> readiness_fds()const override{return decoder.readiness_fds();}
    bool idle_poll_needed()const override{return decoder.idle_poll_needed();}
    DecoderStats statistics()const override{return decoder.statistics();}
    bool needs_keyframe()const override{return decoder.needs_keyframe();}
    void keyframe_request_queued()override{decoder.keyframe_request_queued();}
};
}
AsyncVideoDecoder::AsyncVideoDecoder(LatestFrameSlot& output,DecoderBackend backend,FrameSink sink,unsigned threads,RgbaFrameAllocator allocator,FrameDemand demand,DecoderFrameLayout layout,Nv12FrameAllocator nv12_allocator)
    :impl_(std::make_unique<Impl>([&output,backend,sink=std::move(sink),threads,allocator=std::move(allocator),demand,layout,nv12_allocator=std::move(nv12_allocator)]()mutable{
        return std::make_unique<RealDecoder>(output,backend,std::move(sink),threads,std::move(allocator),std::move(demand),layout,std::move(nv12_allocator));
    },demand,layout==DecoderFrameLayout::PackedStereo?kMaxPackedPackets:kMaxPackets)){}
AsyncVideoDecoder::AsyncVideoDecoder(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
AsyncVideoDecoder::~AsyncVideoDecoder()=default;
void AsyncVideoDecoder::configure(std::uint32_t codec,std::uint32_t width,std::uint32_t height,bool /*restart*/){impl_->barrier(Impl::CommandKind::Configure,codec,width,height);}
void AsyncVideoDecoder::deactivate(){impl_->barrier(Impl::CommandKind::Deactivate);}
void AsyncVideoDecoder::submit(const std::uint8_t* data,std::size_t size,std::uint64_t timestamp,std::shared_ptr<const VrVideoMetadata> metadata){impl_->submit(data,size,timestamp,std::move(metadata));}
void AsyncVideoDecoder::submit(std::vector<std::uint8_t>&& bytes,std::size_t offset,std::uint64_t timestamp,std::shared_ptr<const VrVideoMetadata> metadata){
    if(offset>=bytes.size())throw VideoDecodeError("invalid_encoded_packet");
    impl_->submit(bytes.data()+offset,bytes.size()-offset,timestamp,std::move(metadata),true,&bytes,offset);
}
bool AsyncVideoDecoder::try_submit(const std::uint8_t* data,std::size_t size,std::uint64_t timestamp,std::shared_ptr<const VrVideoMetadata> metadata){return impl_->submit(data,size,timestamp,std::move(metadata),false);}
bool AsyncVideoDecoder::try_submit(std::vector<std::uint8_t>&& bytes,std::size_t offset,std::uint64_t timestamp,std::shared_ptr<const VrVideoMetadata> metadata){
    if(offset>=bytes.size())throw VideoDecodeError("invalid_encoded_packet");
    return impl_->submit(bytes.data()+offset,bytes.size()-offset,timestamp,std::move(metadata),false,&bytes,offset);
}
DecoderStats AsyncVideoDecoder::statistics()const{return impl_->statistics();}
bool AsyncVideoDecoder::needs_keyframe()const{
    std::lock_guard<std::mutex> lock(impl_->mutex);impl_->throw_locked();
    if(impl_->keyframe)impl_->observed_keyframe_version=impl_->keyframe_version;
    return impl_->keyframe;
}
void AsyncVideoDecoder::keyframe_request_queued(){
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->throw_locked();
        impl_->keyframe_ack_pending=true;impl_->keyframe_ack_version=impl_->observed_keyframe_version;
        if(impl_->keyframe_ack_version==impl_->keyframe_version)impl_->keyframe=false;}
    impl_->notify();
}
void AsyncVideoDecoder::check()const{std::lock_guard<std::mutex> lock(impl_->mutex);if(impl_->failure)std::rethrow_exception(impl_->failure);}
void AsyncVideoDecoder::stop()noexcept{impl_->stop();}

} // namespace surfxr
