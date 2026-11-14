#include "display_private.hpp"

namespace surfxr {
void XrDisplay::set_timestamped_tracking(bool enabled){
    impl_->timestamped_tracking_.store(enabled,std::memory_order_release);
    if(!enabled)impl_->current_tracking_.clear();
}
bool XrDisplay::sample_tracking(TrackingFrame& frame){
    if(!impl_->timestamped_tracking_.load(std::memory_order_acquire))return false;
    timespec monotonic{};const auto before=Clock::now();
    if(clock_gettime(CLOCK_MONOTONIC,&monotonic)!=0)return false;
    const auto after=Clock::now();
    return impl_->current_tracking_.sample(frame,monotonic,clock_ns(before+(after-before)/2));
}

XrDisplay::XrDisplay(bool sync_boundary):impl_(std::make_unique<Impl>()){impl_->boundary_requested_=sync_boundary;impl_->initialize();}
XrDisplay::~XrDisplay()=default;
XrDisplayStats XrDisplay::run(LatestFrameSlot& slot,std::atomic_bool& stop,unsigned seconds){return run({XrPanelSource{0,&slot,"Desktop",{}}},stop,seconds,{});}
XrDisplayStats XrDisplay::run(const std::vector<XrPanelSource>& panels,std::atomic_bool& stop,unsigned seconds,const XrUiCallbacks& callbacks,const UiLayout* initial_layout){return impl_->run(panels,stop,seconds,callbacks,initial_layout);}
std::optional<UiLayout> XrDisplay::layout() const {if(!impl_->ui_.anchored())return std::nullopt;return impl_->ui_.export_layout();}
XrDisplayStats XrDisplay::statistics() const noexcept {auto s=impl_->stats_;if(impl_->used_&&!s.elapsed_seconds)s.elapsed_seconds=std::chrono::duration<double>(Clock::now()-impl_->started_).count();return s;}
RgbaFrameAllocator XrDisplay::rgba_frame_allocator()const{
    return impl_->format_==VK_FORMAT_R8G8B8A8_SRGB?vulkan_rgba_frame_allocator(impl_->physical_,impl_->device_):RgbaFrameAllocator{};
}
Nv12FrameAllocator XrDisplay::nv12_frame_allocator()const{
    return impl_->format_==VK_FORMAT_R8G8B8A8_SRGB?vulkan_nv12_frame_allocator(impl_->physical_,impl_->device_,impl_->dmabuf_device_enabled_):Nv12FrameAllocator{};
}
void XrDisplay::retire_decoder_frames(const std::atomic_bool& stop){
    impl_->decoder_frames_retire_requested_.store(true,std::memory_order_release);
    while(impl_->decoder_frames_retire_requested_.load(std::memory_order_acquire)&&!stop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void XrDisplay::set_timing_trace_enabled(bool enabled){impl_->timing_.set_enabled(enabled);}
void XrDisplay::set_desktop_nv12_fused(bool enabled){
    if(impl_->desktop_nv12_fused_==enabled)return;
    impl_->desktop_nv12_fused_=enabled;
    if(impl_->projection_renderer_)for(unsigned slot=0;slot<4;++slot)impl_->projection_renderer_->invalidate(slot);
}
void XrDisplay::set_vr_presentation_pacing(bool enabled){impl_->vr_pacer_.set_enabled(enabled);}
FrameTimingSnapshot XrDisplay::timing_snapshot()const{return impl_->timing_.snapshot();}
} // namespace surfxr
