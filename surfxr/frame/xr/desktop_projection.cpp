#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::create_projection() {
    if(projection_renderer_)return;
    std::uint32_t count=0;
    xr_check(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"xrEnumerateViewConfigurationViews(count)");
    if(count!=2)throw std::runtime_error("Projection renderer requires two stereo views");
    std::array<XrViewConfigurationView,2> views;
    for(auto& view:views)view=xr_info<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW);
    xr_check(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,views.data()),"xrEnumerateViewConfigurationViews");
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical_,&props);
    unsigned width_limit=std::min({max_width_,max_image_,props.limits.maxFramebufferWidth,props.limits.maxViewportDimensions[0]}),
             height_limit=std::min({max_height_,max_image_,props.limits.maxFramebufferHeight,props.limits.maxViewportDimensions[1]});
    unsigned width=2160,height=2160;
    for(const auto& v:views){width=std::max(width,v.recommendedImageRectWidth);height=std::max(height,v.recommendedImageRectHeight);
        width_limit=std::min(width_limit,v.maxImageRectWidth);height_limit=std::min(height_limit,v.maxImageRectHeight);}
    // A desktop occupies only part of the full eye FOV. Double linear
    // resolution before clamping to retain text density; use spatial
    // supersampling plus analytical edge coverage instead of 4x MSAA.
    width=static_cast<unsigned>(std::min<std::uint64_t>(std::uint64_t{width}*2,width_limit));
    height=static_cast<unsigned>(std::min<std::uint64_t>(std::uint64_t{height}*2,height_limit));
    if(!width||!height)throw std::runtime_error("Invalid projection view limits");
    auto& s=projection_surface_;s.width=width;s.height=height;
    auto ci=xr_info<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;ci.format=format_;ci.sampleCount=1;
    ci.width=width;ci.height=height;ci.faceCount=ci.mipCount=1;ci.arraySize=2;
    xr_check(xrCreateSwapchain(session_,&ci,&s.swapchain),"xrCreateSwapchain(projection)");
    xr_check(xrEnumerateSwapchainImages(s.swapchain,0,&count,nullptr),"xrEnumerateSwapchainImages(projection count)");
    if(!count||count>64)throw std::runtime_error("Invalid projection swapchain image count");
    s.images.assign(count,xr_info<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
    xr_check(xrEnumerateSwapchainImages(s.swapchain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data())),"xrEnumerateSwapchainImages(projection)");
    s.images.resize(count);
    for(const auto& image:s.images)for(unsigned eye=0;eye<2;++eye){
        auto vi=vk_info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);vi.image=image.image;
        vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format_;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,eye,1};
        VkImageView view=VK_NULL_HANDLE;vk_check(vkCreateImageView(device_,&vi,nullptr,&view),"vkCreateImageView(projection eye)");s.views.push_back(view);
    }
    projection_renderer_=std::make_unique<ProjectionRenderer>(physical_,device_,queue_,queue_family_,format_,width,height,1,61,mip_format_list_enabled_);
    stats_.projection_mesh_enabled=true;stats_.projection_width=width;stats_.projection_height=height;
    stats_.projection_msaa_samples=projection_renderer_->samples();
}

