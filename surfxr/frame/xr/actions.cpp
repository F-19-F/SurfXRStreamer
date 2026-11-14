#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::haptic_request(unsigned hand,float amplitude,std::uint64_t duration_ns,float frequency) noexcept {
    const bool play=duration_ns>0&&amplitude>0;
    if(!session_||!haptic_action_||!running_){++stats_.haptic_suppressed;haptic_active_[hand]=false;return;}
    auto info=xr_info<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
    info.action=haptic_action_;info.subactionPath=hand_paths_[hand];
    XrResult result;
    if(play){
        ++stats_.haptic_apply_requests[hand];
        auto effect=xr_info<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION);
        effect.amplitude=amplitude;effect.duration=static_cast<XrDuration>(duration_ns);
        effect.frequency=frequency>0?frequency:XR_FREQUENCY_UNSPECIFIED;
        result=xrApplyHapticFeedback(session_,&info,reinterpret_cast<const XrHapticBaseHeader*>(&effect));
    }else{++stats_.haptic_stop_requests[hand];result=xrStopHapticFeedback(session_,&info);}
    stats_.last_haptic_result=static_cast<int>(result);
    // Optional controller output must never tear down an otherwise valid
    // video session. SUCCESS confirms API acceptance, not physical motion.
    if(result==XR_SUCCESS){
        haptic_active_[hand]=play;
        if(play)++stats_.haptic_apply_accepted[hand];else ++stats_.haptic_stop_accepted[hand];
    }else{
        haptic_active_[hand]=false;
        if(result==XR_SESSION_NOT_FOCUSED)++stats_.haptic_suppressed;else ++stats_.haptic_errors;
    }
}

void XrDisplay::Impl::stop_haptics() noexcept {
    if(callbacks_.haptics)try{
        callbacks_.haptics->clear(false); // focus is not a new stream generation
        callbacks_.haptics->consume(clock_ns(Clock::now()),false);
    }catch(...){++stats_.haptic_errors;}
    for(unsigned hand=0;hand<2;++hand)if(haptic_active_[hand])haptic_request(hand,0,0);
}

void XrDisplay::Impl::update_haptics(bool focused) {
    if(!callbacks_.haptics)return;
    const auto commands=callbacks_.haptics->consume(clock_ns(Clock::now()),focused&&running_&&!exit_requested_);
    for(unsigned hand=0;hand<2;++hand)if(commands[hand].changed)
        haptic_request(hand,commands[hand].amplitude,commands[hand].duration_ns,commands[hand].frequency);
}

void XrDisplay::Impl::events() {
    for (;;) {
        auto event = xr_info<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        const auto result = xrPollEvent(instance_, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        xr_check(result, "xrPollEvent");
        if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING||
           event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED||
           event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)current_tracking_.clear();
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            stop_haptics();
            invalidate_vr_reuse();
            finished_ = true;
            throw std::runtime_error("OpenXR instance loss pending");
        }
        if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING){
            const auto& changed=*reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            if(changed.session==session_&&changed.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_STAGE){
                if(stage_changes_.size()>=64)throw std::runtime_error("Too many pending Stage changes");
                stage_changes_.push_back(changed.changeTime);std::sort(stage_changes_.begin(),stage_changes_.end());
            }
            continue;
        }
        if(event.type==XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB){
            const auto& changed=*reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
            observe_refresh_rate(changed.toDisplayRefreshRate);
            continue;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        if (changed.session != session_) continue;
        // Clear both on loss and regain: commands received while another
        // application owned focus must not replay when this session returns.
        if(changed.state!=XR_SESSION_STATE_FOCUSED||state_!=XR_SESSION_STATE_FOCUSED)stop_haptics();
        if(changed.state!=XR_SESSION_STATE_FOCUSED){system_input_.suspend();shortcuts_.reset();}
        state_ = changed.state;
        stats_.last_session_state = static_cast<int>(state_);
        if (state_ >= 0 && static_cast<std::size_t>(state_) < stats_.session_state_events.size())
            ++stats_.session_state_events[static_cast<std::size_t>(state_)];
        if (state_ == XR_SESSION_STATE_READY && !running_ && !exit_requested_) {
            invalidate_vr_reuse();
            auto begin = xr_info<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            xr_check(xrBeginSession(session_, &begin), "xrBeginSession");
            running_ = true;
            start_performance_metrics();
        } else if (state_ == XR_SESSION_STATE_STOPPING) {
            invalidate_vr_reuse();
            if (running_) xr_check(xrEndSession(session_), "xrEndSession");
            running_ = false;
            finished_ = true;
        } else if (state_ == XR_SESSION_STATE_LOSS_PENDING) {
            invalidate_vr_reuse();
            finished_ = true;
            throw std::runtime_error("OpenXR session loss pending");
        } else if (state_ == XR_SESSION_STATE_EXITING) {
            invalidate_vr_reuse();
            finished_ = true;
        }
    }
}

