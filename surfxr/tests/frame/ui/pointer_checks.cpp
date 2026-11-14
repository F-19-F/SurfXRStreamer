#include "surfxr/vr_ui.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace surfxr {
namespace {
void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
bool near(float a,float b){return std::abs(a-b)<.0001f;}
UiPose aim_at(const UiPanel& panel,float u,float v){
    const float theta=(u-.5f)*panel.curvature_radians;
    const auto point=ui_panel_point(panel,u,v);
    const auto normal=ui_rotate(panel.pose.orientation,{-std::sin(theta),0,std::cos(theta)});
    return {{point.x+normal.x,point.y+normal.y,point.z+normal.z},
        ui_multiply(panel.pose.orientation,{0,std::sin(-theta/2),0,std::cos(theta/2)})};
}
struct PointerFixture {
    VrUiModel model;
    std::array<UiHand,2> hands{};
    std::vector<UiEvent> events;
    explicit PointerFixture(float curve=0,UiCrop crop={}){
        UiPanel a;a.source_id=3;a.crop=crop;
        UiPanel b;b.source_id=8;model.set_panels({a,b});model.reset_anchor({});
        if(curve){auto layout=model.export_layout();layout.panels[0].curvature_radians=curve;
            require(model.restore_layout(layout),"pointer_curved_layout");}
        hands[0].active=true;aim(0,0,.5f,.5f);settle();events.clear();
    }
    void tick(float seconds=.02f){model.update(hands,seconds,[&](const auto& e){events.push_back(e);});}
    void settle(){for(int i=0;i<4;++i)tick();}
    void aim(unsigned hand,unsigned panel,float u,float v=.5f){hands[hand].aim=aim_at(model.panels()[panel],u,v);}
    void offset(float meters,unsigned hand=0,unsigned panel=0){aim(hand,panel,.5f+meters/model.panels()[panel].width);}
    std::size_t count(UiEventType type)const{
        return std::count_if(events.begin(),events.end(),[&](const auto& e){return e.type==type;});
    }
    void visual(float u,float v=.5f,unsigned hand=0)const{
        const auto& hit=model.hits()[hand];require(hit.valid&&near(hit.u,u)&&near(hit.v,v),"pointer_visual_target");
        const auto& panel=model.panels()[hit.panel_id];auto point=ui_panel_point(panel,u,v);
        if(panel.curvature_radians>.001f&&panel.curve_segments){
            const float index=std::floor(u*panel.curve_segments),t=u*panel.curve_segments-index;
            const auto a=ui_panel_point(panel,index/panel.curve_segments,v);
            const auto b=ui_panel_point(panel,(index+1)/panel.curve_segments,v);
            point={a.x*(1-t)+b.x*t,a.y*(1-t)+b.y*t,a.z*(1-t)+b.z*t};
        }
        require(near(hit.point.x,point.x)&&near(hit.point.y,point.y)&&near(hit.point.z,point.z),"pointer_visual_world_point");
    }
    void pair(float u,float v=.5f,unsigned button=0,unsigned source=3)const{
        require(count(UiEventType::PointerButton)==2,"pointer_exact_click_pair");
        bool down=true;
        for(const auto& e:events)if(e.type==UiEventType::PointerButton){
            require(e.pressed==down&&e.button==button&&e.source_id==source&&near(e.u,u)&&near(e.v,v),"pointer_click_pair_target");down=false;
        }
    }
    void all_moves(float u,float v=.5f)const{
        for(const auto& e:events)if(e.type==UiEventType::PointerMove)
            require(near(e.u,u)&&near(e.v,v),"pointer_no_phantom_motion");
    }
};
void squeeze_checks(){
    PointerFixture f;
    f.hands[0].trigger=.12f;f.offset(.006f);f.tick();f.visual(.5f);
    f.hands[0].trigger=.30f;f.offset(.011f);f.tick();f.visual(.5f);
    f.hands[0].trigger=.46f;f.offset(.020f);f.tick();f.visual(.5f);
    require(f.count(UiEventType::PointerButton)==0,"squeeze_waits_for_release");
    for(int i=0;i<6;++i){f.offset(i%2?.024f:.017f);f.tick(.03f);f.visual(.5f);}
    require(f.count(UiEventType::PointerButton)==0,"squeeze_displacement_not_drag_intent");
    f.all_moves(.5f);f.events.clear();
    f.hands[0].trigger=.19f;f.offset(.06f);f.tick();
    f.pair(.5f);f.visual(.5f);
    require(f.count(UiEventType::PointerMove)==0,"squeeze_release_does_not_move_cursor");
    f.hands[0].trigger=0;f.tick();
    require(f.model.hits()[0].u>.52f,"squeeze_hover_resumes_after_release");
}
void button_checks(){
    for(bool right:{false,true})for(float sign:{-1.f,1.f}){
        PointerFixture f;f.offset(sign*.009f);
        if(right)f.hands[0].right_click=true;else f.hands[0].select=true;
        f.tick();f.visual(.5f);f.all_moves(.5f);
        if(right){f.pair(.5f,.5f,1);f.tick();f.pair(.5f,.5f,1);}
        else{
            require(f.count(UiEventType::PointerButton)==0,"digital_press_pending");
            f.events.clear();f.offset(sign*.014f);f.tick();f.visual(.5f);
            require(f.events.empty(),"digital_pending_suppresses_tremor");
            f.hands[0].select=false;f.tick();f.pair(.5f);f.visual(.5f);
            require(f.count(UiEventType::PointerMove)==0,"digital_release_no_phantom_move");
        }
    }
    // A fast deliberate retarget is already beyond the small button-squeeze radius.
    PointerFixture f;f.offset(.020f);f.hands[0].select=true;f.tick();
    const float target=.5f+.020f/f.model.panels()[0].width;
    f.visual(target);f.hands[0].select=false;f.tick();f.pair(target);
}
void hover_checks(){
    PointerFixture f;f.offset(.0015f);f.tick(.005f);
    const float displacement=(f.model.hits()[0].u-.5f)*f.model.panels()[0].width;
    require(displacement>0&&displacement<.0012f,"fine_tremor_is_damped");
    f.all_moves(f.model.hits()[0].u);
    f.events.clear();f.offset(.08f);f.tick(.005f);
    const float target=.5f+.08f/f.model.panels()[0].width;
    f.visual(target);f.all_moves(target);
    // A moving cursor must not qualify as settled merely because each sample is recent.
    PointerFixture moving;
    for(float x:{.006f,.012f,.018f}){moving.offset(x);moving.tick();}
    moving.events.clear();moving.offset(.024f);moving.hands[0].select=true;moving.tick();
    const float next=.5f+.024f/moving.model.panels()[0].width;
    moving.visual(next);moving.hands[0].select=false;moving.tick();moving.pair(next);
    for(float rate:{72.f,120.f,144.f}){
        PointerFixture sweep;const float delta=1/rate;float x=0;
        for(int i=0;i<20;++i){x+=.24f*delta;sweep.offset(x);sweep.tick(delta);}
        sweep.hands[0].trigger=.10f;
        for(int i=0;i<3;++i){x+=.24f*delta;sweep.offset(x);sweep.tick(delta);}
        sweep.events.clear();x+=.24f*delta;sweep.offset(x);sweep.hands[0].trigger=.45f;sweep.tick(delta);
        const float target=.5f+x/sweep.model.panels()[0].width;sweep.visual(target);
        sweep.hands[0].trigger=0;sweep.tick(delta);sweep.pair(target);
    }
}
void preparation_checks(){
    PointerFixture f;f.offset(.006f);f.hands[0].trigger=.12f;f.tick();f.visual(.5f);
    f.offset(.010f);f.hands[0].trigger=.09f;f.tick();f.visual(.5f);
    f.hands[0].trigger=.07f;f.tick();
    require(f.model.hits()[0].u>.5f,"partial_squeeze_full_rearm_unlocks");
    f.settle();const float target=f.model.hits()[0].u;
    f.offset(.015f);f.hands[0].trigger=.12f;f.tick();f.visual(target);
    f.hands[0].trigger=.46f;f.tick();f.hands[0].trigger=0;f.tick();f.pair(target);

    PointerFixture timeout;timeout.offset(.008f);timeout.hands[0].trigger=.12f;timeout.tick();
    for(int i=0;i<6;++i)timeout.tick(.05f);
    require(timeout.model.hits()[0].u>.5f,"partial_squeeze_timeout_unlocks");
    timeout.settle();const float after_timeout=timeout.model.hits()[0].u;
    timeout.hands[0].trigger=.09f;timeout.tick();
    timeout.offset(.014f);timeout.hands[0].trigger=.12f;timeout.tick();
    require(timeout.model.hits()[0].u>after_timeout+.0005f,"timeout_does_not_relatch_without_rearm");

    PointerFixture motion;motion.offset(.006f);motion.hands[0].trigger=.12f;motion.tick();
    motion.offset(.05f);motion.tick();
    const float moved=.5f+.05f/motion.model.panels()[0].width;
    motion.visual(moved);motion.settle();motion.offset(.057f);motion.tick();
    require(motion.model.hits()[0].u>moved+.0005f,"intentional_motion_cancels_squeeze_without_relatch");
}
void reset_checks(){
    for(unsigned reset=0;reset<5;++reset){
        PointerFixture f;
        if(reset==0){f.offset(.008f);f.tick(.2f);}
        else if(reset==1){f.hands[0].aim.position.x=10;f.tick();f.offset(.008f);f.tick();}
        else if(reset==2){f.hands[0].active=false;f.tick();f.hands[0].active=true;f.offset(.008f);f.tick();}
        else if(reset==3){f.model.set_available(0,false,{});f.tick();f.model.set_available(0,true,{});f.offset(.008f);f.tick();}
        else{f.model.set_aspect(0,.8f);f.offset(.008f);f.tick();}
        f.events.clear();f.offset(.012f);f.hands[0].select=true;f.tick();
        const float target=.5f+.012f/f.model.panels()[0].width;
        f.visual(target);f.hands[0].select=false;f.tick();f.pair(target);
    }
    PointerFixture source;source.aim(0,1,.5f);source.tick();source.events.clear();
    source.offset(.008f,0,1);source.hands[0].select=true;source.tick();
    const float target=.5f+.008f/source.model.panels()[1].width;
    source.hands[0].select=false;source.tick();source.pair(target,.5f,0,8);
    require(source.model.hits()[0].panel_id==1,"screen_change_preserves_source_identity");

    PointerFixture loss;loss.hands[0].select=true;loss.tick();loss.events.clear();
    loss.hands[0].active=false;loss.tick();
    require(loss.count(UiEventType::ReleaseAll)==1&&loss.count(UiEventType::PointerButton)==0,"tracking_loss_cancels_pending_click");
    loss.hands[0].active=true;loss.tick();loss.hands[0].select=false;loss.tick();
    require(loss.count(UiEventType::PointerButton)==0,"tracking_return_does_not_replay_click");
}
void drag_checks(){
    PointerFixture f;f.offset(.008f);f.hands[0].select=true;f.tick();
    f.tick(.05f);f.tick(.05f);f.events.clear();
    // More than 18 mm from the visible target is still less than 18 mm from down.
    f.offset(.024f);for(int i=0;i<4;++i)f.tick(.03f);
    require(f.count(UiEventType::PointerButton)==0,"drag_uses_raw_down_origin");
    f.offset(.05f);f.tick(.02f);f.offset(.008f);f.tick(.02f);
    require(f.count(UiEventType::PointerButton)==0,"transient_drag_spike_is_ignored");
    f.offset(.05f);f.tick(.02f);f.tick(.02f);
    require(f.count(UiEventType::PointerButton)==0,"drag_requires_sustained_motion");
    f.tick(.03f);
    require(f.count(UiEventType::PointerButton)==1,"intentional_drag_starts_once");
    const auto down=std::find_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;});
    require(down->pressed&&near(down->u,.5f),"drag_down_at_stabilized_target");
    const float dragged=.5f+.05f/f.model.panels()[0].width;f.visual(dragged);
    f.events.clear();f.offset(.09f);f.hands[0].select=false;f.tick();f.visual(dragged);
    require(f.events.size()==1&&f.events[0].type==UiEventType::PointerButton&&!f.events[0].pressed&&near(f.events[0].u,dragged),"drag_release_at_last_admitted_position");
}
void surface_checks(){
    for(unsigned surface=0;surface<3;++surface){
        PointerFixture f(surface?1.4f:0.f,{.2f,.1f,.6f,.75f});
        if(surface==2)f.model.set_curve_segments(0,16);
        f.aim(0,0,.65625f,.4f);f.settle();const auto target=f.model.hits()[0];f.events.clear();
        f.hands[0].trigger=.12f;f.aim(0,0,.65625f+.007f/f.model.panels()[0].width,.4f);f.tick();
        f.visual(target.u,target.v);f.hands[0].trigger=.46f;f.tick();f.hands[0].trigger=0;f.tick();
        f.pair(.2f+target.u*.6f,.1f+target.v*.75f);f.visual(target.u,target.v);
        f.all_moves(.2f+target.u*.6f,.1f+target.v*.75f);
    }
}
void two_hand_checks(){
    for(unsigned owner=0;owner<2;++owner){
        PointerFixture f;const unsigned other=1-owner;
        f.hands[0].active=f.hands[1].active=true;f.aim(owner,0,.5f);f.aim(other,1,.5f);f.settle();
        f.hands[owner].select=true;f.tick();f.events.clear();
        f.offset(.009f,owner);f.hands[owner].select=false;
        f.hands[other].right_click=true;f.hands[other].stick_y=1;f.tick();
        f.pair(.5f);f.visual(.5f,.5f,owner);
        require(f.count(UiEventType::PointerMove)==0,"other_hand_cannot_move_on_release_frame");
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.source_id==8;}),"other_hand_cannot_scroll_or_click_on_release_frame");
    }
}
}
void pointer_checks(){
    squeeze_checks();button_checks();hover_checks();preparation_checks();
    reset_checks();drag_checks();surface_checks();two_hand_checks();
}
}
