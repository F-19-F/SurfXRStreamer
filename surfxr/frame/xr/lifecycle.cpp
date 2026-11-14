#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::notify_renderable(bool value){
    if(callbacks_.renderable&&(!renderable_notified_||last_renderable_notification_!=value)){
        if(!value){invalidate_vr_reuse();have_vr_submission_time_=false;}
        callbacks_.renderable(value);
        renderable_notified_=true;last_renderable_notification_=value;
    }
}

XrDisplay::Impl::~Impl(){
    current_tracking_.clear();
    boundary_.reset();
    stop_haptics();
    if(device_)vkDeviceWaitIdle(device_);
    // The media worker and external VR state have already released their
    // frames before display destruction. Release our retained pair while
    // the VkDevice needed by its mapped allocations is still alive too.
    vr_released_pair_.reset();vr_pending_uploads_.clear();vr_pending_source_frames_={};
    if(session_&&set_metrics_state_&&stats_.render_latency_enabled){
        auto state=xr_info<XrPerformanceMetricsStateMETA>(XR_TYPE_PERFORMANCE_METRICS_STATE_META);state.enabled=XR_FALSE;
        set_metrics_state_(session_,&state); // optional, session-local collection only
    }
    projection_renderer_.reset(); // cached framebuffers must die before their image views
    foveation_renderer_.reset();
    enhancement_renderer_.reset();
    clear_surface(projection_surface_);
    clear_surface(transition_surface_);
    clear_surface(performance_surface_);
    clear_surface(mini_hud_surface_);
    clear_surface(gaze_debug_surface_);
    for(auto& s:vr_unwarped_surfaces_)clear_surface(s);
    for(auto& s:vr_enhanced_surfaces_)clear_surface(s);
    for(auto& s:vr_surfaces_)clear_surface(s);
    for(auto& s:vr_focus_surfaces_)clear_surface(s);
    for(auto& s:surfaces_)clear_surface(s);
    if(fence_)vkDestroyFence(device_,fence_,nullptr);
    if(pool_)vkDestroyCommandPool(device_,pool_,nullptr);
    for(auto aim:aim_spaces_)if(aim)xrDestroySpace(aim);
    for(auto grip:grip_spaces_)if(grip)xrDestroySpace(grip);
    if(gaze_space_)xrDestroySpace(gaze_space_);
    if(stage_space_)xrDestroySpace(stage_space_);
    if(head_space_)xrDestroySpace(head_space_);
    if(space_)xrDestroySpace(space_);
    if(session_)xrDestroySession(session_);
    if(action_set_)xrDestroyActionSet(action_set_);
    if(device_)vkDestroyDevice(device_,nullptr);
    if(vk_instance_)vkDestroyInstance(vk_instance_,nullptr);
    if(instance_)xrDestroyInstance(instance_);
}

void XrDisplay::Impl::initialize(){create_xr();create_vulkan();create_session();create_actions();create_commands();}

XrDisplayStats XrDisplay::Impl::run(const std::vector<XrPanelSource>& panels,std::atomic_bool& stop,unsigned seconds,const XrUiCallbacks& callbacks,const UiLayout* initial_layout){
    if(used_)throw std::runtime_error("XrDisplay can only run once");
    if(seconds>3600)throw std::runtime_error("Display duration must be 0 (unlimited) or in 1..3600 seconds");
    if(initial_layout){if(!validate_ui_layout(*initial_layout))throw std::runtime_error("Invalid initial panel layout");initial_layout_=*initial_layout;}
    std::vector<UiPanel> model;for(const auto& source:panels){if(!source.frames)throw std::runtime_error("Null panel frame source");UiPanel p;p.source_id=source.source_id;p.title=source.title;p.crop=source.crop;model.push_back(p);}
    ui_.set_panels(std::move(model));sources_=panels;callbacks_=callbacks;stats_.adaptive_prediction_enabled=callbacks.adaptive_prediction;used_=true;started_=Clock::now();auto exit_deadline=Clock::time_point{};
    notify_renderable(false);
    try{
        while(!finished_){events();if(finished_)break;retire_decoder_frames();const auto now=Clock::now();
            if(stop.load()||ui_.exit_requested()||(seconds&&now-started_>=std::chrono::seconds(seconds))){
                stop_haptics();
                stop.store(true);if(running_&&!exit_requested_){xr_check(xrRequestExitSession(session_),"xrRequestExitSession");exit_requested_=true;exit_deadline=now+std::chrono::seconds(3);}else if(!running_)break;
            }
            if(exit_requested_&&now>=exit_deadline)throw std::runtime_error("Runtime did not stop within 3 seconds");
            if(!running_){if(now-started_>std::chrono::seconds(10))throw std::runtime_error("Runtime did not become ready within 10 seconds");std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
            frame(stop);
        }
        current_tracking_.clear();notify_renderable(false);complete_vr_render();
    }catch(...){current_tracking_.clear();system_input_.close();notify_renderable(false);stop_haptics();stop.store(true);try{ui_.release_inputs([&](const UiEvent& e){dispatch(e);});}catch(...){}throw;}
    system_input_.close();stop_haptics();ui_.release_inputs([&](const UiEvent& e){dispatch(e);});stop.store(true);stats_.elapsed_seconds=std::chrono::duration<double>(Clock::now()-started_).count();return stats_;
}

} // namespace surfxr
