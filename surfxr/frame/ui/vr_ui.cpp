#include "ui_helpers.hpp"
using namespace surfxr::ui_detail;
#include "surfxr/vr_ui.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace surfxr {
namespace {
UiVec3 add(UiVec3 a,UiVec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
UiVec3 scale(UiVec3 a,float s){return {a.x*s,a.y*s,a.z*s};}
UiQuat inverse(UiQuat q){return {-q.x,-q.y,-q.z,q.w};}
void emit(const UiEventCallback& fn,const UiEvent& e){if(fn)fn(e);}

}
UiQuat ui_multiply(UiQuat a,UiQuat b){return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
UiVec3 ui_rotate(UiQuat q,UiVec3 v){auto t=ui_multiply(ui_multiply(q,{v.x,v.y,v.z,0}),inverse(q));return {t.x,t.y,t.z};}
UiPose ui_compose(UiPose a,UiPose b){return {add(a.position,ui_rotate(a.orientation,b.position)),ui_multiply(a.orientation,b.orientation)};}
UiPose ui_inverse(UiPose p){const auto q=inverse(p.orientation);return {ui_rotate(q,scale(p.position,-1)),q};}
bool validate_ui_layout(const UiLayout& layout) noexcept {
    const auto valid=[](UiPose pose,float width){
        const auto p=pose.position;
        return valid_pose(pose)&&std::isfinite(width)&&width>=kUiMinPanelWidthM&&width<=kUiMaxPanelWidthM&&
            p.x*p.x+p.y*p.y+p.z*p.z<=100.f;
    };
    if((layout.version<1||layout.version>3)||layout.panels.empty()||layout.panels.size()>4||
       !valid(layout.keyboard_pose,layout.keyboard_width_m)||layout.keyboard_binding>2||!valid(layout.keyboard_offset,layout.keyboard_space_width_m))return false;
    for(std::size_t i=0;i<layout.panels.size();++i){
        if(!valid(layout.panels[i].anchor_pose,layout.panels[i].width_m)||!std::isfinite(layout.panels[i].curvature_radians)||layout.panels[i].curvature_radians<0||layout.panels[i].curvature_radians>1.8f)return false;
        for(std::size_t j=0;j<i;++j)if(layout.panels[i].source_id==layout.panels[j].source_id)return false;
    }
    return true;
}
UiVec3 ui_panel_point(const UiPanel& p,float u,float v){
    UiVec3 point{(u-.5f)*p.width,(.5f-v)*p.height,0};
    if(p.curvature_radians>.001f){const float r=p.width/p.curvature_radians,theta=(u-.5f)*p.curvature_radians;point.x=r*std::sin(theta);point.z=r*(1-std::cos(theta));}
    return ui_compose(p.pose,{point,{}}).position;
}
UiHit ui_hit_panel(const UiPanel& p,const UiPose& aim){
    UiHit hit;
    if(!p.visible||!p.available||!valid_pose(p.pose)||!valid_pose(aim)||!std::isfinite(p.width)||!std::isfinite(p.height)||p.width<=0||p.height<=0||!std::isfinite(p.curvature_radians)||p.curvature_radians<0||p.curvature_radians>1.8f)return hit;
    if(p.curvature_radians>.001f&&p.curve_segments>=2&&p.curve_segments<=64){
        const float step=p.curvature_radians/p.curve_segments,r=p.width/p.curvature_radians;
        for(unsigned i=0;i<p.curve_segments;++i){const float theta=-p.curvature_radians/2+(i+.5f)*step;
            UiPanel segment=p;segment.curvature_radians=0;
            const float chord_center=r*std::cos(step/2);
            segment.pose=ui_compose(p.pose,{{chord_center*std::sin(theta),0,r-chord_center*std::cos(theta)},{0,std::sin(-theta/2),0,std::cos(theta/2)}});
            segment.width=2*r*std::sin(step/2);
            auto candidate=ui_hit_panel(segment,aim);
            if(candidate.valid&&(!hit.valid||candidate.distance<hit.distance)){hit=candidate;hit.u=(i+candidate.u)/p.curve_segments;}
        }
        return hit;
    }
    const auto local=ui_compose(ui_inverse(p.pose),aim);const auto direction=ui_rotate(local.orientation,{0,0,-1});
    const auto accept=[&](float t,float u,float v){if(t<.05f||t>10||u<0||u>1||v<0||v>1)return;
        hit.valid=true;hit.panel_id=p.panel_id;hit.u=u;hit.v=v;hit.distance=t;hit.point=ui_compose(p.pose,{add(local.position,scale(direction,t)),{}}).position;};
    if(p.curvature_radians<=.001f){
        if(direction.z>=-1e-5f||local.position.z<=0)return hit;
        const float t=-local.position.z/direction.z;const auto point=add(local.position,scale(direction,t));accept(t,point.x/p.width+.5f,.5f-point.y/p.height);return hit;
    }
    const float r=p.width/p.curvature_radians,x=local.position.x,z=local.position.z-r;
    const float a=direction.x*direction.x+direction.z*direction.z,b=2*(x*direction.x+z*direction.z),c=x*x+z*z-r*r;
    const float disc=b*b-4*a*c;if(a<1e-8f||disc<0)return hit;
    for(const float sign:{-1.f,1.f}){const float t=(-b+sign*std::sqrt(disc))/(2*a);const auto point=add(local.position,scale(direction,t));
        if(-point.x*direction.x+(r-point.z)*direction.z>=0)continue;
        accept(t,std::atan2(point.x,r-point.z)/p.curvature_radians+.5f,.5f-point.y/p.height);if(hit.valid)break;}
    return hit;
}
void VrUiModel::set_panels(std::vector<UiPanel> panels){
    cancel_reset();
    if(panels.empty()||panels.size()>4)throw std::invalid_argument("panel_count_must_be_1_to_4");
    for(std::size_t i=0;i<panels.size();++i){auto& p=panels[i];p.panel_id=static_cast<std::uint32_t>(i);
        const auto c=p.crop;
        if(!std::isfinite(c.x)||!std::isfinite(c.y)||!std::isfinite(c.width)||!std::isfinite(c.height)||c.x<0||c.y<0||c.width<=0||c.height<=0||c.x+c.width>1.00001f||c.y+c.height>1.00001f)throw std::invalid_argument("invalid_panel_crop");
        if(p.title.size()>48)p.title.resize(48);
    }
    panels_=std::move(panels);desktop_pointers_={};focus_source_=panels_[0].source_id;focus_panel_=0;
    keyboard_.panel_id=4;keyboard_.title="Keyboard";keyboard_.width=2.05f;keyboard_.height=.78f;keyboard_.visible=false;
    build_keys();layout();
}
void VrUiModel::reset_anchor(UiPose head){
    if(!valid_pose(head))throw std::invalid_argument("invalid_head_anchor");
    const auto forward=ui_rotate(head.orientation,{0,0,-1});
    const auto yaw=std::atan2(-forward.x,-forward.z);
    anchor_={head.position,{0,std::sin(yaw/2),0,std::cos(yaw/2)}};anchored_=true;layout();
}
UiLayout VrUiModel::export_layout() const {
    if(!anchored_)throw std::logic_error("layout_anchor_unavailable");
    UiLayout result;const auto inv=ui_inverse(anchor_);
    for(const auto& p:panels_)result.panels.push_back({p.source_id,ui_compose(inv,p.pose),p.width,p.visible,p.curvature_radians});
    result.keyboard_pose=ui_compose(inv,keyboard_.pose);result.keyboard_width_m=keyboard_.width;result.keyboard_visible=settings_visible_?keyboard_before_settings_:keyboard_.visible;
    result.keyboard_space_width_m=keyboard_binding_?keyboard_space_width_:keyboard_.width;result.keyboard_binding=keyboard_binding_;result.keyboard_offset=keyboard_offset_;
    if(keyboard_binding_)result.keyboard_pose=ui_compose(inv,keyboard_space_pose_);
    return result;
}
bool VrUiModel::restore_layout(const UiLayout& saved) noexcept {
    if(!anchored_||button_hand_>=0||!validate_ui_layout(saved))return false;
    for(const auto& h:hands_)if(h.active||h.drag>=0)return false;
    const auto normalized=[](UiPose pose){auto& q=pose.orientation;const auto n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);q.x/=n;q.y/=n;q.z/=n;q.w/=n;return pose;};
    for(auto& p:panels_)for(const auto& saved_panel:saved.panels)if(p.source_id==saved_panel.source_id){
        const float ratio=p.height/p.width;p.width=saved_panel.width_m;p.height=ratio*p.width;
        p.pose=ui_compose(anchor_,normalized(saved_panel.anchor_pose));p.visible=saved_panel.visible;p.curvature_radians=saved.version==1?0:saved_panel.curvature_radians;break;
    }
    const float ratio=keyboard_.height/keyboard_.width;keyboard_.width=saved.keyboard_width_m;keyboard_.height=ratio*keyboard_.width;
    keyboard_.pose=ui_compose(anchor_,normalized(saved.keyboard_pose));keyboard_.visible=saved.keyboard_visible;
    keyboard_space_pose_=keyboard_.pose;keyboard_space_width_=saved.version>=3?saved.keyboard_space_width_m:saved.keyboard_width_m;keyboard_binding_=saved.version>=3?saved.keyboard_binding:0;keyboard_offset_=normalized(saved.keyboard_offset);
    ++revision_;return true;
}
void VrUiModel::layout(){
    desktop_pointers_={};
    for(std::size_t i=0;i<panels_.size();++i){
        auto& p=panels_[i];const float x=i==0?0:(i==1?-1.95f:(i==2?1.95f:0));
        const float y=i==3?1.25f:0,angle=i==1?.55f:(i==2?-.55f:0);
        const auto ratio=p.height/p.width;p.width=1.8f;p.height=ratio*p.width;p.visible=true;p.curvature_radians=0;
        p.pose=ui_compose(anchor_,{{x,y,i==0?-2.0f:-2.45f},{0,std::sin(angle/2),0,std::cos(angle/2)}});
    }
    keyboard_.pose=ui_compose(anchor_,{{0,-.78f,-1.65f},{}});keyboard_.width=2.05f;keyboard_.height=.78f;
    keyboard_space_pose_=keyboard_.pose;
    if(keyboard_binding_){keyboard_.width=.85f;keyboard_.height=.85f*.78f/2.05f;keyboard_offset_={{0,.16f,-.28f},{}};}
    for(auto& hand:hands_)hand.drag=-1;
    ++revision_;
}
void VrUiModel::set_aspect(std::uint32_t id,float ratio,float){
    if(id>=panels_.size()||!std::isfinite(ratio)||ratio<=0||ratio>10)return;
    if(panels_[id].height!=panels_[id].width*ratio)desktop_pointers_={};
    panels_[id].height=panels_[id].width*ratio;panels_[id].header_fraction=0;
}
void VrUiModel::set_curve_segments(std::uint32_t id,unsigned segments){
    if(id<panels_.size()&&(segments==0||(segments>=2&&segments<=64))&&panels_[id].curve_segments!=segments){
        panels_[id].curve_segments=segments;desktop_pointers_={};
    }
}
UiPanelControls VrUiModel::panel_controls(std::uint32_t id) const {
    UiPanelControls result;if(id>=panels_.size())return result;const auto& p=panels_[id];
    result.visible=p.visible&&p.available&&(controls_remaining_[id]>0||pending_reset_==static_cast<int>(id));
    result.top=p;result.bottom=p;
    for(auto* strip:{&result.top,&result.bottom}){strip->curvature_radians=0;strip->curve_segments=0;strip->width=std::clamp(p.width,1.05f,1.45f);strip->visible=result.visible;}
    result.top.height=.16f;result.bottom.height=.32f;
    result.top.pose=ui_compose(p.pose,{{0,p.height/2+.10f,.012f},{}});
    result.bottom.pose=ui_compose(p.pose,{{0,-p.height/2-.20f,.012f},{}});
    return result;
}
void VrUiModel::request_reset(int panel,const UiEventCallback& events){
    if(panel<-1||(panel>=0&&static_cast<unsigned>(panel)>=panels_.size()))return;
    release_inputs(events);reset_return_settings_=settings_visible_;reset_return_keyboard_=keyboard_.visible;
    pending_reset_=panel;
    if(panel>=0)controls_remaining_[static_cast<unsigned>(panel)]=.9f;
    else {if(!settings_visible_)keyboard_before_settings_=keyboard_.visible;settings_visible_=true;keyboard_.visible=true;}
    ++revision_;
}
void VrUiModel::cancel_reset(){
    if(pending_reset_==-2)return;
    settings_visible_=reset_return_settings_;keyboard_.visible=reset_return_keyboard_;pending_reset_=-2;++revision_;
}
void VrUiModel::reset_panel(std::uint32_t id){
    if(id>=panels_.size())return;
    auto& p=panels_[id];const float ratio=p.height/p.width;
    const float x=id==0?0:(id==1?-1.95f:(id==2?1.95f:0)),y=id==3?1.25f:0,angle=id==1?.55f:(id==2?-.55f:0);
    p.pose=ui_compose(anchor_,{{x,y,id==0?-2.f:-2.45f},{0,std::sin(angle/2),0,std::cos(angle/2)}});
    p.width=1.8f;p.height=p.width*ratio;p.curvature_radians=0;++revision_;
}
void VrUiModel::set_available(std::uint32_t id,bool available,const UiEventCallback& events){
    if(id>=panels_.size()||panels_[id].available==available)return;
    if(!available&&pending_reset_!=-2)cancel_reset();
    panels_[id].available=available;++revision_;
    desktop_pointers_={};
    if(!available){
        controls_remaining_[id]=0;
        if(button_hand_>=0&&button_event_.panel_id==id)release_inputs(events);
        for(auto& hand:hands_)if(hand.drag==static_cast<int>(id))hand.drag=-1;
    }
}
void VrUiModel::set_visible(std::uint32_t id,bool visible,const UiEventCallback& events){
    if(!capture_status_.supported)return;
    auto& p=panels_.at(id);p.visible=visible;++revision_;
    desktop_pointers_={};
    if(!visible){
        controls_remaining_[id]=0;
        if(button_hand_>=0&&button_event_.panel_id==id)release_inputs(events);
        for(auto& hand:hands_)if(hand.drag==static_cast<int>(id))hand.drag=-1;
    }
}
void VrUiModel::release_inputs(const UiEventCallback& events){
    cancel_reset();
    if(shortcut_recording()){shortcut_slot_=-1;shortcut_draft_={};build_keys();++revision_;}
    if(button_hand_>=0&&mouse_down_){button_event_.pressed=false;emit(events,button_event_);}
    button_hand_=-1;mouse_down_=false;press_seconds_=drag_seconds_=0;desktop_pointers_={};
    UiEvent release;release.type=UiEventType::ReleaseAll;emit(events,release);
    if(!held_modifiers_.empty()||shift_)++revision_;
    held_modifiers_.clear();shift_=false;for(auto& h:hands_)h={};
}
const std::array<UiSettingRect,9>& VrUiModel::settings_rows(){
    static constexpr std::array<UiSettingRect,9> rows{{
        {SettingId::DesktopFramerate,.025f,.20f,.445f,.12f},
        {SettingId::DesktopBitrateMbps,.025f,.35f,.445f,.12f},
        {SettingId::MicrophoneEnabled,.025f,.50f,.445f,.12f},
        {SettingId::MicrophoneVolume,.025f,.65f,.445f,.12f},
        {SettingId::VrFramerate,.515f,.20f,.445f,.12f},
        {SettingId::VrGraphicsQuality,.515f,.35f,.445f,.12f},
        {SettingId::VrBitrateMbps,.515f,.50f,.445f,.12f},
        {SettingId::FoveatedStreaming,.515f,.65f,.445f,.12f},
        {SettingId::DesktopPointerPriority,.515f,.50f,.445f,.12f},
    }};return rows;
}
void VrUiModel::set_remote_settings(const RemoteSettingsSnapshot& settings){
    if(remote_seen_&&settings.revision==remote_settings_.revision)return;
    remote_settings_=settings;remote_seen_=true;++revision_;
}
const std::array<UiImageQualityRect,3>& VrUiModel::image_quality_rows(){
    static constexpr std::array<UiImageQualityRect,3> rows{{
        {ImageQualityId::DesktopFilter,.025f,.20f,.935f,.12f},
        {ImageQualityId::VrUpscaling,.025f,.35f,.935f,.12f},
        {ImageQualityId::VrTimewarp,.025f,.50f,.935f,.12f}
    }};return rows;
}
void VrUiModel::set_image_quality(const ImageQualityPreferences& preferences){
    if(!valid_image_quality(preferences)||preferences==image_quality_)return;
    image_quality_=preferences;++revision_;
}
void VrUiModel::set_input_preferences(const InputPreferences& preferences){
    if(preferences==input_preferences_)return;
    input_preferences_=preferences;shortcut_offset_=std::min(shortcut_offset_,static_cast<unsigned>(preferences.input_shortcuts.size()>3?preferences.input_shortcuts.size()-3:0));build_keys();++revision_;
}
void VrUiModel::toggle_keyboard(const UiEventCallback& events){
    if(settings_visible_){show_settings(false,events);keyboard_.visible=true;}
    else{release_inputs(events);keyboard_.visible=!keyboard_.visible;}
    ++revision_;
}
void VrUiModel::show_settings(bool visible,const UiEventCallback& events){
    if(settings_visible_==visible)return;
    release_inputs(events);
    if(visible){keyboard_before_settings_=keyboard_.visible;keyboard_.visible=true;settings_scroll_=0;}
    else keyboard_.visible=keyboard_before_settings_;
    settings_visible_=visible;++revision_;
}
std::array<bool,2> VrUiModel::input_capture_mask() const {
    if(settings_visible_||keyboard_.visible||pending_reset_>=0)return {true,true};
    std::array<bool,2> result{};
    for(std::size_t i=0;i<result.size();++i)result[i]=hits_[i].valid||hands_[i].drag>=0||button_hand_==static_cast<int>(i);
    return result;
}
void VrUiModel::activate_settings(float u,float v,const UiEventCallback& events){
    if(pending_reset_==-1){
        if(in_rect(u,v,.52f,.65f,.22f,.12f)){
            const int target=pending_reset_;release_inputs(events);
            if(target>=0)reset_panel(static_cast<unsigned>(target));
            else {keyboard_binding_=0;keyboard_offset_={{0,.16f,-.28f},{}};keyboard_space_width_=2.05f;layout();}
        }else if(in_rect(u,v,.26f,.65f,.22f,.12f))release_inputs(events);
        return;
    }
    if(in_rect(u,v,.70f,.025f,.275f,.10f)){show_settings(false,events);return;}
    if(in_rect(u,v,.755f,.925f,.22f,.065f)){request_reset(-1,events);return;}
    for(unsigned i=0;i<tab_names.size();++i)if(in_rect(u,v,tab_x+tab_step*i,.025f,tab_width,.10f)){
        const auto page=static_cast<UiSettingsPage>(i);if(settings_page_!=page){settings_page_=page;settings_scroll_=0;++revision_;}return;
    }
    const auto enabled=menu_actions_enabled(remote_settings_.capabilities);
    constexpr std::array<UiEventType,4> actions{UiEventType::LaunchVr,UiEventType::ShowDesktop,UiEventType::ExitVr,UiEventType::Recenter};
    for(unsigned i=0;i<4;++i)if(enabled[i]&&in_rect(u,v,.025f+.245f*i,.815f,.225f,.10f)){
        release_inputs(events);UiEvent event;event.type=actions[i];emit(events,event);return;
    }
    if(v<settings_top||v>=settings_bottom||u>=scroll_x)return;
    v+=settings_scroll_;
    if(settings_page_==UiSettingsPage::Diagnostics)for(unsigned i=0;i<diagnostics_tab_names.size();++i)
        if(in_rect(u,v,.025f+diagnostics_tab_step*i,diagnostics_tab_y,diagnostics_tab_width,diagnostics_tab_height)){
            const auto page=static_cast<UiDiagnosticsPage>(i);if(diagnostics_page_!=page){diagnostics_page_=page;++revision_;}return;
        }
    if(settings_page_==UiSettingsPage::Image)for(const auto& row:image_quality_rows()){
        if(!in_rect(u,v,row.x,row.y,row.width,row.height))continue;
        const float relative=(u-row.x)/row.width;if(relative<.74f)return;
        const int current=image_quality_value(image_quality_,row.id);
        const int next=std::clamp(current+(relative>=.875f?1:-1),0,1);
        if(next==current)return;
        UiEvent event;event.type=UiEventType::ImageQualityChanged;event.image_quality={row.id,next};emit(events,event);return;
    }
    if(settings_page_==UiSettingsPage::Stream&&in_rect(u,v,monitors_x,monitors_y,monitors_width,controllers_height)){
        const float relative=(u-monitors_x)/monitors_width;if(relative<.74f)return;
        const bool add=relative>=.875f;
        if(!monitor_actions_enabled(remote_settings_.capabilities)[add?1:0])return;
        release_inputs(events);UiEvent event;event.type=add?UiEventType::AddMonitor:UiEventType::RemoveMonitor;
        emit(events,event);return;
    }
    for(const auto& row:preference_rows)if(preference_visible(row,settings_page_,diagnostics_page_)&&in_rect(u,v,row.x,row.y,monitors_width,controllers_height)){
        const float relative=(u-row.x)/monitors_width;if(relative<.74f)return;
        const auto& spec=*input_preference_spec(row.id);const int current=input_preference_value(input_preferences_,row.id);
        const bool plus=relative>=.875f;const int next=spec.boolean?int(plus):std::clamp(current+(plus?5:-5),spec.minimum,spec.maximum);
        if(next==current)return;
        UiEvent event;event.type=UiEventType::InputPreferenceChanged;event.input_preference={row.id,next};emit(events,event);return;
    }
    if(settings_page_==UiSettingsPage::Devices&&in_rect(u,v,monitors_x,system_capture_y,monitors_width,controllers_height)){
        const float relative=(u-monitors_x)/monitors_width;if(relative<.74f)return;
        const bool next=relative>=.875f;
        if(next==system_capture_active_||(next&&!system_capture_available_))return;
        UiEvent event;event.type=UiEventType::SystemCaptureChanged;event.pressed=next;emit(events,event);return;
    }
    for(const auto& row:settings_rows()){
        if(!setting_visible(settings_page_,row.id))continue;
        if(!in_rect(u,v,row.x,row.y,row.width,row.height))continue;
        const float relative=(u-row.x)/row.width;
        if(relative<.74f)return;
        const auto choices=setting_choices(row.id,remote_settings_);if(choices.empty())return;
        const auto& value=remote_settings_.values[static_cast<std::size_t>(row.id)];double next=value.requested;
        if(relative>=.875f){const auto it=std::upper_bound(choices.begin(),choices.end(),value.requested+.00001);if(it!=choices.end())next=*it;}
        else{const auto it=std::lower_bound(choices.begin(),choices.end(),value.requested-.00001);if(it!=choices.begin())next=*std::prev(it);}
        SettingChange change{row.id,next};if(next==value.requested||!setting_request_allowed(change,remote_settings_))return;
        // Only the bridge knows whether enqueue succeeded. Its next snapshot
        // marks SENT; do not leave a rejected request looking permanently busy.
        UiEvent event;event.type=UiEventType::RemoteSettingChanged;event.setting=change;emit(events,event);return;
    }
}
void VrUiModel::scroll_settings(float amount){
    if(!std::isfinite(amount))return;
    const auto next=std::clamp(settings_scroll_+amount,0.f,settings_content_bottom-settings_bottom);
    if(next==settings_scroll_)return;
    settings_scroll_=next;++revision_;
}
void VrUiModel::update(const std::array<UiHand,2>& raw,float delta,const UiEventCallback& events){
    if(!std::isfinite(delta)||delta<=0||delta>.1f)desktop_pointers_={};
    delta=std::clamp(std::isfinite(delta)?delta:0.f,0.f,.05f);const auto old_hits=hits_;
    auto input=raw;std::array<bool,2> active_hands{};bool lost_tracking=false;
    for(std::size_t i=0;i<2;++i){auto& in=input[i];
        active_hands[i]=in.active&&valid_pose(in.aim)&&std::isfinite(in.stick_x)&&std::isfinite(in.stick_y)&&std::isfinite(in.trigger)&&std::isfinite(in.squeeze);
        if(active_hands[i])in.aim=normalized_pose(in.aim);
        if(in.trigger>=0){
            hands_[i].trigger_down=hands_[i].trigger_down?in.trigger>.20f:in.trigger>=.45f;
            in.select=in.select||hands_[i].trigger_down;
        }
        if(in.squeeze>=0)in.grip=hands_[i].grip?in.squeeze>.35f:in.squeeze>=.55f;
        lost_tracking|=hands_[i].active&&!active_hands[i];
    }
    if(lost_tracking)release_inputs(events);
    for(auto& repeat:shortcut_repeat_)repeat=std::max(0.f,repeat-delta);
    update_keyboard_attachment(input,active_hands);
    for(auto& remaining:controls_remaining_){const bool was=remaining>0;remaining=std::max(0.f,remaining-delta);if(was&&remaining==0)++revision_;}
    const auto wake=[&](unsigned id){if(id>=panels_.size())return;if(controls_remaining_[id]==0)++revision_;controls_remaining_[id]=.9f;controls_order_[id]=++controls_clock_;};
    const int captured_hand=button_hand_;
    for(std::size_t i=0;i<2;++i){
        const auto& in=input[i];auto& h=hands_[i];hits_[i]={};const bool active=active_hands[i];h.active=active;
        if(!active){h={};desktop_pointers_[i]={};continue;}
        const auto consider=[&](UiHit hit){if(hit.valid&&(!hits_[i].valid||hit.distance<hits_[i].distance))hits_[i]=hit;};
        if(!settings_visible_&&(!keyboard_.visible||keyboard_binding_!=i+1))for(const auto& p:panels_){
            consider(ui_hit_panel(p,in.aim));
            if(!p.visible||!p.available)continue;
            auto controls=panel_controls(p.panel_id);controls.bottom.visible=true;
            auto bottom=ui_hit_panel(controls.bottom,in.aim);bottom.region=UiHitRegion::BottomControls;consider(bottom);
            auto top=ui_hit_panel(controls.top,in.aim);top.region=UiHitRegion::TopControls;consider(top);
        }
        auto keyhit=keyboard_.visible&&keyboard_binding_==i+1?UiHit{}:ui_hit_panel(keyboard_,in.aim);keyhit.keyboard=true;keyhit.region=UiHitRegion::Keyboard;consider(keyhit);
        if(keyboard_binding_!=i+1){auto candidate=ui_hit_panel(candidate_panel(),in.aim);candidate.keyboard=true;candidate.region=UiHitRegion::Candidate;consider(candidate);}
        if(pending_reset_>=0){
            hits_[i]={};const auto target=static_cast<unsigned>(pending_reset_);
            auto bottom=ui_hit_panel(panel_controls(target).bottom,in.aim);bottom.region=UiHitRegion::BottomControls;consider(bottom);
        }
        const auto raw_hit=hits_[i];
        auto hit=raw_hit;
        if(captured_hand<0)hit=hits_[i]=desktop_pointer_hit(i,in,hit,delta);
        else if(captured_hand!=static_cast<int>(i))desktop_pointers_[i]={};
        if(hit.valid&&(hit.region==UiHitRegion::BottomControls||hit.region==UiHitRegion::TopControls))wake(hit.panel_id);
        if(!in.select&&!in.grip&&!in.right_click)h.armed=true;
        // Held Menu buttons have no action, including when they are released.
        if(in.menu)h.menu_seconds=std::min(h.menu_seconds+delta,1.5f);
        else if(h.menu){if(h.menu_seconds<1.5f)toggle_keyboard(events);h.menu_seconds=0;}
        // Keep pending clicks and real drags on the original video source.
        if(button_hand_==static_cast<int>(i))hit=hits_[i]=desktop_pointer_capture(in,delta,events);
        if(h.drag>=0){
            if(h.drag_by_grip?in.grip:in.select){
                auto& panel=h.drag==4?keyboard_:panels_.at(static_cast<std::size_t>(h.drag));
                if(h.drag<4)wake(static_cast<unsigned>(h.drag));
                if(h.adjust>=0){const auto adjust_hit=ui_hit_panel(h.adjust_plane,in.aim);if(adjust_hit.valid){
                    const float change=adjust_hit.u-h.adjust_u;
                    if(h.adjust==0){const float ratio=panel.height/panel.width;panel.width=std::clamp(h.initial_width*std::exp(change*4.f),kUiMinPanelWidthM,kUiMaxPanelWidthM);panel.height=panel.width*ratio;++scale_updates_;}
                    else panel.curvature_radians=std::clamp(h.initial_curve+change*3.6f,0.f,1.8f);
                    ++revision_;
                }}else{
                    if(discontinuous_grab(h.last_drag_aim,in.aim)){h.drag=-1;h.adjust=-1;h.armed=false;}
                    else{
                        h.last_drag_aim=in.aim;
                        // A neutral stick must not clamp a far/off-axis panel and teleport it.
                        if(std::abs(in.stick_y)>.2f){
                            const float depth=h.relative.position.z;
                            const float next=std::clamp(depth-std::clamp(in.stick_y,-1.f,1.f)*delta,std::min(depth,-5.f),std::max(depth,-.35f));
                            h.relative.position.z=next;
                        }
                        panel.pose=normalized_pose(ui_compose(in.aim,h.relative));
                        if(h.drag==4&&keyboard_binding_)keyboard_offset_=normalized_pose(ui_compose(ui_inverse(keyboard_attachment_),panel.pose));
                        ++drag_updates_;
                        if(std::abs(in.stick_x)>.2f){const float ratio=panel.height/panel.width;panel.width=std::clamp(panel.width*std::exp(in.stick_x*delta),kUiMinPanelWidthM,kUiMaxPanelWidthM);panel.height=panel.width*ratio;++scale_updates_;++revision_;}
                    }
                }
            }else{h.drag=-1;h.adjust=-1;}
        }else if(hit.valid){
            const bool pressed=h.armed&&in.select&&!h.select;
            auto& panel=hit.keyboard?keyboard_:panels_.at(hit.panel_id);
            bool dragging=h.armed&&in.grip&&!h.grip&&hit.region!=UiHitRegion::Candidate;
            bool owned=false;for(std::size_t other=0;other<2;++other)if(other!=i&&hands_[other].drag==static_cast<int>(panel.panel_id))owned=true;
            if(pending_reset_>=0){
                dragging=false;
                if(pressed&&!in.grip&&hit.region==UiHitRegion::BottomControls){
                    const auto target=static_cast<unsigned>(pending_reset_);
                    if(in_rect(hit.u,hit.v,.26f,.29f,.225f,.34f)){release_inputs(events);reset_panel(target);}
                    else if(in_rect(hit.u,hit.v,.025f,.29f,.21f,.34f))release_inputs(events);
                }
            }else if(hit.region==UiHitRegion::TopControls&&pressed&&!owned){
                h.drag=static_cast<int>(hit.panel_id);h.drag_by_grip=false;h.adjust=hit.u<.5f?0:1;h.adjust_plane=panel_controls(hit.panel_id).top;
                h.adjust_u=hit.u;h.initial_width=panel.width;h.initial_curve=panel.curvature_radians;dragging=false;
            }else if(hit.region==UiHitRegion::BottomControls&&pressed&&!owned){
                if(hit.v>=.72f)dragging=true;
                else{
                    const float centers[]={.12f,.375f,.625f,.88f};const auto strip=panel_controls(hit.panel_id).bottom;
                    for(unsigned button=0;button<4;++button){const float x=(hit.u-centers[button])*strip.width,y=(hit.v-.31f)*strip.height;
                        if(x*x+y*y<=.09f*.09f){
                            if(button==0){request_reset(static_cast<int>(hit.panel_id),events);}
                            else if(button==1){release_inputs(events);keyboard_.visible=!keyboard_.visible;++revision_;}
                            else if(button==2)set_visible(hit.panel_id,false,events);
                            else show_settings(true,events);
                            break;
                        }
                    }
                }
            }
            if(dragging){
                if(!owned){h.drag=static_cast<int>(panel.panel_id);h.drag_by_grip=in.grip;h.adjust=-1;h.last_drag_aim=in.aim;h.relative=normalized_pose(ui_compose(ui_inverse(in.aim),panel.pose));}
            }else if(hit.region==UiHitRegion::Candidate){
                if(pressed&&!in.grip){UiEvent click;click.type=UiEventType::PointerButton;click.candidate=candidate_token_;click.u=hit.u;click.v=hit.v;click.pressed=true;emit(events,click);}
            }else if(hit.keyboard){
                if(!settings_visible_&&!shortcut_recording()&&!in.grip&&!in.select&&hit.v>=.92f&&hit.u>=.35f){
                    const float axis=std::abs(in.stick_x)>=std::abs(in.stick_y)?in.stick_x:-in.stick_y;
                    if(std::abs(axis)>.55f&&shortcut_repeat_[i]<=0){scroll_shortcuts(axis>0?1:-1);shortcut_repeat_[i]=.25f;}
                    if(std::abs(axis)<.25f)shortcut_repeat_[i]=0;
                }
                if(settings_visible_&&!in.select&&!in.grip&&std::abs(in.stick_y)>.2f&&hit.v>=settings_top&&hit.v<settings_bottom)
                    scroll_settings(-std::copysign((std::min(std::abs(in.stick_y),1.f)-.2f)/.8f,in.stick_y)*delta*.6f);
                if(pressed&&settings_visible_)activate_settings(hit.u,hit.v,events);
                else if(pressed)for(std::size_t k=0;k<keys_.size();++k){const auto& key=keys_[k];if(hit.u>=key.x&&hit.u<=key.x+key.width&&hit.v>=key.y&&hit.v<=key.y+key.height){activate_key(k,events);break;}}
            }else if(hit.region==UiHitRegion::Video){
                const auto move=desktop_pointer_event(hit);
                const bool free=button_hand_<0&&captured_hand<0;
                if(free)emit(events,move);
                if(pressed&&free){focus_source_=panel.source_id;focus_panel_=panel.panel_id;button_event_=move;button_event_.type=UiEventType::PointerButton;button_event_.button=0;button_hand_=static_cast<int>(i);press_hit_=hit;drag_origin_=raw_hit;press_seconds_=drag_seconds_=0;mouse_down_=false;}
                if(h.armed&&in.right_click&&!h.right_click&&free&&!pressed){auto click=move;click.type=UiEventType::PointerButton;click.button=1;click.pressed=true;emit(events,click);click.pressed=false;emit(events,click);}
                if((captured_hand<0||captured_hand==static_cast<int>(i))&&(button_hand_<0||button_hand_==static_cast<int>(i))&&!in.grip&&std::abs(in.stick_y)>.2f){auto scroll=move;scroll.type=UiEventType::Scroll;scroll.scroll_y=std::copysign((std::min(std::abs(in.stick_y),1.f)-.2f)/.8f,in.stick_y)*delta*1440.f;emit(events,scroll);}
            }
        }
        h.select=in.select;h.grip=in.grip;h.menu=in.menu;h.right_click=in.right_click;
        if(exit_)break;
    }
    // At most two visible control strips (one per hand), retaining active grabs.
    while(std::count_if(controls_remaining_.begin(),controls_remaining_.end(),[](float value){return value>0;})>2){
        int oldest=-1;for(unsigned i=0;i<panels_.size();++i){bool held=false;for(const auto& hand:hands_)held|=hand.drag==static_cast<int>(i);
            if(controls_remaining_[i]>0&&!held&&(oldest<0||controls_order_[i]<controls_order_[static_cast<unsigned>(oldest)]))oldest=static_cast<int>(i);}
        if(oldest<0)break;
        controls_remaining_[static_cast<unsigned>(oldest)]=0;++revision_;
    }
    const auto hover_index=[&](UiHit hit){if(!hit.valid||hit.region==UiHitRegion::Candidate)return -1;if(hit.region==UiHitRegion::TopControls)return 200+static_cast<int>(hit.panel_id)*10+(hit.u<.5f?0:1);if(hit.region==UiHitRegion::BottomControls)return 300+static_cast<int>(hit.panel_id)*10+static_cast<int>(hit.u*4);if(!hit.keyboard)return -1;if(settings_visible_)return 500+static_cast<int>(hit.v*10)*20+static_cast<int>(hit.u*20);for(std::size_t i=0;i<keys_.size();++i){const auto& k=keys_[i];if(hit.u>=k.x&&hit.u<=k.x+k.width&&hit.v>=k.y&&hit.v<=k.y+k.height)return static_cast<int>(i);}return -1;};
    for(std::size_t i=0;i<2;++i)if(hover_index(old_hits[i])!=hover_index(hits_[i]))++revision_;
}

} // namespace surfxr
