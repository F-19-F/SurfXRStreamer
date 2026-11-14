#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::clear_surface(Surface& s) noexcept {
    if(device_)vkDeviceWaitIdle(device_);
    s.desktop_nv12_renderer.reset(); // cached framebuffers precede target views
    if(s.mapped)vkUnmapMemory(device_,s.memory);
    if(s.buffer)vkDestroyBuffer(device_,s.buffer,nullptr);
    if(s.memory)vkFreeMemory(device_,s.memory,nullptr);
    for(auto view:s.views)if(view)vkDestroyImageView(device_,view,nullptr);
    for(auto view:s.raw_views)if(view)vkDestroyImageView(device_,view,nullptr);
    if(s.swapchain)xrDestroySwapchain(s.swapchain);
    s=Surface{};
}

void XrDisplay::Impl::stage(Surface& s,const VideoFrame& f,const XrPanelSource* source,unsigned panel,
           std::shared_ptr<const VideoFrame> owner) {
    if(copy_pending_)throw std::runtime_error("Cannot overwrite pending upload staging");
    if(s.have_frame&&s.generation==f.generation)return;
    const bool vr_source=&s==&vr_surfaces_[0]||&s==&vr_surfaces_[1]||&s==&vr_focus_surfaces_[0]||&s==&vr_focus_surfaces_[1];
    const auto stage_begin=source||vr_source?Clock::now():Clock::time_point{};
    if(s.have_frame&&f.generation<s.generation)throw std::runtime_error("Decoded frame generation regressed");
    const auto& pixels=f.packed_source?*f.packed_source:f;
    if(pixels.nv12_storage){
        if(source){stage_desktop_nv12(s,f,*source,panel,std::move(owner));return;}
        auto* storage=dynamic_cast<VulkanNv12FrameStorage*>(pixels.nv12_storage.get());
        if(!vr_source||source||owner.get()!=&f||!storage||storage->device()!=device_||!f.width||!f.height||
           f.width>max_width_||f.height>max_height_||!f.generation||!valid_nv12_storage(*storage)||
           storage->width!=pixels.width||storage->height!=pixels.height||
           !f.valid_packed_region())
            throw std::runtime_error("Invalid NV12 VR source");
        if(s.swapchain||s.width!=f.width||s.height!=f.height){complete_vr_render();clear_surface(s);s.width=f.width;s.height=f.height;}
        s.sampled_storage=nullptr;s.nv12_storage=storage;s.direct_buffer=VK_NULL_HANDLE;s.direct_frame=std::move(owner);
        s.sample_rect=f.source_rect();
        s.have_frame=true;s.generation=f.generation;s.decoded_ns=f.decoded_monotonic_ns;s.serial=++content_serial_;
        ++stats_.vr_direct_sample_frames;stats_.vr_upload_avoided_bytes+=std::uint64_t{f.width}*f.height*4;
        const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-stage_begin).count();
        ++stats_.vr_stage_calls;stats_.vr_stage_ms_sum+=ms;stats_.vr_stage_ms_max=std::max(stats_.vr_stage_ms_max,ms);return;
    }
    if(!f.width||!f.height||f.width>8192||f.height>8192||!f.generation||pixels.stride<std::uint64_t{pixels.width}*4||
       std::uint64_t{pixels.stride}*pixels.height!=pixels.rgba_size()||pixels.rgba_size()>256ULL*1024*1024||!pixels.rgba_data()||
       (f.packed_source&&(!vr_source||source||!f.valid_packed_region())))
        throw std::runtime_error("Invalid RGBA frame layout");
    const auto crop=source?source->crop:UiCrop{};
    const auto x=std::min(static_cast<unsigned>(crop.x*f.width),f.width-1);
    const auto y=std::min(static_cast<unsigned>(crop.y*f.height),f.height-1);
    const auto w=std::clamp(static_cast<unsigned>(crop.width*f.width),1u,f.width-x);
    const auto h=std::clamp(static_cast<unsigned>(crop.height*f.height),1u,f.height-y);
    auto* storage=dynamic_cast<VulkanRgbaFrameStorage*>(pixels.rgba_storage.get());
    const bool mapped_owner=owner.get()==&f&&storage&&storage->device()==device_&&format_==VK_FORMAT_R8G8B8A8_SRGB;
    const bool direct_vr=vr_source&&!source&&mapped_owner&&f.stride==std::uint64_t{w}*4&&w==f.width&&h==f.height;
    const bool direct_desktop=source&&mapped_owner&&f.stride%4==0&&w>4&&h>4;
    const bool direct=direct_vr||direct_desktop;
    if(w>max_width_||h>max_height_||w>max_image_||h>max_image_)throw std::runtime_error("Panel exceeds graphics image limits");
    const auto old_generation=s.generation;
    const bool continuous=s.have_frame&&s.width==w&&s.height==h;
    const bool sampled=direct_vr&&storage->prepare_sampled_image(physical_,pixels.width,pixels.height);
    if(sampled){
        // This source is never submitted as an XR layer. Sampling its mapped
        // decoder allocation removes both the upload and source swapchain.
        if(s.swapchain||s.width!=w||s.height!=h){
            // A temporary ordinary pool frame can use the upload fallback.
            // Retire its reconstruction before replacing source resources.
            complete_vr_render();clear_surface(s);s.width=w;s.height=h;
        }
    }else if(s.width!=w||s.height!=h||!s.swapchain){
        clear_surface(s);s.width=w;s.height=h;create_swapchain(s);
    }
    // Keep the full frame: the decoder pool must not overwrite its pixels.
    // Previous uploads have completed before stage(); directly sampled old
    // frames additionally live in vr_pending_source_frames_ until rendering
    // completes, so replacing this surface owner cannot recycle them early.
    s.sampled_storage=sampled?storage:nullptr;
    s.nv12_storage=nullptr;
    s.sample_rect=sampled?f.source_rect():std::array<float,4>{0,0,1,1};
    s.direct_buffer=direct&&!sampled?storage->buffer():VK_NULL_HANDLE;
    s.direct_frame=direct?std::move(owner):std::shared_ptr<const VideoFrame>{};
    s.edge_plan={};s.direct_offset=0;s.direct_row_length=0;
    const auto copy_begin=source||vr_source?Clock::now():Clock::time_point{};
    std::size_t cpu_copy_bytes=0;
    if(sampled){++stats_.vr_direct_sample_frames;stats_.vr_upload_avoided_bytes+=std::uint64_t{w}*h*4;}
    else if(direct_vr){++stats_.vr_direct_upload_frames;stats_.vr_direct_upload_bytes+=std::uint64_t{w}*h*4;
        if(f.packed_source){s.direct_offset=std::uint64_t{f.source_y()}*pixels.stride+std::uint64_t{f.source_x()}*4;s.direct_row_length=pixels.stride/4;}}
    else if(direct_desktop){
        const auto plan=rgba_upload_edge_plan(w,h);create_transfer(s,plan.bytes);
        s.edge_plan=rgba_upload_edge_copy(static_cast<std::uint8_t*>(s.mapped),
            f.rgba_data()+std::size_t{y}*f.stride+std::size_t{x}*4,f.stride,w,h);
        s.direct_offset=std::uint64_t{y+2}*f.stride+std::uint64_t{x+2}*4;s.direct_row_length=f.stride/4;
        cpu_copy_bytes=s.edge_plan.bytes;
        ++stats_.desktop_direct_upload_frames;stats_.desktop_direct_upload_bytes+=std::uint64_t{w-4}*(h-4)*4;
        stats_.desktop_edge_patch_bytes+=cpu_copy_bytes;
    }
    else{
        create_transfer(s);
        rgba_upload_copy(static_cast<std::uint8_t*>(s.mapped),
                         pixels.rgba_data()+std::size_t{y+f.source_y()}*pixels.stride+std::size_t{x+f.source_x()}*4,
                         pixels.stride,w,h,format_==VK_FORMAT_B8G8R8A8_SRGB,source!=nullptr);
        cpu_copy_bytes=std::size_t{w}*h*4;
    }
    if(vr_source&&!direct){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-copy_begin).count();
        stats_.vr_stage_copy_ms_sum+=ms;stats_.vr_stage_copy_ms_max=std::max(stats_.vr_stage_copy_ms_max,ms);}
    if(source){
        const auto copy_ms=std::chrono::duration<double,std::milli>(Clock::now()-copy_begin).count();
        stats_.desktop_stage_copy_bytes+=cpu_copy_bytes;
        stats_.desktop_stage_copy_ms_sum+=copy_ms;
        stats_.desktop_stage_copy_ms_max=std::max(stats_.desktop_stage_copy_ms_max,copy_ms);
        if(s.cached)++stats_.desktop_stage_cached_calls;
        if(s.coherent)++stats_.desktop_stage_coherent_calls;
        ui_.set_aspect(panel,static_cast<float>(h)/w,0);
        // Generation counters survive a decoder's mode changes. The first
        // frame after availability/size reset has no previous visible frame.
        if(continuous&&f.generation>old_generation)stats_.skipped_generations+=f.generation-old_generation-1;
        ++stats_.new_generations;stats_.last_generation=f.generation;stats_.width=f.width;stats_.height=f.height;
    }
    if(!direct_vr&&!s.coherent){const auto flush_begin=source?Clock::now():Clock::time_point{};
        auto range=vk_info<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);range.memory=s.memory;range.size=VK_WHOLE_SIZE;
        vk_check(vkFlushMappedMemoryRanges(device_,1,&range),"vkFlushMappedMemoryRanges");
        if(source){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-flush_begin).count();
            stats_.desktop_stage_flush_ms_sum+=ms;stats_.desktop_stage_flush_ms_max=std::max(stats_.desktop_stage_flush_ms_max,ms);}}
    s.have_frame=true;s.generation=f.generation;s.decoded_ns=f.decoded_monotonic_ns;s.serial=++content_serial_;
    if(vr_source){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-stage_begin).count();
        ++stats_.vr_stage_calls;stats_.vr_stage_ms_sum+=ms;stats_.vr_stage_ms_max=std::max(stats_.vr_stage_ms_max,ms);}
    if(source){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-stage_begin).count();
        ++stats_.desktop_stage_calls;stats_.desktop_stage_ms_sum+=ms;
        stats_.desktop_stage_ms_max=std::max(stats_.desktop_stage_ms_max,ms);}
}

std::uint64_t XrDisplay::Impl::predicted_monotonic_ns(XrTime target) {
    if(!timespec_to_xr_)return 0;
    timespec ts{};const auto before=Clock::now();
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    const auto after=Clock::now();XrTime current=0;
    if(timespec_to_xr_(instance_,&ts,&current)!=XR_SUCCESS||target<=0||current<=0)return 0;
    const auto delta=target-current;
    if(delta < -100'000'000||delta > 100'000'000)return 0;
    const auto now=clock_ns(before+(after-before)/2);
    if(delta<0&&std::uint64_t(-delta)>now)return 0;
    return delta<0?now-std::uint64_t(-delta):now+std::uint64_t(delta);
}

} // namespace surfxr