bool XrDisplay::Impl::project(const std::vector<unsigned>& ids,XrTime time,std::vector<Acquired>& acquired,
             const std::atomic_bool& stop,std::array<XrCompositionLayerProjectionView,2>& out) {
    if(ids.empty())return false;
    auto locate=xr_info<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime=time;locate.space=space_;auto state=xr_info<XrViewState>(XR_TYPE_VIEW_STATE);
    std::array<XrView,2> views;for(auto& v:views)v=xr_info<XrView>(XR_TYPE_VIEW);
    std::uint32_t count=0;xr_check(xrLocateViews(session_,&locate,&state,2,&count,views.data()),"xrLocateViews(projection)");
    constexpr auto valid=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if(count!=2||(state.viewStateFlags&valid)!=valid)return false;
    create_projection();
    std::vector<ProjectionSource> sources;
    std::vector<VulkanNv12FrameStorage*> consumed;
    for(unsigned id:ids){auto& s=surfaces_[id];const auto found=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&s;});
        ProjectionSource input{};input.panel=ui_.panels()[id];input.width=s.width;input.height=s.height;input.cache_slot=id;input.content_generation=s.serial;
        sources.push_back(input);
        if(desktop_nv12_fused_&&s.nv12_storage){
            auto& storage=*s.nv12_storage;auto& source=sources.back();
            source.image=storage.image(0);source.view=storage.view(0);
            source.chroma_image=storage.image(1);source.chroma_view=storage.view(1);
            source.source_rect=s.sample_rect;source.yuv_coefficients=s.direct_frame->yuv_coefficients;
            source.nv12_initialized=storage.initialized();source.nv12_foreign=storage.foreign();
            if(!projection_renderer_->can_reuse_source(id,s.width,s.height,s.serial,
                image_quality_.desktop_filter?ProjectionSampling::QualitySS:ProjectionSampling::Baseline))consumed.push_back(&storage);
        }else{
            if(found==acquired.end())throw std::runtime_error("Missing acquired projection texture");
            sources.back().image=s.images[found->index].image;sources.back().view=s.views[found->index];
        }
        stats_.curve_segments[id]=ui_.panels()[id].curvature_radians>.001f?64:0;}
    acquire(projection_surface_,acquired,stop);const auto index=acquired.back().index;
    std::array<ProjectionEye,2> eyes;
    for(unsigned e=0;e<2;++e){const auto& v=views[e];eyes[e]={projection_surface_.views[index*2+e],ui_pose(v.pose),{v.fov.angleLeft,v.fov.angleRight,v.fov.angleUp,v.fov.angleDown}};
        out[e]=xr_info<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);out[e].pose=v.pose;out[e].fov=v.fov;
        out[e].subImage.swapchain=projection_surface_.swapchain;out[e].subImage.imageArrayIndex=e;
        out[e].subImage.imageRect.extent={static_cast<std::int32_t>(projection_surface_.width),static_cast<std::int32_t>(projection_surface_.height)};}
    const auto previous_sampling=projection_renderer_->statistics();
    projection_renderer_->render(sources,eyes,image_quality_.desktop_filter?ProjectionSampling::QualitySS:ProjectionSampling::Baseline,stats_.waited_frames);
    for(auto* storage:consumed)storage->mark_initialized();
    const auto sampling=projection_renderer_->statistics();
    const auto fused_updates=sampling.nv12_mip_updates-previous_sampling.nv12_mip_updates;
    const auto base_updates=sampling.nv12_base_updates-previous_sampling.nv12_base_updates;
    stats_.desktop_nv12_base_updates+=base_updates;
    stats_.desktop_nv12_base_reuses+=sampling.nv12_base_reuses-previous_sampling.nv12_base_reuses;
    stats_.desktop_nv12_encoded_base_updates+=sampling.nv12_encoded_base_updates-previous_sampling.nv12_encoded_base_updates;
    stats_.desktop_nv12_encoded_mip_updates+=sampling.nv12_encoded_mip_updates-previous_sampling.nv12_encoded_mip_updates;
    stats_.desktop_nv12_mip_updates+=fused_updates;stats_.desktop_uploads+=fused_updates+base_updates;stats_.upload_count+=fused_updates+base_updates;
    stats_.desktop_nv12_intermediate_bytes_avoided+=sampling.nv12_intermediate_bytes_avoided-previous_sampling.nv12_intermediate_bytes_avoided;
    // Accumulate successful render deltas, so replacing a renderer never
    // resets the XR session's monotonic counters or prior maxima.
    stats_.desktop_quality_frames+=sampling.quality_frames-previous_sampling.quality_frames;
    stats_.desktop_mip_updates+=sampling.mip_updates-previous_sampling.mip_updates;
    stats_.desktop_mip_reuses+=sampling.mip_reuses-previous_sampling.mip_reuses;
    stats_.desktop_gpu_timestamps=sampling.gpu_timestamps;
    stats_.desktop_mip_gpu_samples+=sampling.mip_gpu_samples-previous_sampling.mip_gpu_samples;
    stats_.desktop_projection_gpu_samples+=sampling.projection_gpu_samples-previous_sampling.projection_gpu_samples;
    stats_.desktop_mip_gpu_ms_sum+=sampling.mip_gpu_ms_sum-previous_sampling.mip_gpu_ms_sum;
    stats_.desktop_projection_gpu_ms_sum+=sampling.projection_gpu_ms_sum-previous_sampling.projection_gpu_ms_sum;
    stats_.desktop_mip_gpu_ms_max=std::max(stats_.desktop_mip_gpu_ms_max,sampling.mip_gpu_ms_max);
    stats_.desktop_projection_gpu_ms_max=std::max(stats_.desktop_projection_gpu_ms_max,sampling.projection_gpu_ms_max);
    const auto ms=projection_renderer_->last_gpu_wait_ms();
    stats_.projection_gpu_wait_ms_sum+=ms;stats_.projection_gpu_wait_ms_max=std::max(stats_.projection_gpu_wait_ms_max,ms);
    ++stats_.gpu_wait_calls;stats_.gpu_wait_ms_sum+=ms;stats_.gpu_wait_ms_max=std::max(stats_.gpu_wait_ms_max,ms);
    ++stats_.projection_frames;return true;
}

} // namespace surfxr
