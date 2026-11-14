#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::frame(const std::atomic_bool& stop) {
    desktop_measuring_frame_=false;vr_measuring_frame_=false;vr_timewarp_reconstruct_=false;vr_timewarp_={};
    std::uint64_t pacing_wait_ns=0;
    // A streaming image can arrive after the runtime's usual wake phase.
    // Wait for it before entering the next OpenXR frame: the application is
    // then doing real input/render work between xrWaitFrame and xrEndFrame,
    // rather than teaching runtime timing estimation to reserve our sleep.
    if(stats_.last_should_render&&stats_.last_vr_pair_valid&&ui_.anchored()&&stage_locatable_&&
       vr_previous_display_period_>0&&callbacks_.vr_frame&&!exit_requested_&&
       !stop.load(std::memory_order_relaxed)){
        const auto candidate=callbacks_.vr_frame();
        if(candidate&&valid_vr_pair(*candidate)){
            pacing_wait_ns=wait_vr_presentation(vr_previous_wait_return_ns_,
                vr_previous_predicted_display_ns_,vr_previous_display_period_,*candidate,stop,true);
        }
    }
    const auto pre_wait_pacing_ns=pacing_wait_ns;
    auto wait=xr_info<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);auto state=xr_info<XrFrameState>(XR_TYPE_FRAME_STATE);
    const auto before=Clock::now();xr_check(xrWaitFrame(session_,&wait,&state),"xrWaitFrame");
    stats_.max_xr_wait_ms=std::max(stats_.max_xr_wait_ms,std::chrono::duration<double,std::milli>(Clock::now()-before).count());++stats_.waited_frames;
    const auto before_work=Clock::now();
    notify_renderable(state.shouldRender==XR_TRUE);
    // Vulkan enable2 allows xrReleaseSwapchainImage/xrEndFrame while
    // submitted work on the binding's queue is still executing. Only the
    // earlier upload must finish before overwriting mapped staging. Keep
    // reconstruction in flight while preparing the next CPU-side images;
    // its own fence is retired before reusing renderer command resources.
    // https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html
    complete_vr_upload();
    if(state.shouldRender)++stats_.renderable_frames;else ++stats_.skipped_not_renderable;
    if(state_==XR_SESSION_STATE_VISIBLE||state_==XR_SESSION_STATE_FOCUSED)++stats_.visible_frames;
    if(state_==XR_SESSION_STATE_FOCUSED)++stats_.focused_frames;
    auto begin=xr_info<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);const auto br=xrBeginFrame(session_,&begin);stats_.last_begin_result=static_cast<int>(br);
    if(br==XR_FRAME_DISCARDED)++stats_.begin_frame_discarded;
    xr_check(br,"xrBeginFrame");
    auto end=xr_info<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);end.displayTime=state.predictedDisplayTime;end.environmentBlendMode=blend_;
    bool performance_presented=false;
    std::vector<Acquired> acquired;acquired.reserve(surfaces_.size()+1);bool end_attempted=false;
    std::vector<Layer> composed;composed.reserve(72);
    std::vector<unsigned> presented,projected;
    bool transition_presented=false;
    std::optional<Layer> transition_layer;
    std::array<XrCompositionLayerProjectionView,2> projection_views{};
    auto projection=xr_info<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    projection.space=space_;projection.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    projection.viewCount=2;projection.views=projection_views.data();bool has_projection=false;
    std::array<XrCompositionLayerProjectionView,2> vr_views{};
    auto vr_layer=xr_info<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    vr_layer.space=stage_space_;vr_layer.viewCount=2;vr_layer.views=vr_views.data();bool has_vr=false,reused_vr=false;
    std::shared_ptr<const VrStereoFrame> vr;
    std::uint64_t vr_draw_begin_ns=0;
    std::uint64_t after_input_ns=0,after_stage_ns=0,after_render_ns=0;
    std::uint64_t predicted_display_ns=0,video_select_ns=0;
    const auto record_timing=[&](std::uint64_t before_end_ns,std::uint64_t after_end_ns,int result){
        if(!timing_.enabled())return;
        FrameTimingRecord record=shortcut_timing_;
        record.wait_return_ns=clock_ns(before_work);record.before_end_ns=before_end_ns;record.after_end_ns=after_end_ns;
        record.after_input_ns=after_input_ns;record.after_stage_ns=after_stage_ns;record.after_render_ns=after_render_ns;
        record.predicted_display_time=state.predictedDisplayTime;record.predicted_display_period=state.predictedDisplayPeriod;
        record.predicted_display_monotonic_ns=predicted_display_ns;record.video_select_ns=video_select_ns;
        record.pacing_wait_ns=pacing_wait_ns;record.pacing_enabled=vr_pacer_.enabled();
        record.pre_wait_pacing_ns=pre_wait_pacing_ns;
        record.result=result;record.should_render=state.shouldRender;record.has_vr=has_vr;record.reused=reused_vr;
        if(vr&&vr->metadata){
            const auto& metadata=*vr->metadata;
            record.source_timestamp_us=metadata.timestamp;record.origin_epoch=vr->origin_epoch;
            record.has_retained_first_primary_metadata=true;
            record.retained_first_primary_metadata_tracking_frame_id=metadata.tracking_frame_id;
            record.pair_ready_ns=vr->ready_monotonic_ns;
            if(vr->eyes[0])record.decoded_left_ns=vr->eyes[0]->decoded_monotonic_ns;
            if(vr->eyes[1])record.decoded_right_ns=vr->eyes[1]->decoded_monotonic_ns;
            if(vr->eyes[0]){record.packet_left_ns=vr->eyes[0]->packet_submit_monotonic_ns;record.conversion_left_ns=vr->eyes[0]->conversion_begin_monotonic_ns;}
            if(vr->eyes[1]){record.packet_right_ns=vr->eyes[1]->packet_submit_monotonic_ns;record.conversion_right_ns=vr->eyes[1]->conversion_begin_monotonic_ns;}
            record.foveated=vr->foveated;
            if(vr->focus[0]){record.decoded_focus_left_ns=vr->focus[0]->decoded_monotonic_ns;
                record.packet_focus_left_ns=vr->focus[0]->packet_submit_monotonic_ns;record.conversion_focus_left_ns=vr->focus[0]->conversion_begin_monotonic_ns;}
            if(vr->focus[1]){record.decoded_focus_right_ns=vr->focus[1]->decoded_monotonic_ns;
                record.packet_focus_right_ns=vr->focus[1]->packet_submit_monotonic_ns;record.conversion_focus_right_ns=vr->focus[1]->conversion_begin_monotonic_ns;}
            record.pose_query_ns=vr->pose_query_monotonic_ns;record.pose_query_xr_time=vr->pose_query_xr_time;
            record.pose_target_xr_time=vr->pose_target_xr_time;
        }
        timing_.record(record);
    };
    std::uint64_t curved_layers=0,control_layers=0;
    unsigned gaze_debug_eyes=0;
    try{
        if(callbacks_.vr_frame)vr=callbacks_.vr_frame();
        bool show_vr=vr&&valid_vr_pair(*vr);
        bool target_vr=callbacks_.settings&&callbacks_.settings().capabilities.vr_requested;
        if(!show_vr){invalidate_vr_reuse();have_vr_submission_time_=false;}
        if(vr&&!show_vr)++stats_.vr_rejected_stereo_frames;
        const auto apply_mode=[&](bool active){
            if(active!=vr_was_active_){ui_.release_inputs([&](const UiEvent& e){dispatch(e);});
                desktop_previous_submission_={};desktop_previous_new_submission_.fill(Clock::time_point{});
                if(active)reset_vr_surfaces();
                vr_was_active_=active;}
        };
        apply_mode(show_vr);
        std::array<std::shared_ptr<const VideoFrame>,4> snapshots;
        const auto select_sources=[&](bool vr_active){for(unsigned i=0;i<sources_.size();++i){
            const bool enabled=!sources_[i].available||sources_[i].available->load(std::memory_order_acquire);
            snapshots[i].reset();
            if(enabled&&!vr_active)snapshots[i]=sources_[i].frames->snapshot();
            if(snapshots[i]&&sources_[i].demand_epoch){const auto epoch=sources_[i].demand_epoch->load(std::memory_order_acquire);
                if(!epoch||snapshots[i]->output_epoch!=epoch)snapshots[i].reset();}
            // A configured hidden panel remains selectable in SCREEN while
            // its demand-gated pixels are absent. Showing it still waits for
            // a fresh snapshot before rendering or accepting panel input.
            const bool available=enabled&&(snapshots[i]||!ui_.panels()[i].visible);
            stats_.desktop_decoded_width[i]=snapshots[i]?snapshots[i]->width:0;
            stats_.desktop_decoded_height[i]=snapshots[i]?snapshots[i]->height:0;
            if(!available&&ui_.panels()[i].available){surfaces_[i].have_frame=false;surfaces_[i].generation=surfaces_[i].submitted_generation=0;}
            ui_.set_available(i,available,[&](const UiEvent& e){dispatch(e);});
            // Hit geometry and the submitted image must use the same
            // full-video aspect in this frame, including newly shown panes.
            if(snapshots[i]&&snapshots[i]->width&&snapshots[i]->height){
                const auto& f=*snapshots[i];const auto c=sources_[i].crop;
                const auto x=std::min(static_cast<unsigned>(c.x*f.width),f.width-1),y=std::min(static_cast<unsigned>(c.y*f.height),f.height-1);
                const auto w=std::clamp(static_cast<unsigned>(c.width*f.width),1u,f.width-x),h=std::clamp(static_cast<unsigned>(c.height*f.height),1u,f.height-y);
                ui_.set_aspect(i,static_cast<float>(h)/w,0);
            }
        }};
        // Keep the actual source live while the requested runtime prepares.
        select_sources(show_vr);
        block_pc_input(!ui_.anchored()||(!show_vr&&(std::none_of(ui_.panels().begin(),ui_.panels().end(),
            [](const auto& panel){return panel.available;}))));
        input(state.predictedDisplayTime);
        if(timing_.enabled())after_input_ns=clock_ns(Clock::now());
        if(callbacks_.image_quality){
            const auto next=callbacks_.image_quality();
            if(valid_image_quality(next)&&next!=image_quality_){
                if(next.vr_timewarp!=image_quality_.vr_timewarp||next.vr_upscaling!=image_quality_.vr_upscaling||next.vr_sharpening!=image_quality_.vr_sharpening||next.vr_sharpening_enabled!=image_quality_.vr_sharpening_enabled){
                    invalidate_vr_reuse();
                    
                    for(auto& target:vr_unwarped_surfaces_)std::fill(target.image_serials.begin(),target.image_serials.end(),0);
                    for(auto& target:vr_enhanced_surfaces_)std::fill(target.image_serials.begin(),target.image_serials.end(),0);
                }
                image_quality_=next;
            }
        }
        predicted_display_ns=predicted_monotonic_ns(state.predictedDisplayTime);
        vr_previous_wait_return_ns_=clock_ns(before_work);
        vr_previous_predicted_display_ns_=predicted_display_ns;
        const auto pacing_period=VrPresentationPacer::pacing_period(state.predictedDisplayPeriod,stats_.current_refresh_hz);
        vr_previous_display_period_=pacing_period;
        if(state.shouldRender&&show_vr&&ui_.anchored()&&stage_locatable_&&
           !exit_requested_&&!stop.load(std::memory_order_relaxed))
            pacing_wait_ns+=wait_vr_presentation(clock_ns(before_work),predicted_display_ns,pacing_period,*vr,stop);
        // Actions/settings may switch mode, and decode may publish a newer
        // complete pair while input queries are running. Take one late,
        // nonblocking snapshot; never retain an early pair after null or
        // an epoch/mode change from the authoritative provider.
        if(callbacks_.vr_frame){
            ++stats_.vr_late_snapshot_checks;
            auto late=callbacks_.vr_frame();
            const bool valid=late&&stage_locatable_&&valid_vr_pair(*late);
            if(valid&&late!=vr)++stats_.vr_late_snapshot_replacements;
            if(vr&&!valid)++stats_.vr_late_snapshot_invalidations;
            if(late&&!valid)++stats_.vr_rejected_stereo_frames;
            vr=std::move(late);
            if(show_vr!=valid)apply_mode(valid);
            show_vr=valid;
        }else show_vr=false;
        video_select_ns=clock_ns(Clock::now());
        target_vr=callbacks_.settings&&callbacks_.settings().capabilities.vr_requested;
        if(callbacks_.desktop_capture_status)ui_.set_desktop_capture_status(callbacks_.desktop_capture_status());
        if(callbacks_.desktop_capture_request){unsigned mask=15;
            for(unsigned i=0;i<sources_.size();++i)if(sources_[i].source_id<4&&!ui_.panels()[i].visible)mask&=~(1u<<sources_[i].source_id);
            callbacks_.desktop_capture_request(mask);
        }
        if(callbacks_.desktop_demand){unsigned mask=0;
            if(state.shouldRender&&!show_vr)for(unsigned i=0;i<sources_.size();++i)
                if(ui_.panels()[i].visible&&sources_[i].source_id<4)mask|=1u<<sources_[i].source_id;
            callbacks_.desktop_demand(mask);
        }
        if(state.shouldRender)
        select_sources(show_vr);
        if(!show_vr){invalidate_vr_reuse();have_vr_submission_time_=false;}
        bool desktop_configured=false,desktop_visible=false;
        for(unsigned i=0;i<sources_.size();++i)if(!sources_[i].available||sources_[i].available->load(std::memory_order_acquire)){
            desktop_configured=true;desktop_visible|=ui_.panels()[i].visible;
        }
        const bool desktop_ready=!show_vr&&((desktop_configured&&!desktop_visible)||
            std::any_of(ui_.panels().begin(),ui_.panels().end(),[](const auto& panel){return panel.visible&&panel.available;}));
        const bool waiting_video=!ui_.anchored()||(!show_vr&&!desktop_ready);
        block_pc_input(waiting_video);
        stats_.last_should_render=state.shouldRender;stats_.last_ui_anchored=ui_.anchored();
        stats_.last_vr_requested=target_vr;stats_.last_vr_pair_valid=show_vr;stats_.last_stage_locatable=stage_locatable_;
        stats_.last_transition_pending=waiting_video||(target_vr!=show_vr);
        startup_hud_.update(stats_.last_transition_pending,target_vr,mini_hud_preferences_.transition_hud_enabled);
        stats_.last_enabled_desktop_mask=stats_.last_ready_desktop_mask=stats_.last_cached_desktop_mask=0;
        for(unsigned i=0;i<sources_.size();++i){
            if(!sources_[i].available||sources_[i].available->load(std::memory_order_acquire))stats_.last_enabled_desktop_mask|=1u<<i;
            if(snapshots[i])stats_.last_ready_desktop_mask|=1u<<i;
        }
        desktop_measuring_frame_=desktop_ready;
        stats_.desktop_visible_mask=0;
        std::uint64_t layout_hash=14695981039346656037ULL;
        const auto hash_word=[&](std::uint32_t word){for(unsigned byte=0;byte<4;++byte){layout_hash^=(word>>(byte*8))&255u;layout_hash*=1099511628211ULL;}};
        const auto hash_float=[&](float value){std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof(bits));hash_word(bits);};
        for(unsigned i=0;i<ui_.panels().size();++i){const auto& p=ui_.panels()[i];
            if(p.visible)stats_.desktop_visible_mask|=1u<<i;
            hash_word(p.panel_id);hash_word(p.source_id);hash_word(p.visible?1u:0u);
            for(float value:{p.pose.position.x,p.pose.position.y,p.pose.position.z,
                p.pose.orientation.x,p.pose.orientation.y,p.pose.orientation.z,p.pose.orientation.w,
                p.width,p.height,p.curvature_radians,p.crop.x,p.crop.y,p.crop.width,p.crop.height})hash_float(value);
        }
        stats_.desktop_layout_fingerprint=layout_hash;
        if(state.shouldRender&&!exit_requested_&&!stop.load()){
            // A projection layer and a native quad cannot share a depth
            // buffer. Once any visible monitor bends, render all video
            // panels together so overlap agrees with nearest-hit input.
            bool project_video=false;
            if(image_quality_.desktop_filter)for(unsigned i=0;i<sources_.size();++i){const auto& p=ui_.panels()[i];
                project_video|=p.visible&&p.available;}
            if(!stats_.cylinder_layer_enabled)for(unsigned i=0;i<sources_.size();++i){const auto& p=ui_.panels()[i];
                project_video|=p.visible&&p.available&&p.curvature_radians>.001f;}
            const auto acquire_desktop=[&](unsigned i){auto& s=surfaces_[i];
                // Projection reads leased NV12 directly into its owned cache.
                // Native layers can reference the last released current image
                // without acquiring it again at the headset refresh cadence.
                if(desktop_nv12_fused_&&s.nv12_storage&&(project_video||s.released_serial==s.serial))return;
                acquire(s,acquired,stop);
            };
            for(unsigned i=0;i<sources_.size();++i)if(ui_.anchored()&&ui_.panels()[i].visible&&ui_.panels()[i].available){
                if(snapshots[i])stage(surfaces_[i],*snapshots[i],&sources_[i],i,snapshots[i]);
                if(surfaces_[i].have_frame){
                    acquire_desktop(i);const auto before_layers=composed.size();
                    if(project_video)projected.push_back(i);
                    else panel_layers(i,composed);
                    if(ui_.panels()[i].curvature_radians>.001f)curved_layers+=composed.size()-before_layers;
                    presented.push_back(i);
                }
            }
            if(show_vr&&ui_.anchored()){
                reused_vr=reusable_vr_pair(vr);
                // New images retain their source views for runtime composition.
                // Retained images reconstruct once from original decoder pixels.
                vr_timewarp_reconstruct_=reused_vr&&image_quality_.vr_timewarp&&vr->metadata->surfxr_tracking_timestamp;
                if(!reused_vr){invalidate_vr_reuse();
                    vr_measuring_frame_=true;vr_draw_begin_ns=clock_ns(Clock::now());}
                if(!reused_vr||vr_timewarp_reconstruct_)stage_vr(*vr,acquired,stop,vr_views);
                else vr_projection_views(*vr,vr_views);
                has_vr=true;
            }
            if(startup_hud_.visible()){
                stage_transition(target_vr);
                acquire(transition_surface_,acquired,stop);
                const auto p=StartupHud::panel();
                auto card=quad(transition_surface_,p.pose,p.width,p.height);
                card.space=head_space_;
                card.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                transition_layer.emplace(card);
            }
            stage_ui();
            candidate_presented_=false;
            if(ui_.anchored()&&ui_.keyboard_panel().visible&&ui_.keyboard_panel().available){const auto& p=ui_.keyboard_panel();acquire(surfaces_[4],acquired,stop);composed.emplace_back(quad(surfaces_[4],p.pose,p.width,p.height));}
            if(candidate_visible_&&ui_.anchored()&&composed.size()+(projected.empty()?0:1)+(has_vr?1:0)+5<=max_layers_){
                const auto p=ui_candidate_panel(ui_.keyboard_panel(),candidate_image_->width,candidate_image_->height);
                acquire(surfaces_[11],acquired,stop);auto layer=quad(surfaces_[11],p.pose,p.width,p.height);candidate_presented_=true;++stats_.keyboard_candidate_frames;
                layer.subImage.imageRect.extent={static_cast<std::int32_t>(candidate_image_->width),static_cast<std::int32_t>(candidate_image_->height)};
                composed.emplace_back(layer);
            }
            for(unsigned i=0;i<sources_.size();++i){
                const auto controls=ui_.panel_controls(i);
                if(ui_.anchored()&&controls.visible){
                    auto layer=controls_layer(i,controls);acquire(surfaces_[7+i],acquired,stop);
                    composed.emplace_back(layer);++control_layers;
                }
            }
            // Quad composition order is painter order. Put nearer panel
            // centers last; rays/cursors remain above all desktop layers.
            const auto distance=[&](const Layer& layer){const auto& p=layer.center;const float x=p.x-head_pose_.position.x,y=p.y-head_pose_.position.y,z=p.z-head_pose_.position.z;return x*x+y*y+z*z;};
            std::stable_sort(composed.begin(),composed.end(),[&](const auto& a,const auto& b){return distance(a)>distance(b);});
            // VIEW-space coordinates must not be distance-sorted against
            // world-space panel centers. Keep the local message on top.
            unsigned pointers_left=static_cast<unsigned>(std::count_if(ui_hands_.begin(),ui_hands_.end(),[](const auto& h){return h.active;}));
            if(transition_layer&&composed.size()+(projected.empty()?0:1)+(has_vr?1:0)+pointers_left+1<=max_layers_){
                composed.push_back(*transition_layer);transition_presented=true;
            }
            for(unsigned i=0;i<2;++i)if(ui_.anchored()&&ui_hands_[i].active&&(!ui_.keyboard_panel().visible||ui_.keyboard_binding()!=i+1)&&(!has_vr||ui_.keyboard_panel().visible)){
                acquire(surfaces_[5+i],acquired,stop);
                // A cursor is always retained. Extra laser-body layers use
                // only the budget left after reserving other hand cursors.
                const bool laser=composed.size()+(projected.empty()?0:1)+(has_vr?1:0)+pointers_left+1<=max_layers_;
                if(!laser)++stats_.pointer_laser_budget_omissions;
                pointer_layers(i,composed,laser);--pointers_left;
            }
            if(performance_enabled_&&has_vr&&composed.size()+(projected.empty()?0:1)+1<max_layers_){
                // Unchanged HUD images remain the last released image;
                // never acquire/wait/upload this swapchain at VR cadence.
                if(stage_performance())acquire(performance_surface_,acquired,stop);
                auto hud=quad(performance_surface_,{{0,PerformanceHud::y_m,PerformanceHud::z_m},{}},PerformanceHud::width_m,PerformanceHud::height_m);
                hud.space=head_space_;
                hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                composed.emplace_back(hud);performance_presented=true;
            }else if(performance_enabled_&&has_vr)++stats_.performance_overlay_budget_omissions;
            mini_hud_layer(composed,acquired,stop,has_vr,unsigned(projected.empty()?0:1)+(has_vr?1:0));
            if(gaze_debug_enabled_&&has_vr){
                if(composed.size()+(projected.empty()?0:1)+1+2<=max_layers_)
                    gaze_debug_eyes=gaze_debug_layers(composed,acquired,stop);
                else ++stats_.gaze_debug_budget_omissions;
            }
            if(composed.size()+(projected.empty()?0:1)+(has_vr?1:0)>max_layers_)throw std::runtime_error("Runtime layer limit cannot accommodate desktop UI");
            const bool reconstruct=has_vr&&(!reused_vr||vr_timewarp_reconstruct_);
            if(timing_.enabled())after_stage_ns=clock_ns(Clock::now());
            // CPU staging above may overlap the prior GPU reconstruction.
            // Complete it before a new upload changes copy_pending_ or the
            // renderer resets its single command/descriptor/query slots.
            complete_vr_render();
            if(image_quality_.desktop_filter&&projection_renderer_)for(unsigned id:projected){
                auto& surface=surfaces_[id];
                const auto found=std::find_if(acquired.begin(),acquired.end(),[&](const auto& a){return a.surface==&surface;});
                if(found!=acquired.end()&&needs_upload(*found)&&
                   projection_renderer_->can_reuse_source(id,surface.width,surface.height,surface.serial)){
                    // QualitySS samples its owned mip cache for this serial.
                    // Leave the rotating XR image's serial honestly stale:
                    // filter-off or cache invalidation must upload it later.
                    found->suppress_upload=true;++stats_.desktop_cached_upload_skips;
                }
            }
            copy_batch(acquired,reconstruct||!projected.empty());
            convert_desktop_nv12(acquired,projected);
            if(desktop_measuring_frame_&&!projected.empty()&&copy_pending_)++stats_.desktop_deferred_copy_batches;
            if(reconstruct){
                // Locate after staging/prior GPU waits, immediately before
                // reconstruction. Never warp a previously warped swapchain.
                if(vr_timewarp_reconstruct_)late_timewarp(state.predictedDisplayTime,vr_views);
                const bool completed=reconstruct_vr(acquired,projected.empty());
                // An already-current target can make reconstruction a
                // no-op. In that case the upload still needs its own wait.
                if(!vr_render_pending_)complete_copy_batch(acquired,completed);
            }
            const auto before_projection=desktop_measuring_frame_&&!projected.empty()?Clock::now():Clock::time_point{};
            has_projection=project(projected,state.predictedDisplayTime,acquired,stop,projection_views);
            if(before_projection!=Clock::time_point{}){const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-before_projection).count();
                ++stats_.desktop_projection_calls;stats_.desktop_projection_ms_sum+=ms;
                stats_.desktop_projection_ms_max=std::max(stats_.desktop_projection_ms_max,ms);}
            // A completed projection on queue_ covers its preceding uploads,
            // including UI textures. If view location made it a no-op, wait
            // for the upload fence before releasing any swapchain images.
            if(desktop_measuring_frame_&&has_projection&&copy_pending_)++stats_.desktop_deferred_copy_covered_completions;
            if(!vr_render_pending_)complete_copy_batch(acquired,has_projection);
            complete_desktop_nv12(acquired,has_projection);
            if(has_projection)++curved_layers;
            else for(unsigned id:projected){
                presented.erase(std::remove(presented.begin(),presented.end(),id),presented.end());
            }
            for(auto& a:acquired){auto release=xr_info<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);xr_check(xrReleaseSwapchainImage(a.surface->swapchain,&release),"xrReleaseSwapchainImage");
                a.released=true;if(a.index<a.surface->image_serials.size())a.surface->released_serial=a.surface->image_serials[a.index];}
        }else complete_vr_render();
        if(timing_.enabled())after_render_ns=clock_ns(Clock::now());
        std::vector<const XrCompositionLayerBaseHeader*> layers;layers.reserve(composed.size()+1);
        if(has_vr)layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vr_layer));
        if(has_projection)layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
        for(const auto& layer:composed)layers.push_back(layer.header());
        end.layerCount=static_cast<std::uint32_t>(layers.size());end.layers=layers.empty()?nullptr:layers.data();
        stats_.last_layer_count=end.layerCount;
        const auto before_end_ns=clock_ns(Clock::now());
        end_attempted=true;const auto result=xrEndFrame(session_,&end);stats_.last_end_result=static_cast<int>(result);
        const auto after_end=Clock::now();
        if(result==XR_SUCCESS&&has_vr&&!reused_vr)surfxr_performance_.presented(*vr,clock_ns(after_end));
        
        publish_surfxr_performance();
        record_timing(before_end_ns,clock_ns(after_end),static_cast<int>(result));xr_check(result,"xrEndFrame");
        if(result==XR_SUCCESS&&has_vr&&video_select_ns)
            vr_pacer_.observe_cpu(clock_ns(after_end)-video_select_ns);
        if(result==XR_SUCCESS&&performance_presented)++stats_.performance_overlay_frames;
        if(result==XR_SUCCESS&&gaze_debug_eyes){++stats_.gaze_debug_frames;stats_.gaze_debug_eye_layers+=gaze_debug_eyes;}
        startup_hud_presented_=result==XR_SUCCESS&&transition_presented;
        if(startup_hud_presented_){
            ++stats_.transition_frames;
            ++stats_.transition_placeholder_frames;
        }
        if(result==XR_SUCCESS&&(has_vr||!presented.empty())){
            if(callbacks_.frame_presented)callbacks_.frame_presented(has_vr);
        }
        const bool desktop_presented=result==XR_SUCCESS&&!has_vr&&!presented.empty();
        if(desktop_presented){
            ++stats_.desktop_submitted_frames;
            const auto end_ms=double(clock_ns(after_end)-before_end_ns)/1'000'000.;
            stats_.desktop_end_frame_ms_sum+=end_ms;stats_.desktop_end_frame_ms_max=std::max(stats_.desktop_end_frame_ms_max,end_ms);
            const auto work_ms=std::chrono::duration<double,std::milli>(after_end-before_work).count();
            stats_.desktop_frame_work_ms_sum+=work_ms;stats_.desktop_frame_work_ms_max=std::max(stats_.desktop_frame_work_ms_max,work_ms);
            if(desktop_previous_submission_!=Clock::time_point{}){
                const auto ms=std::chrono::duration<double,std::milli>(after_end-desktop_previous_submission_).count();
                ++stats_.desktop_submit_intervals;stats_.desktop_submit_interval_ms_sum+=ms;
                stats_.desktop_submit_interval_ms_max=std::max(stats_.desktop_submit_interval_ms_max,ms);}
            desktop_previous_submission_=after_end;
            for(unsigned i=0;i<desktop_previous_new_submission_.size();++i){
                if(std::find(presented.begin(),presented.end(),i)==presented.end()){desktop_previous_new_submission_[i]={};continue;}
                const auto& s=surfaces_[i];
                if(s.submitted_generation==s.generation)continue;
                ++stats_.desktop_new_frames[i];
                if(desktop_previous_new_submission_[i]!=Clock::time_point{}){
                    const auto ms=std::chrono::duration<double,std::milli>(after_end-desktop_previous_new_submission_[i]).count();
                    ++stats_.desktop_new_frame_intervals[i];stats_.desktop_new_frame_interval_ms_sum[i]+=ms;
                    stats_.desktop_new_frame_interval_ms_max[i]=std::max(stats_.desktop_new_frame_interval_ms_max[i],ms);}
                desktop_previous_new_submission_[i]=after_end;
            }
        }else{desktop_previous_submission_={};desktop_previous_new_submission_.fill(Clock::time_point{});}
        if(!presented.empty()||has_vr)++stats_.submitted_frames;else ++stats_.zero_layer_frames;
        if(has_vr){++stats_.vr_submitted_frames;if(!have_vr_timestamp_||stats_.vr_last_timestamp!=vr->metadata->timestamp)++stats_.vr_new_stereo_frames;
            stats_.vr_last_timestamp=vr->metadata->timestamp;have_vr_timestamp_=true;
            if(result==XR_SUCCESS){
                const auto end_ms=double(clock_ns(after_end)-before_end_ns)/1'000'000.;
                stats_.vr_end_frame_ms_sum+=end_ms;stats_.vr_end_frame_ms_max=std::max(stats_.vr_end_frame_ms_max,end_ms);
                const auto work_ms=std::chrono::duration<double,std::milli>(after_end-before_work).count();
                stats_.vr_frame_work_ms_sum+=work_ms;stats_.vr_frame_work_ms_max=std::max(stats_.vr_frame_work_ms_max,work_ms);
                if(!reused_vr&&vr_draw_begin_ns){
                    const MotionFeedback feedback{vr->origin_epoch,vr->metadata->timestamp,vr->pose_query_monotonic_ns,
                        clock_ns(after_end),predicted_display_ns};
                    // SURFXR learns through this frame's estimated display.
                    observe_motion_feedback(feedback);
                }
                vr_released_pair_=vr;if(reused_vr)++stats_.vr_reused_stereo_frames;
                if(vr_timewarp_reconstruct_)++(vr_timewarp_[0].fov[0]>0?stats_.vr_timewarp_frames:stats_.vr_timewarp_fallback_frames);
                if(callbacks_.vr_frame_presented)callbacks_.vr_frame_presented(vr);
                const auto now=Clock::now();
                const auto submitted_ns=clock_ns(now);
                if(performance_enabled_||mini_hud_preferences_.mini_hud_enabled){
                    const auto oldest=vr_decode_time_range(*vr)[0];
                    // Original matched tracking query, not the later pose
                    // attachment time; all subtraction stays on Frame.
                    performance_.observe(clock_ns(after_end),vr->metadata->timestamp,vr->pose_query_monotonic_ns,
                                         oldest,predicted_monotonic_ns(state.predictedDisplayTime),vr->origin_epoch);
                }
                if(vr->pose_sampled_monotonic_ns&&submitted_ns>=vr->pose_sampled_monotonic_ns){
                    const auto age=double(submitted_ns-vr->pose_sampled_monotonic_ns)/1'000'000.;
                    ++stats_.vr_submitted_pose_age_samples;stats_.vr_submitted_pose_age_ms_sum+=age;
                    stats_.vr_submitted_pose_age_ms_max=std::max(stats_.vr_submitted_pose_age_ms_max,age);
                }
                if(!reused_vr){
                    const auto now_ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
                    if(vr->pose_sampled_monotonic_ns&&now_ns>=vr->pose_sampled_monotonic_ns){
                        const double age=double(now_ns-vr->pose_sampled_monotonic_ns)/1'000'000.;
                        ++stats_.vr_pose_sample_age_samples;stats_.vr_pose_sample_age_ms_sum+=age;
                        stats_.vr_pose_sample_age_ms_max=std::max(stats_.vr_pose_sample_age_ms_max,age);
                    }
                    const auto times=vr_decode_time_range(*vr);
                    const auto oldest=times[0],newest=times[1];
                    if(oldest&&now_ns>=newest){
                        const double age=double(now_ns-oldest)/1'000'000.;
                        ++stats_.vr_decoded_age_samples;stats_.vr_decoded_age_ms_sum+=age;
                        stats_.vr_decoded_age_ms_max=std::max(stats_.vr_decoded_age_ms_max,age);
                        stats_.vr_eye_decode_skew_ms_max=std::max(stats_.vr_eye_decode_skew_ms_max,double(newest-oldest)/1'000'000.);
                    }
                }
                if(have_vr_submission_time_){const auto interval=std::chrono::duration<double,std::milli>(now-vr_previous_submission_).count();
                    ++stats_.vr_submit_intervals;stats_.vr_submit_interval_ms_sum+=interval;
                    stats_.vr_submit_interval_ms_max=std::max(stats_.vr_submit_interval_ms_max,interval);}
                vr_previous_submission_=now;have_vr_submission_time_=true;
            }
            else invalidate_vr_reuse();}
        if((performance_enabled_||mini_hud_preferences_.mini_hud_enabled)&&(!has_vr||result!=XR_SUCCESS))performance_.reset();
        stats_.panel_layer_submissions+=presented.size();stats_.max_visible_panels=std::max(stats_.max_visible_panels,static_cast<std::uint32_t>(presented.size()));
        stats_.curved_layer_submissions+=curved_layers;stats_.controls_layer_submissions+=control_layers;
        for(const auto i:presented){auto& s=surfaces_[i];
            if(s.submitted_generation==s.generation)++stats_.repeated_submissions;
            else if(s.decoded_ns){const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
                if(now>=s.decoded_ns){const auto age=static_cast<double>(now-s.decoded_ns)/1'000'000.;++stats_.new_generation_age_samples;stats_.new_generation_age_ms_mean+=(age-stats_.new_generation_age_ms_mean)/static_cast<double>(stats_.new_generation_age_samples);stats_.new_generation_age_ms_max=std::max(stats_.new_generation_age_ms_max,age);}}
            s.submitted_generation=s.generation;
        }
        if(callbacks_.frame_observed)callbacks_.frame_observed();
    }catch(...){
        invalidate_vr_reuse();
        if(device_)vkDeviceWaitIdle(device_);
        copy_pending_=false; // only after pending GPU work has been waited
        vr_render_pending_=false;vr_pending_uploads_.clear();vr_pending_source_frames_={};
        for(auto& a:acquired)if(a.waited&&!a.released){auto release=xr_info<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);xrReleaseSwapchainImage(a.surface->swapchain,&release);}
        if(!end_attempted){end.layerCount=0;end.layers=nullptr;stats_.last_end_result=static_cast<int>(xrEndFrame(session_,&end));}
        if(!end_attempted)record_timing(0,clock_ns(Clock::now()),static_cast<int>(XR_ERROR_RUNTIME_FAILURE));
        throw;
    }
}

} // namespace surfxr
