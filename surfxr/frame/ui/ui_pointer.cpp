#include "surfxr/vr_ui.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr {
namespace {
float separation(const UiPanel& panel,const UiHit& a,const UiHit& b){
    const float x=(a.u-b.u)*panel.width,y=(a.v-b.v)*panel.height;
    return std::sqrt(x*x+y*y);
}
UiHit positioned(const UiPanel& panel,UiHit hit,const UiPose& aim){
    hit.point=ui_panel_point(panel,hit.u,hit.v);
    if(panel.curvature_radians>.001f&&panel.curve_segments>=2&&panel.curve_segments<=64){
        const float segment=hit.u*panel.curve_segments;
        const unsigned index=std::min(panel.curve_segments-1,static_cast<unsigned>(segment));
        const auto a=ui_panel_point(panel,float(index)/panel.curve_segments,hit.v);
        const auto b=ui_panel_point(panel,float(index+1)/panel.curve_segments,hit.v);
        const float t=segment-index;
        hit.point={a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};
    }
    const auto a=aim.position,b=hit.point;
    hit.distance=std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));
    return hit;
}
}
UiEvent VrUiModel::desktop_pointer_event(const UiHit& hit) const {
    const auto& p=panels_.at(hit.panel_id);UiEvent e;e.type=UiEventType::PointerMove;
    e.panel_id=p.panel_id;e.source_id=p.source_id;
    e.u=std::clamp(p.crop.x+hit.u*p.crop.width,0.f,1.f);
    e.v=std::clamp(p.crop.y+hit.v*p.crop.height,0.f,1.f);return e;
}
UiHit VrUiModel::desktop_pointer_hit(std::size_t hand,const UiHand& in,UiHit hit,float delta){
    auto& state=desktop_pointers_[hand];
    if(!hit.valid||hit.region!=UiHitRegion::Video||in.grip||hands_[hand].drag>=0){state={};return hit;}
    const auto& panel=panels_.at(hit.panel_id);
    const bool same=state.hover.valid&&state.hover.panel_id==hit.panel_id;
    // Never interpolate across screens, tracking gaps or deliberate large moves.
    if(!same||delta<=0||separation(panel,state.hover,hit)>.025f){
        state={};state.hover=state.quiet_origin=hit;state.trigger_started=in.trigger>=.10f;return hit;
    }
    const auto previous=state.hover;
    const bool settled=state.quiet_seconds>=.05f;
    if(separation(panel,state.quiet_origin,hit)<=.003f)state.quiet_seconds=std::min(.1f,state.quiet_seconds+delta);
    else{state.quiet_origin=hit;state.quiet_seconds=0;}
    if(in.trigger<=.08f){state.trigger_started=false;state.prepared={};}
    if(in.trigger>=.10f&&!state.trigger_started){
        state.trigger_started=true;state.prepare_seconds=0;
        if(settled)state.prepared=previous;
    }
    bool expired=false;
    if(state.prepared.valid){
        state.prepare_seconds+=delta;
        if(state.prepare_seconds>.25f||separation(panel,state.prepared,hit)>.025f){
            state.prepared={};state.quiet_seconds=0;expired=true;
        }
    }
    const bool edge=(in.select&&!hands_[hand].select)||(in.right_click&&!hands_[hand].right_click);
    if(state.prepared.valid)hit=state.prepared;
    else if(edge){
        // A settled, recently displayed target survives a small button squeeze.
        // Moving onto another target and clicking immediately uses that target.
        if(settled&&!expired&&separation(panel,previous,hit)<=.012f)hit=previous;
    }else{
        // Damp fine tremor, but reduce the time constant as intentional motion grows.
        const float motion=std::clamp(separation(panel,previous,hit)/.015f,0.f,1.f);
        const float tau=.035f+(.004f-.035f)*motion,alpha=-std::expm1(-delta/tau);
        hit.u=previous.u+(hit.u-previous.u)*alpha;hit.v=previous.v+(hit.v-previous.v)*alpha;
    }
    state.hover=positioned(panel,hit,in.aim);return state.hover;
}
UiHit VrUiModel::desktop_pointer_capture(const UiHand& in,float delta,const UiEventCallback& events){
    const auto emit=[&](const UiEvent& event){if(events)events(event);};
    const auto& panel=panels_.at(button_event_.panel_id);
    const auto raw=ui_hit_panel(panel,in.aim);
    auto shown=press_hit_;press_seconds_+=delta;
    if(!in.select||in.grip){
        if(mouse_down_){button_event_.pressed=false;emit(button_event_);}
        else if(!in.grip){button_event_.pressed=true;emit(button_event_);button_event_.pressed=false;emit(button_event_);}
        auto& state=desktop_pointers_[static_cast<std::size_t>(button_hand_)];state={};
        state.hover=shown;state.quiet_origin=raw;state.trigger_started=in.trigger>=.10f;
        button_hand_=-1;mouse_down_=false;press_seconds_=drag_seconds_=0;
        if(in.grip)return raw;
    }else if(raw.valid){
        // Measure intent from the raw down pose: squeeze displacement between the
        // stable target and that pose must not itself turn a click into a drag.
        if(separation(panel,raw,drag_origin_)>=.018f)drag_seconds_+=delta;else drag_seconds_=0;
        if(!mouse_down_&&press_seconds_>=.10f&&drag_seconds_>=.06f){
            button_event_.pressed=true;emit(button_event_);mouse_down_=true;
        }
        if(mouse_down_){
            const auto move=desktop_pointer_event(raw);emit(move);
            button_event_.u=move.u;button_event_.v=move.v;shown=press_hit_=raw;
        }
    }else drag_seconds_=0;
    // The visible cursor and click use the same point, including the release frame.
    return positioned(panel,shown,in.aim);
}
}
