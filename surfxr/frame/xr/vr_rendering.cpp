#include "display_private.hpp"
#include "surfxr/localization.hpp"

namespace surfxr {
bool XrDisplay::Impl::valid_vr_pair(const VrStereoFrame& pair) const {
    return stage_space_&&valid_vr_stereo_frame(pair,stage_epoch_);
}

void XrDisplay::Impl::invalidate_vr_reuse(){
    if(vr_released_pair_)++stats_.vr_reuse_cache_resets;
    vr_released_pair_.reset();
}

bool XrDisplay::Impl::reusable_vr_pair(const std::shared_ptr<const VrStereoFrame>& pair) const {
    // Keep an owning reference: pointer equality cannot be fooled by a
    // freed/reallocated pair, and metadata/eye pixels are immutable.
    if(!pair||pair!=vr_released_pair_||pair->origin_epoch!=stage_epoch_
       ||!have_vr_timestamp_||pair->metadata->timestamp!=stats_.vr_last_timestamp)return false;
    const auto extent=vr_reconstruction_extent(*pair);
    for(unsigned eye=0;eye<2;++eye){const auto& input=vr_surfaces_[eye];const auto& output=vr_unwarped_surfaces_[eye];const auto& frame=*pair->eyes[eye];
        if(!input.have_frame||input.generation!=frame.generation||input.width!=frame.width||input.height!=frame.height
           ||!output.swapchain||output.width!=extent[0]||output.height!=extent[1])return false;
    }
    return true;
}

void XrDisplay::Impl::vr_projection_views(const VrStereoFrame& pair,std::array<XrCompositionLayerProjectionView,2>& views){
    for(unsigned eye=0;eye<2;++eye){auto& view=views[eye];const auto& target=vr_enhancement_active_&&!vr_fused_sharpen_?vr_enhanced_surfaces_[eye]:vr_unwarped_surfaces_[eye];
        view=xr_info<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
        view.pose=xr_pose(pair.metadata->eye_poses[eye]);const auto fov=vr_eye_fov_radians(pair.metadata->fov[eye]);view.fov={fov[0],fov[1],fov[2],fov[3]};
        view.subImage.swapchain=target.swapchain;view.subImage.imageRect.extent={static_cast<std::int32_t>(target.width),static_cast<std::int32_t>(target.height)};
    }
}

void XrDisplay::Impl::stage_transition(bool vr_requested) {
    const unsigned kind=vr_requested?2u:1u;
    const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    const auto phase=StartupHud::animation_phase(static_cast<std::uint64_t>(now));
    const auto language=static_cast<unsigned>(i18n::system_language());
    if(transition_surface_.have_frame&&transition_card_kind_==kind&&transition_animation_phase_==phase&&transition_language_==language)return;
    stage(transition_surface_,StartupHud::raster(vr_requested,++transition_generation_,phase));
    transition_card_kind_=kind;transition_animation_phase_=phase;transition_language_=language;
}

void XrDisplay::Impl::reset_vr_surfaces(){
    complete_vr_render();
    invalidate_vr_reuse();
    have_vr_submission_time_=false;
    // Renderer framebuffers retain target views; destroy them first.
    foveation_renderer_.reset();
    enhancement_renderer_.reset();
    for(auto& s:vr_enhanced_surfaces_)clear_surface(s);
    vr_enhancement_active_=vr_upscale_active_=vr_fused_sharpen_=false;
    for(auto& s:vr_unwarped_surfaces_)clear_surface(s);
    for(auto& s:vr_surfaces_)clear_surface(s);
    for(auto& s:vr_focus_surfaces_)clear_surface(s);
    vr_foveated_=false;vr_focus_rects_={};
    stats_.vr_focus_width=stats_.vr_focus_height=0;
    stats_.vr_focus_center_x={};stats_.vr_focus_center_y={};
    have_vr_timestamp_=false;
}

void XrDisplay::Impl::stage_vr(const VrStereoFrame& pair,std::vector<Acquired>& acquired,const std::atomic_bool& stop,
              std::array<XrCompositionLayerProjectionView,2>& views){
    const bool different=!have_vr_timestamp_||pair.metadata->timestamp!=stats_.vr_last_timestamp;
    const auto canvas=vr_reconstruction_extent(pair);
    const auto width=canvas[0],height=canvas[1];
    if(!width||!height||width>8192||height>8192||width>max_width_||height>max_height_||width>max_image_||height>max_image_)
        throw std::runtime_error("Invalid VR reconstruction dimensions");
    const auto plan=make_postprocess_plan(pair.eyes[0]->width,pair.eyes[0]->height,width,height,
        {image_quality_.vr_upscaling,image_quality_.vr_sharpening_enabled?image_quality_.vr_sharpening/100.f:0.f},
        std::min(max_width_,max_image_),std::min(max_height_,max_image_));
    // Gamma2 fused sharpening writes linear color to the sRGB attachment.
    // Only a separate upscale pass needs compatible encoded-source views.
    bool recreate=vr_unwarped_surfaces_[0].width!=width||vr_unwarped_surfaces_[0].height!=height
        ||vr_unwarped_surfaces_[0].raw_requested!=plan.upscale_active||vr_foveated_!=pair.foveated;
    for(unsigned eye=0;eye<2;++eye){const auto& s=vr_surfaces_[eye];const auto& f=*pair.eyes[eye];
        recreate|=s.have_frame&&((different&&f.generation<=s.generation)||s.width!=f.width||s.height!=f.height);}
    if(pair.foveated)for(unsigned eye=0;eye<2;++eye){const auto& s=vr_focus_surfaces_[eye];const auto& f=*pair.focus[eye];
        recreate|=s.have_frame&&((different&&f.generation<=s.generation)||s.width!=f.width||s.height!=f.height);}
    if(recreate)reset_vr_surfaces();
    vr_foveated_=pair.foveated;
    vr_linear_layout_=pair.metadata->linear_layout;
    if(!foveation_renderer_){
        for(auto& target:vr_unwarped_surfaces_){target.width=width;target.height=height;create_swapchain(target,plan.upscale_active);}
        // create_vulkan() negotiates Vulkan 1.1 or newer. The renderer still
        // checks fragment subgroup capabilities before loading its quad shader.
        foveation_renderer_=std::make_unique<FoveationRenderer>(physical_,device_,queue_,queue_family_,format_,width,height,true,true,FoveationSharpening::Gamma2);
    }
    vr_postprocess_plan_=plan;
    const auto& extent=vr_postprocess_plan_.output;
    vr_upscale_active_=vr_postprocess_plan_.upscale_active;
    vr_enhancement_active_=vr_postprocess_plan_.enhancement_active;
    vr_fused_sharpen_=vr_postprocess_plan_.fused_sharpen;
    if(vr_enhancement_active_&&!vr_fused_sharpen_){
        if(enhancement_renderer_&&(vr_enhanced_surfaces_[0].width!=extent.width||vr_enhanced_surfaces_[0].height!=extent.height)){
            complete_vr_render();
            enhancement_renderer_.reset();for(auto& target:vr_enhanced_surfaces_)clear_surface(target);
        }
        if(!enhancement_renderer_){
            for(auto& target:vr_enhanced_surfaces_){target.width=extent.width;target.height=extent.height;create_swapchain(target,true);}
            enhancement_renderer_=std::make_unique<ImageEnhancementRenderer>(physical_,device_,queue_,queue_family_,format_,width,height,extent.width,extent.height);
        }
    }
    stats_.vr_enhanced_width=vr_enhancement_active_?extent.width:width;
    stats_.vr_enhanced_height=vr_enhancement_active_?extent.height:height;
    for(unsigned eye=0;eye<2;++eye){auto& s=vr_surfaces_[eye];const auto& f=*pair.eyes[eye];
        stage(s,f,nullptr,0,pair.eyes[eye]);
        if(!s.sampled_storage&&!s.nv12_storage)acquire(s,acquired,stop);
        if(pair.foveated){auto& focus=vr_focus_surfaces_[eye];const auto& crop=*pair.focus[eye];
            stage(focus,crop,nullptr,0,pair.focus[eye]);
            if(!focus.sampled_storage&&!focus.nv12_storage)acquire(focus,acquired,stop);
            vr_focus_rects_[eye]={float(pair.metadata->regions[eye].x),float(pair.metadata->regions[eye].y),float(crop.width),float(crop.height)};
            stats_.vr_focus_width=crop.width;stats_.vr_focus_height=crop.height;
            stats_.vr_focus_center_x[eye]=(vr_focus_rects_[eye][0]+crop.width*.5f)/width;
            stats_.vr_focus_center_y[eye]=(vr_focus_rects_[eye][1]+crop.height*.5f)/height;
        }
        auto& target=vr_unwarped_surfaces_[eye];target.serial=f.generation;
        acquire(target,acquired,stop);
        if(vr_enhancement_active_&&!vr_fused_sharpen_){auto& enhanced=vr_enhanced_surfaces_[eye];enhanced.serial=f.generation;acquire(enhanced,acquired,stop);}
        stats_.vr_eye_width[eye]=width;stats_.vr_eye_height[eye]=height;
        stats_.vr_decoded_eye_width[eye]=s.width;stats_.vr_decoded_eye_height[eye]=s.height;
    }
    vr_projection_views(pair,views);
}

void XrDisplay::Impl::record_foveation_completion(bool count_wait){
    const auto ms=foveation_renderer_->last_gpu_wait_ms();
    stats_.vr_foveation_gpu_wait_ms_sum+=ms;stats_.vr_foveation_gpu_wait_ms_max=std::max(stats_.vr_foveation_gpu_wait_ms_max,ms);
    if(count_wait||ms>0){++stats_.gpu_wait_calls;stats_.gpu_wait_ms_sum+=ms;stats_.gpu_wait_ms_max=std::max(stats_.gpu_wait_ms_max,ms);}
    const auto gpu_ms=foveation_renderer_->last_gpu_duration_ms();
    if(gpu_ms>=0){++stats_.vr_foveation_gpu_samples;stats_.vr_foveation_gpu_ms_sum+=gpu_ms;
        stats_.vr_foveation_gpu_ms_max=std::max(stats_.vr_foveation_gpu_ms_max,gpu_ms);}
}

void XrDisplay::Impl::observe_motion_feedback(const MotionFeedback& frame){
    motion_sync_.observe_display_frame(frame.epoch,frame.timestamp,frame.query_ns,frame.submitted_ns,frame.display_ns);
    const auto& sync=motion_sync_.statistics();
    stats_.motion_sync_updates=sync.updates;
    stats_.motion_sync_rejected_feedback=sync.rejected_invalid_time+sync.rejected_epoch+sync.rejected_duplicate+sync.rejected_out_of_order;
    stats_.motion_sync_latency_ms=sync.latency_seconds*1000.;stats_.motion_sync_previous_draw_ms=sync.previous_draw_seconds*1000.;
}

void XrDisplay::Impl::record_enhancement_completion(){
    const auto ms=enhancement_renderer_->last_gpu_wait_ms();
    stats_.vr_enhancement_gpu_wait_ms_sum+=ms;stats_.vr_enhancement_gpu_wait_ms_max=std::max(stats_.vr_enhancement_gpu_wait_ms_max,ms);
    ++stats_.gpu_wait_calls;stats_.gpu_wait_ms_sum+=ms;stats_.gpu_wait_ms_max=std::max(stats_.gpu_wait_ms_max,ms);
    const auto gpu_ms=enhancement_renderer_->last_gpu_duration_ms();
    if(gpu_ms>=0){++stats_.vr_enhancement_gpu_samples;stats_.vr_enhancement_gpu_ms_sum+=gpu_ms;
        stats_.vr_enhancement_gpu_ms_max=std::max(stats_.vr_enhancement_gpu_ms_max,gpu_ms);}
}

void XrDisplay::Impl::complete_vr_upload(){
    if(!vr_render_pending_||!copy_pending_)return;
    const auto before=Clock::now();
    complete_copy_batch(vr_pending_uploads_,false);
    const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-before).count();
    ++stats_.vr_upload_wait_calls;stats_.vr_upload_wait_ms_sum+=ms;
    stats_.vr_upload_wait_ms_max=std::max(stats_.vr_upload_wait_ms_max,ms);
    // Rendering uses the uploaded images, not these mapped CPU buffers.
    // Clear the old acquisition list before staging changes surface serials.
    vr_pending_uploads_.clear();
}

