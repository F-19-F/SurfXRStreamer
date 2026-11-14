#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::retire_decoder_frames(){
    if(!decoder_frames_retire_requested_.load(std::memory_order_acquire))return;
    // This runs between XR frames, after their local source snapshots die.
    // A closed V4L2 fd still owns an iris session while an imported Plane's
    // MMAP holds its file alive. Retire GPU readers before dropping that Plane.
    reset_vr_surfaces();
    vk_check(vkDeviceWaitIdle(device_),"vkDeviceWaitIdle(decoder retirement)");
    for(unsigned i=0;i<surfaces_.size();++i){auto& s=surfaces_[i];
        if(!s.nv12_storage||!s.nv12_storage->foreign())continue;
        // A retired source is never displayed as a frozen transition image.
        // The idle wait above retires GPU reads before releasing its lease.
        s.nv12_storage=nullptr;s.direct_frame.reset();s.have_frame=false;
        if(projection_renderer_)projection_renderer_->invalidate(i);
    }
    // Only the media owner requests this barrier. Release acknowledges that
    // no old decoder mapping remains in the XR surfaces or in-flight reads.
    decoder_frames_retire_requested_.store(false,std::memory_order_release);
}

void XrDisplay::Impl::stage_desktop_nv12(Surface& s,const VideoFrame& f,const XrPanelSource& source,
                                      unsigned panel,std::shared_ptr<const VideoFrame> owner) {
    const auto before=Clock::now();
    auto* storage=dynamic_cast<VulkanNv12FrameStorage*>(f.nv12_storage.get());
    if(owner.get()!=&f||f.packed_source||!storage||storage->device()!=device_||!valid_nv12_storage(*storage)||
       storage->width!=f.width||storage->height!=f.height||!f.width||!f.height||!f.generation)
        throw std::runtime_error("Invalid NV12 desktop source");
    const auto c=source.crop;
    const unsigned x=std::min(unsigned(c.x*f.width),f.width-1),y=std::min(unsigned(c.y*f.height),f.height-1);
    const unsigned w=std::clamp(unsigned(c.width*f.width),1u,f.width-x),h=std::clamp(unsigned(c.height*f.height),1u,f.height-y);
    if(w>max_width_||h>max_height_||w>max_image_||h>max_image_)throw std::runtime_error("Panel exceeds graphics image limits");
    const auto old=s.generation;
    const bool continuous=s.have_frame&&s.width==w&&s.height==h;
    if(s.width!=w||s.height!=h||!s.swapchain){
        clear_surface(s);s.width=w;s.height=h;create_swapchain(s);
    }
    // Projection completes every desktop submit before the next stage. This
    // immutable owner keeps CAPTURE out of QBUF through conversion's fence.
    if(s.desktop_nv12_pending)throw std::runtime_error("Desktop NV12 conversion still pending");
    s.direct_frame=std::move(owner);s.nv12_storage=storage;s.sampled_storage=nullptr;s.direct_buffer=VK_NULL_HANDLE;
    s.sample_rect={float(x)/f.width,float(y)/f.height,float(w)/f.width,float(h)/f.height};
    s.have_frame=true;s.generation=f.generation;s.decoded_ns=f.decoded_monotonic_ns;s.serial=++content_serial_;
    ui_.set_aspect(panel,float(h)/w,0);
    if(continuous&&f.generation>old)stats_.skipped_generations+=f.generation-old-1;
    ++stats_.new_generations;stats_.last_generation=f.generation;stats_.width=f.width;stats_.height=f.height;
    const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-before).count();
    ++stats_.desktop_stage_calls;stats_.desktop_stage_ms_sum+=ms;stats_.desktop_stage_ms_max=std::max(stats_.desktop_stage_ms_max,ms);
}

void XrDisplay::Impl::convert_desktop_nv12(const std::vector<Acquired>& acquired,const std::vector<unsigned>& projected) {
    for(const auto& a:acquired){auto& s=*a.surface;
        // VR NV12 sources have no acquired source swapchain. Only desktop
        // surfaces can carry this storage alongside an acquired target.
        if(!s.nv12_storage||!needs_upload(a))continue;
        if(desktop_nv12_fused_&&std::any_of(projected.begin(),projected.end(),
            [&](unsigned id){return &surfaces_[id]==&s;}))continue;
        if(!s.desktop_nv12_renderer)s.desktop_nv12_renderer=std::make_unique<FoveationRenderer>(
            physical_,device_,queue_,queue_family_,format_,s.width,s.height);
        auto& storage=*s.nv12_storage;
        std::array<FoveationEye,2> eyes{};eyes[1].dirty=false;
        auto& eye=eyes[0];eye.source=storage.image(0);eye.source_view=storage.view(0);eye.target=s.views[a.index];
        eye.host_source=true;eye.host_source_initialized=storage.initialized();eye.foreign_source=storage.foreign();
        eye.chroma_source=storage.image(1);eye.chroma_view=storage.view(1);eye.yuv_coefficients=s.direct_frame->yuv_coefficients;
        eye.source_rect=s.sample_rect;eye.linear_layout=true;eye.desktop_border=true;
        s.desktop_nv12_pending=s.desktop_nv12_renderer->render(eyes,true);storage.mark_initialized();
    }
}

void XrDisplay::Impl::complete_desktop_nv12(const std::vector<Acquired>& acquired,bool covered) {
    for(const auto& a:acquired){auto& s=*a.surface;if(!s.desktop_nv12_pending)continue;
        s.desktop_nv12_renderer->complete(covered);s.desktop_nv12_pending=false;
        const auto ms=s.desktop_nv12_renderer->last_gpu_duration_ms();
        if(ms>=0){++stats_.desktop_nv12_gpu_samples;stats_.desktop_nv12_gpu_ms_sum+=ms;}
        s.image_serials[a.index]=s.serial;++stats_.upload_count;++stats_.desktop_uploads;
    }
}
} // namespace surfxr