XrPath XrDisplay::Impl::path(const char* value) {
    XrPath result=XR_NULL_PATH;xr_check(xrStringToPath(instance_,value,&result),"xrStringToPath");return result;
}

XrAction XrDisplay::Impl::action(const char* name,const char* label,XrActionType type) {
    auto info=xr_info<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
    std::strncpy(info.actionName,name,XR_MAX_ACTION_NAME_SIZE-1);
    std::strncpy(info.localizedActionName,label,XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
    info.actionType=type;info.countSubactionPaths=2;info.subactionPaths=hand_paths_.data();
    XrAction result=XR_NULL_HANDLE;xr_check(xrCreateAction(action_set_,&info,&result),"xrCreateAction");return result;
}

void XrDisplay::Impl::suggest(const char* profile,bool simple,bool frame,bool index) {
    std::vector<XrActionSuggestedBinding> bindings;
    const auto bind=[&](XrAction a,unsigned hand,const char* component){
        const std::string full=std::string(hand?"/user/hand/right":"/user/hand/left")+component;
        bindings.push_back({a,path(full.c_str())});
    };
    for(unsigned i=0;i<2;++i){
        bind(aim_action_,i,"/input/aim/pose");
        bind(select_action_,i,simple?"/input/select/click":"/input/trigger/value");
        if(!simple){
            bind(grip_action_,i,"/input/squeeze/value");bind(stick_action_,i,"/input/thumbstick");
            bind(right_click_action_,i,"/input/thumbstick/click");
            // Frame has its ABXY cluster on the right controller; its
            // installed profile does not advertise a left Y button.
            if(!frame||i)bind(right_click_action_,i,index?"/input/b/click":(i?"/input/b/click":"/input/y/click"));
        }
        if(frame){if(i)bind(menu_action_,i,"/input/menu/click");}
        else if(index)bind(menu_action_,i,"/input/a/click");
        else if(simple)bind(menu_action_,i,"/input/menu/click");
        else bind(menu_action_,i,i?"/input/a/click":"/input/menu/click");
    }
    auto info=xr_info<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    info.interactionProfile=path(profile);info.countSuggestedBindings=static_cast<std::uint32_t>(bindings.size());info.suggestedBindings=bindings.data();
    const auto result=xrSuggestInteractionProfileBindings(instance_,&info);
    // Profile paths may differ on older runtimes. Keep the Touch/simple
    // fallbacks available instead of making one unsupported profile fatal.
    if(result==XR_ERROR_PATH_UNSUPPORTED){++stats_.binding_profiles_rejected;return;}
    xr_check(result,"xrSuggestInteractionProfileBindings");
    // Commit each optional component independently. One unsupported touch
    // path must not remove already accepted grip poses or button clicks.
    bool profile_rejected=false;
    const auto optional_bind=[&](XrAction action,unsigned hand,const char* component){
        bind(action,hand,component);
        info.countSuggestedBindings=static_cast<std::uint32_t>(bindings.size());info.suggestedBindings=bindings.data();
        const auto augmented=xrSuggestInteractionProfileBindings(instance_,&info);
        if(augmented==XR_ERROR_PATH_UNSUPPORTED){
            if(!profile_rejected){++stats_.tracking_binding_profiles_rejected;profile_rejected=true;}
            bindings.pop_back();info.countSuggestedBindings=static_cast<std::uint32_t>(bindings.size());info.suggestedBindings=bindings.data();
            xr_check(xrSuggestInteractionProfileBindings(instance_,&info),"xrSuggestInteractionProfileBindings(restore accepted)");
            return false;
        }else xr_check(augmented,"xrSuggestInteractionProfileBindings(tracking)");
        return true;
    };
    for(unsigned i=0;i<2;++i)optional_bind(grip_pose_action_,i,"/input/grip/pose");
    for(unsigned i=0;i<2;++i){const bool accepted=optional_bind(haptic_action_,i,"/output/haptic");
        if(frame)stats_.frame_haptic_binding_supported[i]=accepted;}
    for(unsigned i=0;i<2;++i){
        for(const auto& raw:raw_buttons_){
            bool click=false,touch=false;
            if(frame){
                click=raw.bit==ButtonThumbstick||raw.bit==ButtonTrigger||raw.bit==ButtonSqueeze||raw.bit==ButtonBumper||raw.bit==ButtonSystem||
                    (i?(raw.bit==ButtonA||raw.bit==ButtonB||raw.bit==ButtonX||raw.bit==ButtonY||raw.bit==ButtonMenu):
                        (raw.bit==ButtonSystem||raw.bit==ButtonView||raw.bit==ButtonDpadUp||raw.bit==ButtonDpadDown||raw.bit==ButtonDpadLeft||raw.bit==ButtonDpadRight));
                touch=click;
            }else if(simple){click=raw.bit==ButtonTrigger||raw.bit==ButtonMenu;}
            else if(index){click=raw.bit==ButtonA||raw.bit==ButtonB||raw.bit==ButtonThumbstick||raw.bit==ButtonTrigger;
                touch=click;}
            else{click=raw.bit==ButtonThumbstick||(i?(raw.bit==ButtonA||raw.bit==ButtonB):(raw.bit==ButtonX||raw.bit==ButtonY||raw.bit==ButtonMenu));
                touch=(click&&raw.bit!=ButtonMenu)||raw.bit==ButtonTrigger||raw.bit==ButtonThumbrest;}
            if(click){const std::string component=simple&&raw.bit==ButtonTrigger?"/input/select/click":std::string("/input/")+raw.name+"/click";
                const bool accepted=optional_bind(raw.click,i,component.c_str());
                if(frame&&i==0&&raw.bit==ButtonSystem)stats_.left_system_binding_supported=accepted;}
            if(touch){const std::string component=std::string("/input/")+raw.name+"/touch";optional_bind(raw.touch,i,component.c_str());}
        }
    }
}

void XrDisplay::Impl::create_actions() {
    hand_paths_={path("/user/hand/left"),path("/user/hand/right")};
    auto info=xr_info<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    std::strcpy(info.actionSetName,"desktop_ui");std::strcpy(info.localizedActionSetName,"Desktop panels and keyboard");
    xr_check(xrCreateActionSet(instance_,&info,&action_set_),"xrCreateActionSet");
    aim_action_=action("aim","Pointer aim",XR_ACTION_TYPE_POSE_INPUT);
    select_action_=action("select","Click or use external panel controls",XR_ACTION_TYPE_FLOAT_INPUT);
    grip_action_=action("grab","Move panel",XR_ACTION_TYPE_FLOAT_INPUT);
    stick_action_=action("stick","Scale depth and scroll",XR_ACTION_TYPE_VECTOR2F_INPUT);
    menu_action_=action("menu","Keyboard or switch streaming mode",XR_ACTION_TYPE_BOOLEAN_INPUT);
    right_click_action_=action("right_click","Right mouse: right B or stick click",XR_ACTION_TYPE_BOOLEAN_INPUT);
    grip_pose_action_=action("grip_pose","Raw controller grip pose",XR_ACTION_TYPE_POSE_INPUT);
    haptic_action_=action("haptics","Remote controller vibration",XR_ACTION_TYPE_VIBRATION_OUTPUT);
    for(auto& raw:raw_buttons_){const auto name=std::string("raw_")+raw.name;
        raw.click=action((name+"_click").c_str(),(name+" click").c_str(),XR_ACTION_TYPE_BOOLEAN_INPUT);
        raw.touch=action((name+"_touch").c_str(),(name+" touch").c_str(),XR_ACTION_TYPE_BOOLEAN_INPUT);
    }
    if(stats_.frame_controller_profile_enabled)suggest("/interaction_profiles/valve/frame_controller_valve",false,true,false);
    suggest("/interaction_profiles/oculus/touch_controller",false,false,false);
    suggest("/interaction_profiles/valve/index_controller",false,false,true);
    suggest("/interaction_profiles/khr/simple_controller",true,false,false);
    for(unsigned i=0;i<2;++i){auto create=xr_info<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        create.action=aim_action_;create.subactionPath=hand_paths_[i];create.poseInActionSpace.orientation.w=1;
        xr_check(xrCreateActionSpace(session_,&create,&aim_spaces_[i]),"xrCreateActionSpace");
        create.action=grip_pose_action_;xr_check(xrCreateActionSpace(session_,&create,&grip_spaces_[i]),"xrCreateActionSpace(grip)");}
    create_gaze_action();
    auto attach=xr_info<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    attach.countActionSets=1;attach.actionSets=&action_set_;
    xr_check(xrAttachSessionActionSets(session_,&attach),"xrAttachSessionActionSets");
}

UiPose XrDisplay::Impl::ui_pose(const XrPosef& p){return {{p.position.x,p.position.y,p.position.z},{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}};}

XrPosef XrDisplay::Impl::xr_pose(UiPose p){return {{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},{p.position.x,p.position.y,p.position.z}};}

void XrDisplay::Impl::dispatch(const UiEvent& e) {
    if(e.type==UiEventType::SystemCaptureChanged){system_input_.request(e.pressed&&ui_.system_capture_available());return;}
    // With no live video, the keyboard still has its own hit
    // geometry, so panel availability alone cannot prevent typing blindly.
    // Releases must always reach the PC, and local recovery actions remain
    // usable while the receiver has no presentable video.
    if(pc_input_blocked_&&(e.type==UiEventType::PointerMove||e.type==UiEventType::Scroll||e.type==UiEventType::Text||
        ((e.type==UiEventType::PointerButton||e.type==UiEventType::Key)&&e.pressed)))return;
    if(e.type==UiEventType::Text||e.type==UiEventType::Key)++stats_.keyboard_events;
    else if(e.type!=UiEventType::Exit)++stats_.pointer_events;
    if(e.type==UiEventType::PointerButton&&e.button==1&&e.pressed)++stats_.right_click_events;
    if(callbacks_.event)callbacks_.event(e); // caller contract: nonblocking queue
}

void XrDisplay::Impl::block_pc_input(bool blocked) {
    if(pc_input_blocked_==blocked)return;
    pc_input_blocked_=blocked;
    // Clear keys held before the transition and local modifier latches
    // clicked during it, before accepting input again after recovery.
    ui_.release_inputs([&](const UiEvent& e){dispatch(e);});
}

void XrDisplay::Impl::allocate_curve_segments() {
    // Mesh vertices follow the exact arc at 64 intervals. The maximum
    // arc/chord separation is sub-millimetre even for an 8m/1.8rad screen;
    // use exact circular hit math instead of layer-budget-dependent chords.
    for(unsigned i=0;i<ui_.panels().size();++i)ui_.set_curve_segments(i,0);
}

} // namespace surfxr