void XrDisplay::Impl::complete_vr_render(){
    if(!vr_render_pending_)return;
    if(vr_pending_enhancement_){enhancement_renderer_->complete(false);record_enhancement_completion();}
    if(vr_pending_reconstruction_){
        foveation_renderer_->complete(vr_pending_enhancement_);
        record_foveation_completion(!vr_pending_enhancement_);
    }
    if(vr_pending_reconstruction_){
        vr_pacer_.observe_gpu(foveation_renderer_->last_gpu_duration_ms(),
            vr_pending_enhancement_?enhancement_renderer_->last_gpu_duration_ms():0.);
    }
    // Reset/exit may retire both together. In normal frames the earlier
    // upload was already retired before overlapping CPU staging with this
    // reconstruction. No new upload may be submitted until this completes.
    complete_copy_batch(vr_pending_uploads_,true);
    vr_pending_uploads_.clear();vr_pending_source_frames_={};vr_render_pending_=false;
    vr_pending_reconstruction_=vr_pending_enhancement_=false;
    ++stats_.vr_deferred_gpu_completions;
}

bool XrDisplay::Impl::reconstruct_vr(const std::vector<Acquired>& acquired,bool allow_deferred){
    std::array<FoveationEye,2> eyes{};std::array<std::uint32_t,2> target_indices{};
    for(unsigned eye=0;eye<2;++eye){auto& source=vr_surfaces_[eye];auto& target=vr_unwarped_surfaces_[eye];
        const auto src=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&source;});
        const auto dst=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&target;});
        if(dst==acquired.end())throw std::runtime_error("Missing VR reconstruction target");
        const bool dirty=vr_timewarp_reconstruct_||target.image_serials[dst->index]!=target.serial;
        if(source.nv12_storage){
            auto& storage=*source.nv12_storage;
            eyes[eye]={storage.image(0),storage.view(0),target.views[dst->index],dirty};
            eyes[eye].chroma_source=storage.image(1);eyes[eye].chroma_view=storage.view(1);
            eyes[eye].host_source=true;eyes[eye].host_source_initialized=storage.initialized();
            eyes[eye].foreign_source=storage.foreign();
            eyes[eye].yuv_coefficients=source.direct_frame->yuv_coefficients;
            vr_pending_source_frames_[eye]=source.direct_frame;
        }else if(source.sampled_storage){
            auto& storage=*source.sampled_storage;
            eyes[eye]={storage.sampled_image(),storage.sampled_view(),target.views[dst->index],dirty};
            eyes[eye].host_source=true;eyes[eye].host_source_initialized=storage.sampled_image_initialized();
            // Retain the frame, not only its storage: the decoder pool uses
            // frame ownership to determine whether CPU writes are safe.
            vr_pending_source_frames_[eye]=source.direct_frame;
        }else{
            if(src==acquired.end())throw std::runtime_error("Missing VR reconstruction source");
            eyes[eye]={source.images[src->index].image,source.views[src->index],target.views[dst->index],dirty};
        }
        if(vr_fused_sharpen_&&foveation_renderer_->supports_raw_views()&&!target.raw_views.empty())eyes[eye].raw_target=target.raw_views[dst->index];
        eyes[eye].timewarp_rotation=vr_timewarp_[eye].rotation;eyes[eye].timewarp_fov=vr_timewarp_[eye].fov;
        eyes[eye].linear_layout=vr_linear_layout_;
        eyes[eye].source_rect=source.sample_rect;
        if(!source.sampled_storage&&!source.nv12_storage)eyes[eye].source_rect={0,0,1,1};
        if(vr_foveated_){auto& focus=vr_focus_surfaces_[eye];auto& input=eyes[eye];input.focus_rect=vr_focus_rects_[eye];
            if(focus.nv12_storage){auto& storage=*focus.nv12_storage;
                if(source.nv12_storage!=focus.nv12_storage)throw std::runtime_error("Gaze NV12 planes must share their atlas");
                input.focus_source=storage.image(0);input.focus_view=storage.view(0);
                input.focus_host_source=true;input.focus_host_initialized=storage.initialized();input.focus_source_rect=focus.sample_rect;
                vr_pending_source_frames_[2+eye]=focus.direct_frame;
            }else if(focus.sampled_storage){auto& storage=*focus.sampled_storage;
                input.focus_source=storage.sampled_image();input.focus_view=storage.sampled_view();
                input.focus_host_source=true;input.focus_host_initialized=storage.sampled_image_initialized();
                input.focus_source_rect=focus.sample_rect;
                vr_pending_source_frames_[2+eye]=focus.direct_frame;
            }else{
                const auto crop=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&focus;});
                if(crop==acquired.end())throw std::runtime_error("Missing VR focus source");
                input.focus_source=focus.images[crop->index].image;input.focus_view=focus.views[crop->index];
            }
        }
        target_indices[eye]=dst->index;
    }
    // Without timestamps keep the synchronous feedback measurement rather
    // than silently training prediction with a missing GPU contribution.
    const bool separate_enhancement=vr_enhancement_active_&&!vr_fused_sharpen_;
    const bool defer_completion=allow_deferred&&foveation_renderer_->supports_gpu_timestamps()
        &&(!separate_enhancement||enhancement_renderer_->supports_gpu_timestamps());
    const bool reconstructed=foveation_renderer_->render(eyes,separate_enhancement||defer_completion,
        vr_fused_sharpen_?vr_postprocess_plan_.options.sharpness:0.f);
    if(reconstructed)for(unsigned eye=0;eye<2;++eye)
        if(eyes[eye].dirty){
            if(vr_surfaces_[eye].nv12_storage)vr_surfaces_[eye].nv12_storage->mark_initialized();
            else if(eyes[eye].host_source)vr_surfaces_[eye].sampled_storage->mark_sampled_image_initialized();
            if(eyes[eye].focus_host_source){auto& focus=vr_focus_surfaces_[eye];
                if(focus.nv12_storage)focus.nv12_storage->mark_initialized();else focus.sampled_storage->mark_sampled_image_initialized();}
        }
    bool enhanced=false;
    if(separate_enhancement){
        std::array<ImageEnhancementEye,2> processed{};std::array<std::uint32_t,2> indices{};
        for(unsigned eye=0;eye<2;++eye){auto& source=vr_unwarped_surfaces_[eye];auto& target=vr_enhanced_surfaces_[eye];
            const auto src=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&source;});
            const auto dst=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&target;});
            if(src==acquired.end()||dst==acquired.end())throw std::runtime_error("Missing enhanced VR image");
            processed[eye]={source.images[src->index].image,source.views[src->index],target.views[dst->index],vr_timewarp_reconstruct_||target.image_serials[dst->index]!=target.serial};indices[eye]=dst->index;
            if(enhancement_renderer_->supports_raw_views()){
                if(!source.raw_views.empty())processed[eye].raw_source_view=source.raw_views[src->index];
                if(!target.raw_views.empty())processed[eye].raw_target=target.raw_views[dst->index];
            }
        }
        enhanced=enhancement_renderer_->render(processed,vr_postprocess_plan_.options,defer_completion);
        if(enhanced){
            ++stats_.vr_enhancement_frames;
            if(vr_upscale_active_)++stats_.vr_upscaled_frames;
            if(processed[0].raw_source_view&&processed[1].raw_source_view&&processed[0].raw_target&&processed[1].raw_target)
                ++stats_.vr_raw_enhancement_frames;
            if(!defer_completion)record_enhancement_completion();
            for(unsigned eye=0;eye<2;++eye)if(processed[eye].dirty)vr_enhanced_surfaces_[eye].image_serials[indices[eye]]=vr_timewarp_reconstruct_?0:vr_enhanced_surfaces_[eye].serial;
        }
        if(!defer_completion)foveation_renderer_->complete(enhanced);
    }
    if(reconstructed){
        ++stats_.vr_foveation_frames;
        if(vr_fused_sharpen_){
            ++stats_.vr_fused_postprocess_frames;++stats_.vr_enhancement_frames;
            if(eyes[0].raw_target&&eyes[1].raw_target)++stats_.vr_raw_enhancement_frames;
        }
        // The final enhancement fence already accounts for the deferred
        // reconstruction. Count only an actual additional host wait.
        if(!defer_completion)record_foveation_completion(!enhanced);
        for(unsigned eye=0;eye<2;++eye)if(eyes[eye].dirty)vr_unwarped_surfaces_[eye].image_serials[target_indices[eye]]=vr_timewarp_reconstruct_?0:vr_unwarped_surfaces_[eye].serial;
    }
    if(defer_completion&&(reconstructed||enhanced)){
        vr_pending_uploads_=acquired;vr_render_pending_=true;
        vr_pending_reconstruction_=reconstructed;vr_pending_enhancement_=enhanced;
        ++stats_.vr_deferred_gpu_submissions;
    }
    if(!vr_render_pending_)vr_pending_source_frames_={};
    return reconstructed||enhanced;
}

} // namespace surfxr
