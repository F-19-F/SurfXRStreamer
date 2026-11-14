#include "surfxr/vr_ui.hpp"
#include "surfxr/layout_store.hpp"
#include "../../../frame/xr/desktop_input.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace surfxr { namespace {
void require(bool value,const char* label){if(!value)throw std::runtime_error(label);}
bool near(float a,float b){return std::abs(a-b)<.001f;}
UiPose aim_at(const UiPanel& panel,float u,float v){
    const float theta=(u-.5f)*panel.curvature_radians;
    const auto point=ui_panel_point(panel,u,v);const auto normal=ui_rotate(panel.pose.orientation,{-std::sin(theta),0,std::cos(theta)});
    return {{point.x+normal.x,point.y+normal.y,point.z+normal.z},ui_multiply(panel.pose.orientation,{0,std::sin(-theta/2),0,std::cos(theta/2)})};
}
struct Fixture {
    VrUiModel model;std::array<UiHand,2> hands{};std::vector<UiEvent> events;
    Fixture(){model.set_desktop_capture_status({true,15,15});UiPanel a;a.source_id=3;UiPanel b;b.source_id=8;model.set_panels({a,b});model.reset_anchor({});hands[0].active=true;hands[0].aim=aim_at(model.panels()[0],.5f,.5f);tick();events.clear();}
    void tick(float seconds=.02f){model.update(hands,seconds,[&](const auto& e){events.push_back(e);});}
    void key(const std::string& label){
        if(!model.keyboard_panel().visible){hands[0].menu=true;tick();hands[0].menu=false;tick();}
        const auto it=std::find_if(model.keys().begin(),model.keys().end(),[&](const auto& k){return k.label==label;});require(it!=model.keys().end(),"key_exists");
        hands[0].aim=aim_at(model.keyboard_panel(),it->x+it->width/2,it->y+it->height/2);hands[0].select=false;tick();hands[0].select=true;tick();hands[0].select=false;tick();
    }
    void menu_click(float u,float v){hands[0].aim=aim_at(model.keyboard_panel(),u,v);hands[0].select=false;tick();hands[0].select=true;tick();hands[0].select=false;tick();}
};
#ifdef __linux__
struct LayoutTestDirectory {
    std::string root;
    LayoutTestDirectory(){char name[]="/tmp/surfxr-ui-layout-XXXXXX";const auto* p=::mkdtemp(name);if(!p)throw std::runtime_error("layout_test_directory");root=p;}
    ~LayoutTestDirectory(){for(const auto* file:{"state/panels.layout","bad.layout","link.layout","pipe.layout"})::unlink((root+"/"+file).c_str());::rmdir((root+"/state").c_str());::rmdir(root.c_str());}
};
#endif
}
void keyboard_checks();
void pointer_checks();
int vr_ui_self_test(){
    keyboard_checks();
    pointer_checks();
    unsigned groups=0;UiPanel p;p.pose.position.z=-2;p.width=2;p.height=1;
    {Fixture f;f.hands={};f.tick();f.events.clear();
        const auto command=[&](const UiEvent& e){f.events.push_back(e);};
        f.model.toggle_keyboard(command);f.tick();
        require(f.model.keyboard_panel().visible,"button_opens_keyboard_without_hand_pose");
        f.model.toggle_keyboard(command);f.tick();
        require(!f.model.keyboard_panel().visible&&std::all_of(f.events.begin(),f.events.end(),
            [](const auto& e){return e.type==UiEventType::ReleaseAll;}),"pose_free_command_only_releases_inputs");
        f.model.show_settings(true,command);f.model.toggle_keyboard(command);f.tick();
        require(!f.model.settings_visible()&&f.model.keyboard_panel().visible,"button_closes_settings_without_pose");}++groups;
    const auto center=ui_hit_panel(p,{});require(center.valid&&near(center.u,.5f)&&near(center.v,.5f),"flat_center");
    require(!ui_hit_panel(p,{{3,0,0},{}}).valid&&!ui_hit_panel(p,{{0,0,-3},{}}).valid,"flat_miss_backface");++groups;
    p.pose.orientation={0,std::sin(.4f),0,std::cos(.4f)};require(ui_hit_panel(p,aim_at(p,.1f,.8f)).valid,"rotated_hit");
    const auto inverse=ui_compose(p.pose,ui_inverse(p.pose));require(near(inverse.position.z,0)&&near(inverse.orientation.w,1),"pose_inverse");++groups;
    p.curvature_radians=1.5f;
    for(float u:{.05f,.25f,.5f,.75f,.95f}){const auto hit=ui_hit_panel(p,aim_at(p,u,.7f));require(hit.valid&&near(hit.u,u)&&near(hit.v,.7f),"cylinder_uv_roundtrip");}++groups;
    p.curve_segments=4;for(float u:{.125f,.375f,.625f,.875f}){const auto hit=ui_hit_panel(p,aim_at(p,u,.6f));require(hit.valid&&near(hit.u,u)&&near(hit.v,.6f),"chord_uv_roundtrip");}++groups;
    {Fixture f;f.hands[0].select=true;f.tick();f.tick(.05f);require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;}),"short_press_not_drag");
        f.hands[0].select=false;f.tick();require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;})==2,"release_click_pair");}++groups;
    {Fixture f;f.hands[0].select=true;f.tick();for(int i=0;i<24;++i)f.tick(.05f);require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;}),"stationary_hold_not_drag");
        f.hands[0].aim=aim_at(f.model.panels()[0],.56f,.5f);for(int i=0;i<4;++i)f.tick();require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton&&e.pressed;}),"intentional_drag_threshold");
        f.events.clear();f.hands[0].active=false;f.tick();require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ReleaseAll;})==1&&std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton&&!e.pressed;})==1,"tracking_loss_releases_drag");
        f.events.clear();f.tick();require(f.events.empty(),"loss_is_edge_only");f.hands[0].active=true;f.tick();require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;}),"held_trigger_not_rearmed");}++groups;
    {Fixture f;f.hands[0].trigger=.45f;f.tick();f.hands[0].trigger=.35f;f.tick();f.hands[0].trigger=.21f;f.tick();require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;}),"trigger_hysteresis_hold");f.hands[0].trigger=.19f;f.tick();require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;})==2,"trigger_hysteresis_release");}++groups;
    {Fixture f;f.hands[0].trigger=.5f;f.tick(.007f);f.hands[0].aim.position.x=4;f.hands[0].trigger=0;f.tick(.007f);
        unsigned edges=0;for(const auto& e:f.events)if(e.type==UiEventType::PointerButton){++edges;require(e.source_id==3&&near(e.u,.5f)&&near(e.v,.5f),"short_click_keeps_press_target");}
        require(edges==2,"fast_release_outside_panel_still_clicks");}++groups;
    {for(unsigned hand=0;hand<2;++hand)for(auto button:{ButtonBumper,ButtonTrigger,hand?ButtonA:ButtonDpadDown,hand?ButtonB:ButtonDpadLeft}){
        Fixture f;f.hands[0].active=false;f.hands[hand].active=true;f.hands[hand].aim=aim_at(f.model.panels()[0],.5f,.5f);f.hands[hand].trigger=0;f.tick();f.events.clear();
        TrackingController controller;controller.profile=TrackingProfile::SteamFrame;controller.buttons_active=controller.buttons=button;
        apply_desktop_buttons(f.hands[hand],controller,hand);f.tick(.007f);f.hands[hand].select=f.hands[hand].right_click=false;f.tick(.007f);
        const int expected=button==(hand?ButtonB:ButtonDpadLeft)?1:0;
        require(std::count_if(f.events.begin(),f.events.end(),[&](const auto& e){return e.type==UiEventType::PointerButton&&e.button==expected;})==2,"frame_desktop_button_click_pair");
    }}++groups;
    {TrackingController c;c.profile=TrackingProfile::SteamFrame;c.buttons=ButtonBumper|ButtonDpadDown|ButtonDpadLeft;UiHand ui;
        apply_desktop_buttons(ui,c,0);require(!ui.select&&!ui.right_click,"inactive_buttons_ignored");
        c.buttons_active=c.buttons;c.profile=TrackingProfile::OculusTouch;apply_desktop_buttons(ui,c,0);require(!ui.select&&!ui.right_click,"other_profile_unchanged");
        c.profile=TrackingProfile::SteamFrame;c.buttons=c.buttons_active=ButtonDpadDown|ButtonDpadLeft;apply_desktop_buttons(ui,c,1);require(!ui.select&&!ui.right_click,"dpad_is_left_only");}++groups;
    {Fixture f;f.hands[0].trigger=.3f;f.hands[0].select=true;f.tick();f.hands[0].select=false;f.tick();
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton;})==2,"digital_click_does_not_latch_analog_trigger");}++groups;
    {Fixture f;f.hands[0].grip=true;f.tick();f.hands[0].select=true;f.hands[0].aim.position.x+=.1f;f.tick();const auto x=f.model.panels()[0].pose.position.x;
        f.hands[0].grip=false;f.hands[0].aim.position.x+=.1f;f.tick();require(near(f.model.panels()[0].pose.position.x,x),"grip_release_ends_grab_with_trigger_held");}++groups;
    {Fixture f;f.hands[0].squeeze=.6f;f.tick();f.hands[0].squeeze=.5f;f.hands[0].aim.position.x+=.1f;f.tick();
        require(near(f.model.panels()[0].pose.position.x,.1f),"grip_hysteresis_keeps_grab");f.hands[0].squeeze=.3f;f.hands[0].aim.position.x+=.1f;f.tick();
        require(near(f.model.panels()[0].pose.position.x,.1f),"grip_hysteresis_releases");}++groups;
    {for(bool rotate:{false,true}){Fixture f;f.hands[0].grip=true;f.tick();const auto pose=f.model.panels()[0].pose;
        if(rotate)f.hands[0].aim.orientation={0,.7071068f,0,.7071068f};else f.hands[0].aim.position.x+=2;
        f.tick();require(near(f.model.panels()[0].pose.position.x,pose.position.x)&&near(f.model.panels()[0].pose.orientation.w,pose.orientation.w),"tracking_jump_freezes_panel");
        f.hands[0].aim=aim_at(f.model.panels()[0],.5f,.5f);f.tick();f.hands[0].aim.position.x+=.1f;f.tick();require(near(f.model.panels()[0].pose.position.x,pose.position.x),"tracking_jump_requires_release");
        f.hands[0].grip=false;f.tick();f.hands[0].grip=true;f.tick();f.hands[0].aim.position.x+=.1f;f.tick();require(near(f.model.panels()[0].pose.position.x,pose.position.x+.1f),"grab_rearms_after_release");
    }}++groups;
    {for(float distance:{.1f,6.f}){Fixture f;f.hands[0].aim.position.z=f.model.panels()[0].pose.position.z+distance;f.hands[0].aim.orientation.w=1.004f;f.hands[0].grip=true;f.tick();
        const auto pose=f.model.panels()[0].pose;f.hands[0].stick_y=.1f;f.tick();
        require(near(f.model.panels()[0].pose.position.z,pose.position.z)&&near(f.model.panels()[0].pose.orientation.w,1),"grab_has_no_initial_depth_or_quaternion_jump");
        f.hands[0].aim.orientation.w=-1.004f;f.tick();require(f.model.drag_updates()==2,"quaternion_sign_flip_is_not_tracking_jump");
    }}++groups;
    {Fixture f;f.hands[0].grip=true;f.tick();const auto pose=f.model.panels()[0].pose;
        f.hands[0].aim.position.x=std::numeric_limits<float>::quiet_NaN();f.tick();f.hands[0].aim=aim_at(f.model.panels()[0],.5f,.5f);f.tick();f.hands[0].aim.position.x+=.1f;f.tick();
        require(near(f.model.panels()[0].pose.position.x,pose.position.x),"invalid_pose_cancels_grab_without_replay");}++groups;
    {Fixture f;f.hands[0].grip=true;f.tick();f.hands[1].active=true;f.hands[1].aim=aim_at(f.model.panel_controls(0).top,.1f,.5f);f.tick();
        const auto width=f.model.panels()[0].width;f.hands[1].select=true;f.tick();f.hands[1].aim.position.x+=.3f;f.tick();
        require(near(f.model.panels()[0].width,width),"other_hand_cannot_resize_grabbed_panel");}++groups;
    {Fixture f;f.hands[0].right_click=true;f.tick();f.tick();require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton&&e.button==1;})==2,"right_click_edge_pair");}++groups;
    {Fixture f;f.hands[1].active=true;f.hands[1].aim=aim_at(f.model.panels()[1],.5f,.5f);f.tick();f.hands[0].select=true;f.tick();f.events.clear();f.hands[1].stick_y=1;f.tick();require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.source_id==8&&(e.type==UiEventType::PointerMove||e.type==UiEventType::Scroll);}),"other_hand_cannot_steal_capture");
        f.hands[0].aim=f.hands[1].aim;f.tick();require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.source_id==8&&e.type==UiEventType::PointerButton;}),"capture_does_not_cross_sources");}++groups;
    {Fixture f;f.hands[0].stick_y=.21f;f.tick(.005f);require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Scroll&&e.scroll_y>0&&e.scroll_y<1;}),"sub_tick_scroll_preserved");}++groups;
    {Fixture f;require(!f.model.panel_controls(0).visible,"controls_start_hidden");auto bottom=f.model.panel_controls(0).bottom;bottom.visible=true;f.hands[0].aim=aim_at(bottom,.5f,.3f);f.tick();require(f.model.panel_controls(0).visible&&f.model.hits()[0].region==UiHitRegion::BottomControls,"bottom_gutter_reveals_controls");
        f.hands[0].aim=aim_at(f.model.panel_controls(0).top,.7f,.5f);f.tick();f.hands[0].select=true;f.tick();f.hands[0].aim.position.x+=.2f;f.tick();require(f.model.panels()[0].curvature_radians>.1f,"curve_handle_changes_curvature");
        f.hands[0].select=false;f.tick();f.hands[0].aim=aim_at(f.model.panels()[0],.5f,.5f);for(int i=0;i<24;++i)f.tick(.05f);require(!f.model.panel_controls(0).visible,"controls_expire_outside_gutter");}++groups;
    {Fixture f;auto bottom=f.model.panel_controls(0).bottom;bottom.visible=true;f.hands[0].aim=aim_at(bottom,.5f,.87f);f.tick();f.hands[0].select=true;f.tick();const auto x=f.model.panels()[0].pose.position.x;f.hands[0].aim.position.x+=.3f;f.tick();require(f.model.panels()[0].pose.position.x>x+.25f,"external_move_region");}++groups;
    {Fixture f;auto bottom=f.model.panel_controls(0).bottom;bottom.visible=true;f.hands[0].aim=aim_at(bottom,.5f,.3f);f.tick();
        const auto top=f.model.panel_controls(0).top;f.hands[0].aim=aim_at(top,.1f,.5f);f.tick();f.hands[0].select=true;f.tick();
        f.hands[0].aim=aim_at(top,.95f,.5f);f.tick();require(near(f.model.panels()[0].width,kUiMaxPanelWidthM),"size_slider_reaches_eight_meter_cap");}++groups;
    {Fixture f;f.hands[0].grip=true;f.tick();f.hands[0].stick_x=1;for(int i=0;i<40;++i)f.tick(.05f);
        require(near(f.model.panels()[0].width,kUiMaxPanelWidthM),"grip_scale_eight_meter_cap");
        f.hands[0].stick_x=-1;for(int i=0;i<80;++i)f.tick(.05f);require(near(f.model.panels()[0].width,kUiMinPanelWidthM),"grip_scale_lower_cap");}++groups;
    {Fixture f;f.hands[0].select=true;f.tick();f.events.clear();f.model.set_available(0,false,[&](const auto& e){f.events.push_back(e);});require(!ui_hit_panel(f.model.panels()[0],f.hands[0].aim).valid&&std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ReleaseAll;}),"unavailable_cancels_pending_click");}++groups;
    {Fixture f;std::set<UiKey> keys;for(const auto& k:f.model.keys())if(k.command==0&&k.key!=UiKey::None)require(keys.insert(k.key).second,"duplicate_physical_key");require(keys.size()==87,"exact_87_physical_keys");
        for(auto key:{UiKey::F12,UiKey::Pause,UiKey::CapsLock,UiKey::RightWindows,UiKey::Menu,UiKey::Insert,UiKey::Delete,UiKey::PageDown})require(keys.count(key)==1,"tkl_keyboard_key_group");
        for(auto key:{UiKey::NumLock,UiKey::Numpad0,UiKey::NumpadEnter,UiKey::VolumeMute,UiKey::Calculator})require(keys.count(key)==0,"tkl_omits_numpad_and_media");}++groups;
    {Fixture f;f.key("LCTRL");f.key("C");require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key&&e.key==UiKey::LeftControl&&e.pressed;}),"modifier_down");require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key&&e.key==UiKey::C&&e.pressed;}),"modifier_letter_code");require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Text;}),"physical_keys_do_not_reset_modifiers_via_text");f.key("LCTRL");require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key&&e.key==UiKey::LeftControl&&!e.pressed;}),"modifier_up");
        f.key("LSHIFT");require(f.model.shift(),"sticky_shift_visible");f.model.release_inputs({});require(!f.model.shift(),"release_clears_sticky_shift");}++groups;
    {Fixture f;f.key("LWIN");f.events.clear();f.key("DASHBOARD");
        require(!f.model.exit_requested()&&std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ShowDashboard;}),"dashboard_keeps_session");
        require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ReleaseAll;}),"dashboard_releases_keys");
        require(std::none_of(f.model.keys().begin(),f.model.keys().end(),[](const auto& k){return k.label=="IME"||k.label=="EXIT";}),"no_ime_or_exit_topbar");
        f.key("CAPTURE 1");require(!f.model.panels()[0].visible,"hide_panel");f.key("CAPTURE 1");require(f.model.panels()[0].visible,"show_panel");
        f.model.set_desktop_capture_status({false,15,15});f.key("CAPTURE 1");require(f.model.panels()[0].visible,"unsupported_capture_not_local_only");
        f.events.clear();f.key("TAKE OVER");require(f.events.empty(),"takeover_unavailable");
        f.model.set_system_capture_status(true,false);f.key("TAKE OVER");require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::SystemCaptureChanged&&e.pressed;}),"topbar_takeover_on");
        f.model.set_system_capture_status(true,true);f.events.clear();f.key("TAKE OVER");require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::SystemCaptureChanged&&!e.pressed;}),"topbar_takeover_off");}++groups;
    {Fixture f;f.key("Ctrl + Space");
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key&&e.key==UiKey::Shortcut&&e.shortcut==kDefaultInputShortcuts[0];})==1,"shortcut_default_once");
        f.events.clear();f.key("Ctrl + V");require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key&&e.shortcut==kDefaultInputShortcuts[1];}),"paste_default");
        require(std::none_of(f.model.keys().begin(),f.model.keys().end(),[](const auto& k){return k.label=="RESET"||k.label=="INPUT METHOD"||k.label=="IME MODE";}),"no_named_actions_or_reset_in_bar");
        for(const auto& k:f.model.keys())if(k.key==UiKey::Shortcut||k.command>=6&&k.command<=8)require(k.y>.9f,"shortcuts_and_attachment_in_footer");
        const auto count=f.model.input_preferences().input_shortcuts.size();
        f.key("+");require(f.model.shortcut_recording(),"add_entry");f.events.clear();f.key("SAVE");require(f.model.shortcut_recording(),"empty_save_rejected");
        f.key("LCTRL");f.key("LSHIFT");f.key("A");f.key("A");f.key("SPACE");
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key;}),"recording_not_forwarded");
        f.key("SAVE");const KeyboardShortcut expected{0x1d,0x2a,0x39};
        require(f.model.input_preferences().input_shortcuts.size()==count+1&&f.model.input_preferences().input_shortcuts.back()==expected,"add_saved");
        require(std::any_of(f.events.begin(),f.events.end(),[&](const auto& e){return e.type==UiEventType::ShortcutChanged&&e.button==count&&e.pressed&&e.shortcut==expected;}),"add_event");
        f.events.clear();f.key("Ctrl + Shift + Space");require(std::any_of(f.events.begin(),f.events.end(),[&](const auto& e){return e.type==UiEventType::Key&&e.shortcut==expected;}),"added_chord_replays");
        f.key("EDIT");f.events.clear();f.key("Ctrl + Shift + Space");require(f.model.shortcut_recording()&&f.events.size()==1&&f.events[0].type==UiEventType::ReleaseAll,"edit_does_not_replay");
        f.key("CANCEL");require(f.model.input_preferences().input_shortcuts.back()==expected,"cancel_preserves_chord");
        f.key("EDIT");f.key("Ctrl + Shift + Space");f.key("DELETE");require(f.model.input_preferences().input_shortcuts.size()==count,"delete_shortcut");
        for(unsigned i=0;i<8;++i)f.key("←");require(std::any_of(f.model.keys().begin(),f.model.keys().end(),[](const auto& k){return k.label=="Ctrl + Space";}),"scroll_start");
        f.hands[0].aim=aim_at(f.model.keyboard_panel(),.55f,.95f);f.hands[0].stick_x=1;f.tick();f.hands[0].stick_x=0;f.tick();
        require(std::none_of(f.model.keys().begin(),f.model.keys().end(),[](const auto& k){return k.label=="Ctrl + Space";}),"stick_scrolls_bar");
        auto full=f.model.input_preferences();full.input_shortcuts.assign(kMaxKeyboardShortcuts,KeyboardShortcut{0x1d,0x39});f.model.set_input_preferences(full);f.key("+");require(!f.model.shortcut_recording(),"bar_limit_blocks_add");
        f.model.set_input_preferences(InputPreferences{});
        f.key("+");f.hands[0].active=false;f.tick();require(!f.model.shortcut_recording(),"focus_loss_cancels_recorder");
    }++groups;
    {Fixture f;
        auto custom=f.model.export_layout();custom.panels[0].width_m=3;custom.panels[0].curvature_radians=.9f;custom.panels[0].anchor_pose.position={.4f,.3f,-3.f};
        custom.panels[1].width_m=2.7f;custom.panels[1].curvature_radians=.6f;custom.panels[1].anchor_pose.position={-2.4f,.5f,-3.2f};
        custom.keyboard_width_m=1.2f;custom.keyboard_space_width_m=1.2f;custom.keyboard_pose.position={.1f,-.5f,-1.9f};
        f.hands[0].active=false;f.tick();require(f.model.restore_layout(custom),"custom_reset_fixture");f.hands[0].active=true;
        const auto before=f.model.export_layout();
        const auto reset_display=[&](unsigned id){auto strip=f.model.panel_controls(id).bottom;strip.visible=true;
            f.hands[0].aim=aim_at(strip,.12f,.31f);f.hands[0].select=false;f.tick();f.hands[0].select=true;f.tick();f.hands[0].select=false;f.tick();};
        const auto inline_click=[&](unsigned id,float u){const auto strip=f.model.panel_controls(id).bottom;
            f.hands[0].aim=aim_at(strip,u,.46f);f.hands[0].select=false;f.tick();f.hands[0].select=true;f.tick();f.hands[0].select=false;f.tick();};
        reset_display(0);require(!f.model.settings_visible()&&!f.model.keyboard_panel().visible&&near(f.model.panels()[0].width,3),"display_reset_stays_beside_control");
        f.hands[0].aim={{10,10,10},{}};for(unsigned i=0;i<30;++i)f.tick(.05f);
        require(f.model.panel_controls(0).visible&&f.model.input_capture_mask()==std::array<bool,2>{true,true},"inline_confirmation_persists_without_hover");
        f.events.clear();f.menu_click(.915f,.075f);require(!f.model.settings_visible()&&near(f.model.panels()[0].width,3)&&f.events.empty(),"inline_blocks_other_surfaces");
        inline_click(0,.13f);require(!f.model.settings_visible()&&near(f.model.panels()[0].width,3),"display_cancel_preserves_layout");
        reset_display(0);inline_click(0,.37f);const auto first=f.model.export_layout();
        require(near(first.panels[0].width_m,1.8f)&&near(first.panels[0].curvature_radians,0)&&near(first.panels[0].anchor_pose.position.z,-2.f),"display_confirm_resets_geometry");
        require(near(first.panels[1].width_m,before.panels[1].width_m)&&near(first.panels[1].curvature_radians,before.panels[1].curvature_radians)&&near(first.panels[1].anchor_pose.position.x,before.panels[1].anchor_pose.position.x)&&near(first.keyboard_width_m,before.keyboard_width_m),"single_reset_leaves_other_display_and_keyboard");
        reset_display(1);f.hands[0].active=false;f.tick();require(!f.model.settings_visible()&&near(f.model.panels()[1].width,2.7f),"tracking_loss_cancels_reset");f.hands[0].active=true;f.tick();
        f.model.show_settings(true);f.tick();f.menu_click(.86f,.95f);require(near(f.model.panels()[1].width,2.7f),"full_reset_waits_for_confirmation");
        f.menu_click(.63f,.71f);const auto all=f.model.export_layout();require(f.model.settings_visible(),"full_reset_returns_settings");
        require(near(all.panels[1].width_m,1.8f)&&near(all.panels[1].curvature_radians,0)&&near(all.keyboard_width_m,2.05f)&&all.keyboard_binding==0&&f.model.input_preferences().input_shortcuts==kDefaultInputShortcuts,"full_reset_layout_only");
    }++groups;
    {Fixture f;const bool keyboard_visible=f.model.keyboard_panel().visible;f.hands[0].menu=true;for(int i=0;i<31;++i)f.tick(.05f);require(!f.model.exit_requested(),"hold_menu_does_not_exit");f.hands[0].menu=false;f.tick();require(!f.model.exit_requested()&&f.model.keyboard_panel().visible==keyboard_visible,"held_menu_release_has_no_action");}++groups;
    {Fixture f;constexpr unsigned w=768,h=128,stride=w*4+32;std::vector<std::uint8_t> pixels(stride*h+32,0x5a);f.model.raster_controls(0,false,pixels.data()+16,w,h,stride);
        require(std::all_of(pixels.begin(),pixels.begin()+16,[](auto b){return b==0x5a;})&&std::all_of(pixels.end()-16,pixels.end(),[](auto b){return b==0x5a;}),"control_raster_guards");for(unsigned row=0;row<h;++row)require(std::all_of(pixels.begin()+16+row*stride+w*4,pixels.begin()+16+(row+1)*stride,[](auto b){return b==0x5a;}),"control_raster_padded_stride");}++groups;
    {RemoteSettings state;require(!state.request({SettingId::DesktopFramerate,60}),"disconnected_settings_disabled");
        RemoteSettingsCapabilities caps;caps.connected=true;caps.has_vr=true;caps.vr_ready=true;caps.max_vr_quality=1;caps.vr_framerates={90,999,90,72};state.set_capabilities(caps);
        require(state.snapshot().capabilities.vr_framerates==std::vector<int>({72,90}),"runtime_rates_filtered");
        require(!state.request({SettingId::VrFramerate,120})&&!state.request({SettingId::VrGraphicsQuality,2})&&!state.request({SettingId::MicrophoneEnabled,1}),"unsupported_settings_rejected");
        for(auto change:{SettingChange{SettingId::Count,1},SettingChange{SettingId::VrGraphicsQuality,.5},SettingChange{SettingId::DesktopBitrateMbps,121},SettingChange{SettingId::VrBitrateMbps,9},SettingChange{SettingId::MicrophoneVolume,std::numeric_limits<double>::quiet_NaN()}})require(!state.request(change)&&!state.observe(change),"invalid_remote_setting_rejected");
        require(state.request({SettingId::VrFramerate,90}),"supported_setting_requested");auto snapshot=state.snapshot();require(snapshot.at(SettingId::VrFramerate).pending&&!snapshot.at(SettingId::VrFramerate).actual,"request_is_not_acknowledgement");
        require(state.observe({SettingId::VrFramerate,72})&&state.snapshot().at(SettingId::VrFramerate).pending,"different_observation_still_pending");
        require(state.observe({SettingId::VrFramerate,90})&&!state.snapshot().at(SettingId::VrFramerate).pending,"matching_observation_confirms");
        require(state.request({SettingId::VrBitrateMbps,100})&&state.observe({SettingId::VrBitrateMbps,100})&&!state.snapshot().at(SettingId::VrBitrateMbps).pending,"absolute_bitrate_ack");
        caps.connected=false;state.set_capabilities(caps);require(!state.request({SettingId::DesktopFramerate,60})&&!state.snapshot().at(SettingId::VrFramerate).supported,"disconnect_disables_settings");}++groups;
    {RemoteSettings state;require(state.snapshot().at(SettingId::VrGraphicsQuality).requested==4,"default_quality_godlike");
        const auto& profiles=surfxr_vr_quality_profiles();require(profiles.size()==9&&profiles.front().quality==-1&&profiles.back().quality==7,"all_q3_quality_profiles");
        for(const auto& p:profiles){require(surfxr_vr_quality_profile(p.quality)==&p&&p.encoded_width*3==p.render_width*2&&p.encoded_height*3==p.render_height*2,"q3_profile_lookup_and_nominal_ratio");}
        require(!surfxr_vr_quality_profile(-2)&&!surfxr_vr_quality_profile(8),"unknown_quality_has_no_dimensions");
        require(surfxr_max_vr_quality(8192,8192,16ULL*1024*1024,8192,8192)==7,"full_size_limits_allow_all_qualities");
        require(surfxr_max_vr_quality(8192,8192,16ULL*1024*1024,4096,4096)==5,"xr_dimensions_limit_quality");
        require(surfxr_max_vr_quality(8192,8192,16ULL*1024*1024,4096,4096,1.5,1.5)==2,"observed_supersampling_limits_quality");
        require(surfxr_max_vr_quality(8192,8192,16ULL*1024*1024,8192,8192,1.5,1.5)==7,"all_profiles_fit_8192_at_observed_supersampling");
        require(surfxr_max_vr_quality(8192,8192,1792ULL*1920,8192,8192)==3,"encoded_area_limit");
        require(surfxr_max_vr_quality(896,960,896ULL*960,1344,1440)==-1&&surfxr_max_vr_quality(895,960,896ULL*960,1344,1440)==-2,"smallest_profile_bounds");
        require(surfxr_max_vr_quality(8192,8192,16ULL*1024*1024,8192,8192,0,1)==-2,"invalid_supersampling_rejected");
        RemoteSettingsCapabilities caps;caps.connected=caps.can_launch_vr=caps.vr_ready=true;caps.max_vr_quality=7;state.set_capabilities(caps);
        require(setting_choices(SettingId::VrGraphicsQuality,state.snapshot())==std::vector<double>({-1,0,1,2,3,4,5,6,7}),"higher_quality_choices_available");
        require(state.request({SettingId::VrGraphicsQuality,7}),"highest_size_supported_quality_allowed");
        caps.max_vr_quality=-2;state.set_capabilities(caps);require(setting_choices(SettingId::VrGraphicsQuality,state.snapshot()).empty()&&!state.request({SettingId::VrGraphicsQuality,-1}),"no_fitting_profile_disables_quality");
        RemoteSettings limited;caps.max_vr_quality=0;limited.set_capabilities(caps);require(limited.snapshot().at(SettingId::VrGraphicsQuality).requested==0,"default_clamps_to_real_cap");}++groups;
    {RemoteSettings state;state.seed_requested({SettingId::VrGraphicsQuality,2});
        require(state.observe({SettingId::VrGraphicsQuality,1}),"initial_quality_observed");
        require(state.snapshot().at(SettingId::VrGraphicsQuality).requested==2&&state.snapshot().at(SettingId::VrGraphicsQuality).actual==1&&!state.snapshot().at(SettingId::VrGraphicsQuality).pending,"explicit_seed_preserved_but_not_sent");
        RemoteSettingsCapabilities caps;caps.connected=caps.can_launch_vr=caps.vr_ready=true;state.set_capabilities(caps);
        require(state.request({SettingId::VrGraphicsQuality,2})&&state.snapshot().at(SettingId::VrGraphicsQuality).pending,"queued_quality_waits_for_observation");
        require(state.observe_vr_dimensions(1408,1536,3168,3456)&&state.snapshot().at(SettingId::VrGraphicsQuality).actual==1&&state.snapshot().at(SettingId::VrGraphicsQuality).pending,"medium_encoded_with_supersampling_not_high_confirmation");
        require(state.observe_vr_dimensions(1664,1792,3744,4032)&&state.snapshot().at(SettingId::VrGraphicsQuality).actual==1&&state.snapshot().at(SettingId::VrGraphicsQuality).pending,"dimensions_do_not_ack_pc_quality");
        const auto dimensions=state.snapshot().vr_dimensions;require(dimensions&&dimensions->encoded_width==1664&&dimensions->render_height==4032,"actual_dimensions_preserved");
        const auto revision=state.snapshot().revision;require(!state.observe_vr_dimensions(0,1792,3744,4032)&&!state.observe_vr_dimensions(1664,1792,16385,4032)&&state.snapshot().revision==revision,"invalid_dimensions_atomic");
        require(state.observe_vr_dimensions(1700,1800,3744,4032)&&state.snapshot().at(SettingId::VrGraphicsQuality).actual==1,"unknown_encoded_dimensions_do_not_guess_quality");}++groups;
    
    
    
    
    {Fixture f;f.key("LCTRL");f.events.clear();f.key("SETTINGS");require(f.model.settings_visible()&&f.model.keyboard_panel().visible,"keyboard_settings_entry");
        require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ReleaseAll;}),"settings_releases_latched_input");
        require(f.model.input_capture_mask()==std::array<bool,2>{true,true},"settings_captures_game_input");
        f.events.clear();f.hands[0].aim=aim_at(f.model.panels()[0],.5f,.5f);f.hands[0].select=true;f.tick();f.tick(.05f);f.hands[0].select=false;f.tick();
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::PointerButton||e.type==UiEventType::PointerMove;}),"modal_settings_never_click_desktop");
        f.menu_click(.86f,.95f);require(f.model.settings_visible(),"complete_reset_confirmation_open");f.menu_click(.35f,.71f);require(f.model.settings_visible(),"reset_cancel_returns_settings");f.menu_click(.915f,.075f);require(!f.model.settings_visible()&&f.model.keyboard_panel().visible,"settings_close_returns_keyboard");}++groups;
    {Fixture f;auto bottom=f.model.panel_controls(0).bottom;bottom.visible=true;f.hands[0].aim=aim_at(bottom,.88f,.31f);f.tick();f.hands[0].select=true;f.tick();f.hands[0].select=false;f.tick();
        require(f.model.settings_visible()&&!f.model.export_layout().keyboard_visible,"fourth_circle_settings_not_persisted");f.menu_click(.915f,.075f);require(!f.model.keyboard_panel().visible,"external_settings_close_restores_hidden_keyboard");
        f.hands[0].menu=true;f.tick();f.hands[0].menu=false;f.tick();require(f.model.keyboard_panel().visible&&!f.model.settings_visible(),"single_menu_still_opens_keyboard");}++groups;
    {Fixture f;RemoteSettings state;RemoteSettingsCapabilities caps;caps.connected=true;caps.has_vr=false;caps.vr_ready=true;caps.can_launch_vr=true;caps.stage_active=true;caps.vr_framerates={72,90};state.set_capabilities(caps);f.model.set_remote_settings(state.snapshot());f.model.show_settings(true);f.tick();
        require(setting_request_allowed({SettingId::VrFramerate,72},state.snapshot()),"cold_start_can_configure_vr");
        const auto& rows=VrUiModel::settings_rows();const auto row=*std::find_if(rows.begin(),rows.end(),[](const auto& item){return item.id==SettingId::DesktopFramerate;});
        f.events.clear();f.menu_click(row.x+row.width*.93f,row.y+row.height/2);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoteSettingChanged&&e.setting.id==SettingId::DesktopFramerate&&e.setting.value==72;})==1,"typed_setting_event_edge");
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Key||e.type==UiEventType::Text||e.type==UiEventType::PointerButton;}),"settings_never_emit_pc_keys");
        f.model.set_remote_settings(state.snapshot());f.menu_click(row.x+row.width*.93f,row.y+row.height/2);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoteSettingChanged&&e.setting.value==72;})==2,"rejected_enqueue_does_not_optimistically_advance_requested_value");
        f.events.clear();f.menu_click(.13f,.865f);f.menu_click(.375f,.865f);f.menu_click(.62f,.865f);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::LaunchVr;})==1&&std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ShowDesktop;})==1&&std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ExitVr;}),"vr_actions_obey_capabilities");
        caps.session_mode_switch=true;caps.surfxr_video_controls=true;state.set_capabilities(caps);f.model.set_remote_settings(state.snapshot());
        f.events.clear();f.menu_click(.625f,.865f);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ExitVr;})==1,"surfxr_exit_steamvr_button_enabled");
        constexpr unsigned w=2048,h=768,stride=w*4+32;std::vector<std::uint8_t> pixels(stride*h+32,0x5a);f.model.raster_keyboard(pixels.data()+16,w,h,stride);
        require(std::all_of(pixels.begin(),pixels.begin()+16,[](auto b){return b==0x5a;})&&std::all_of(pixels.end()-16,pixels.end(),[](auto b){return b==0x5a;}),"settings_raster_guards");
        for(unsigned y=0;y<h;++y)require(std::all_of(pixels.begin()+16+y*stride+w*4,pixels.begin()+16+(y+1)*stride,[](auto b){return b==0x5a;}),"settings_raster_stride");}++groups;
    {Fixture f;RemoteSettings state;RemoteSettingsCapabilities caps;caps.connected=true;caps.can_launch_vr=true;caps.vr_requested=true;caps.vr_ready=false;caps.vr_status=VrMenuStatus::Starting;state.set_capabilities(caps);f.model.set_remote_settings(state.snapshot());f.model.show_settings(true);f.tick();
        f.events.clear();f.menu_click(.13f,.865f);f.menu_click(.375f,.865f);
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::LaunchVr;})&&std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ShowDesktop;})==1,"startup_disables_repeat_launch_but_allows_cancel_without_stage");
        constexpr unsigned w=512,h=192;std::vector<std::uint8_t> starting(w*h*4),failed(starting.size());f.model.raster_keyboard(starting.data(),w,h,w*4);
        caps.vr_status=VrMenuStatus::StartTimedOut;caps.vr_requested=false;state.set_capabilities(caps);f.model.set_remote_settings(state.snapshot());f.model.raster_keyboard(failed.data(),w,h,w*4);require(starting!=failed,"vr_status_changes_raster");
        for(auto status:{VrMenuStatus::Idle,VrMenuStatus::Starting,VrMenuStatus::StageNotReady,VrMenuStatus::NoSupportedRefreshRate,VrMenuStatus::SteamVrUnavailable,VrMenuStatus::ControlQueueFull,VrMenuStatus::StartTimedOut})require(std::strlen(vr_menu_status_text(status))<40,"bounded_fixed_status_text");
        require(std::strcmp(vr_menu_status_text(static_cast<VrMenuStatus>(255)),"VR: UNKNOWN STATUS")==0,"unknown_status_does_not_expose_text");}++groups;
    {Fixture f;f.model.show_settings(true);f.tick();f.events.clear();
        RemoteSettings remote;RemoteSettingsCapabilities caps;caps.connected=true;caps.can_add_monitor=true;caps.desktop_monitor_count=1;
        remote.set_capabilities(caps);f.model.set_remote_settings(remote.snapshot());
        // Show Desktop is available before any VR/Stage/video evidence exists.
        f.menu_click(.375f,.865f);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ShowDesktop;})==1,"desktop_action_without_video_or_stage");
        f.menu_click(.98f,.75f);f.menu_click(.98f,.75f);
        const float y=.56f;
        f.events.clear();f.menu_click(.43f,y);f.menu_click(.385f,y);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::AddMonitor;})==1&&std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoveMonitor;}),"monitor_buttons_obey_server_capabilities");
        caps.monitor_pending=true;remote.set_capabilities(caps);f.model.set_remote_settings(remote.snapshot());f.events.clear();f.menu_click(.43f,y);
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::AddMonitor;}),"pending_monitor_disables_repeat");
        caps.monitor_pending=false;caps.can_remove_monitor=true;caps.desktop_monitor_count=2;
        remote.set_capabilities(caps);f.model.set_remote_settings(remote.snapshot());f.menu_click(.385f,y);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoveMonitor;})==1,"monitor_remove_semantic_event");
        caps.vr_requested=true;remote.set_capabilities(caps);f.model.set_remote_settings(remote.snapshot());f.events.clear();f.menu_click(.43f,y);f.menu_click(.385f,y);
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::AddMonitor||e.type==UiEventType::RemoveMonitor;}),"vr_does_not_change_desktop_monitor_count");
    }++groups;
    {Fixture f;f.model.show_settings(true);f.tick();f.events.clear();
        RemoteSettings state;RemoteSettingsCapabilities caps;caps.connected=true;caps.eye_gaze_available=true;
        state.set_capabilities(caps);f.model.set_remote_settings(state.snapshot());
        const auto& rows=VrUiModel::settings_rows();const auto gaze=*std::find_if(rows.begin(),rows.end(),[](const auto& item){return item.id==SettingId::FoveatedStreaming;});
        f.menu_click(gaze.x+gaze.width*.80f,gaze.y+gaze.height/2);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoteSettingChanged&&e.setting.id==SettingId::FoveatedStreaming&&e.setting.value==0;})==1&&
                std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged;}),"gaze_row_is_visible_and_does_not_toggle_controllers");
        f.menu_click(.50f,.075f);f.events.clear();
        require(f.model.settings_page()==UiSettingsPage::Devices&&near(f.model.settings_scroll_offset(),0),"devices_have_no_hidden_scroll");
        f.menu_click(.025f+.445f*.80f,.26f);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged&&e.input_preference.id==InputPreferenceId::VrControllersEnabled&&e.input_preference.value==0;})==1,"controller_off_event");
        require(f.model.input_preferences().vr_controllers_enabled,"controller_change_waits_for_snapshot");
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoteSettingChanged||e.type==UiEventType::PointerButton||e.type==UiEventType::Key;}),"controller_preference_not_pc_input");
        f.model.set_input_preferences({false});f.events.clear();f.menu_click(.025f+.445f*.93f,.26f);
        require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged&&e.input_preference.id==InputPreferenceId::VrControllersEnabled&&e.input_preference.value==1;}),"local_controller_can_reenable_forwarding");
        f.events.clear();f.menu_click(.515f+.445f*.80f,.26f);
        require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged&&e.input_preference.id==InputPreferenceId::BoundarySyncEnabled&&e.input_preference.value==0;}),"boundary_belongs_to_devices");
        f.menu_click(.63f,.075f);f.events.clear();f.menu_click(.515f+.445f*.93f,.26f);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged&&e.input_preference.id==InputPreferenceId::GazeDebugOverlay&&e.input_preference.value==1;})==1,"gaze_debug_belongs_to_diagnostics");
        f.events.clear();f.menu_click(.025f+.445f*.93f,.26f);
        require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged&&e.input_preference.id==InputPreferenceId::PerformanceOverlay&&e.input_preference.value==1;}),"hud_belongs_to_diagnostics");
        f.events.clear();f.menu_click(.87f,.41f);
        require(f.events.size()==1&&f.events[0].input_preference.id==InputPreferenceId::TransitionHudEnabled&&
                f.events[0].input_preference.value==0,"transition_hud_can_be_disabled_in_diagnostics");
        f.menu_click(.38f,.075f);require(f.model.image_quality_page(),"local_menu_works_when_pc_forwarding_off");
        f.menu_click(.25f,.075f);f.menu_click(.915f,.075f);require(!f.model.settings_visible(),"close_menu_works_when_pc_forwarding_off");}++groups;
    {Fixture f;f.model.show_settings(true);f.tick();
        f.menu_click(.63f,.075f);f.events.clear();f.menu_click(.44f,.41f);
        require(f.events.size()==1&&f.events[0].type==UiEventType::InputPreferenceChanged&&f.events[0].input_preference.id==InputPreferenceId::MiniHudEnabled&&f.events[0].input_preference.value==1,"mini_hud_enable_event");
        require(!f.model.input_preferences().mini_hud_enabled,"mini_hud_waits_for_accepted_snapshot");
        f.events.clear();f.menu_click(.38f,.72f);
        require(f.events.empty()&&f.model.diagnostics_page()==UiDiagnosticsPage::Content,"mini_hud_content_navigation_is_local");
        const std::array<InputPreferenceId,4> metrics{InputPreferenceId::MiniHudFps,InputPreferenceId::MiniHudLatency,InputPreferenceId::MiniHudFrameTime,InputPreferenceId::MiniHudDecode};
        for(unsigned i=0;i<metrics.size();++i){
            const float x=i%2?.515f:.025f,y=i<2?.26f:.41f;const bool on=i>=2;
            f.events.clear();f.menu_click(x+.445f*(on?.93f:.80f),y);
            require(f.events.size()==1&&f.events[0].input_preference.id==metrics[i]&&f.events[0].input_preference.value==int(on),"mini_hud_each_metric_is_selectable");
        }
        f.events.clear();f.menu_click(.625f,.72f);require(f.model.diagnostics_page()==UiDiagnosticsPage::Layout&&f.events.empty(),"mini_hud_layout_navigation");
        f.menu_click(.44f,.26f);require(f.events.size()==1&&f.events[0].input_preference.id==InputPreferenceId::MiniHudX&&f.events[0].input_preference.value==85,"mini_hud_numeric_x_not_boolean");
        f.events.clear();f.menu_click(.87f,.26f);require(f.events.size()==1&&f.events[0].input_preference.id==InputPreferenceId::MiniHudY&&f.events[0].input_preference.value==15,"mini_hud_numeric_y");
        f.events.clear();f.menu_click(.44f,.41f);require(f.events.size()==1&&f.events[0].input_preference.id==InputPreferenceId::MiniHudScale&&f.events[0].input_preference.value==105,"mini_hud_size_percent");
        auto prefs=f.model.input_preferences();prefs.mini_hud_x=100;prefs.mini_hud_y=0;prefs.mini_hud_scale=50;f.model.set_input_preferences(prefs);f.events.clear();
        f.menu_click(.44f,.26f);f.menu_click(.87f,.26f);f.menu_click(.38f,.41f);require(f.events.empty(),"mini_hud_numeric_bounds");
        f.menu_click(.87f,.72f);require(f.model.diagnostics_page()==UiDiagnosticsPage::Gaze,"mini_hud_gaze_navigation");
        f.events.clear();f.menu_click(.44f,.26f);require(f.events.size()==1&&f.events[0].input_preference.id==InputPreferenceId::MiniHudGazeOnly&&f.events[0].input_preference.value==1,"mini_hud_gaze_only_control");
        f.events.clear();f.menu_click(.87f,.26f);require(f.events.size()==1&&f.events[0].input_preference.id==InputPreferenceId::MiniHudGazeMarker&&f.events[0].input_preference.value==0,"mini_hud_gaze_marker_control");
        constexpr unsigned w=1024,h=384,stride=w*4;std::vector<std::uint8_t> pixels(stride*h+32,0x5a);
        for(unsigned page=0;page<4;++page){
            f.menu_click(.13f+.245f*page,.72f);f.model.raster_keyboard(pixels.data()+16,w,h,stride);
            require(near(f.model.settings_scroll_offset(),0),"mini_hud_pages_fit_fixed_body");
            require(std::all_of(pixels.begin(),pixels.begin()+16,[](auto b){return b==0x5a;})&&std::all_of(pixels.end()-16,pixels.end(),[](auto b){return b==0x5a;}),"mini_hud_raster_guards");
        }
        f.menu_click(.38f,.72f);f.menu_click(.50f,.075f);f.menu_click(.63f,.075f);
        require(f.model.diagnostics_page()==UiDiagnosticsPage::Content,"mini_hud_subpage_retained_across_categories");
    }++groups;
    {Fixture f;f.model.show_settings(true);f.tick();
        f.menu_click(.50f,.075f);f.events.clear();
        const float y=.41f;
        f.menu_click(.035f+.445f*.93f,y);
        require(f.events.empty(),"capture_disabled_without_native_bridge");
        f.model.set_system_capture_status(true,false);f.menu_click(.035f+.445f*.93f,y);
        require(f.events.size()==1&&f.events[0].type==UiEventType::SystemCaptureChanged&&f.events[0].pressed,"capture_requires_manual_menu_entry");
        require(!f.model.system_capture_active()&&f.model.input_preferences().vr_controllers_enabled,"capture_waits_for_driver_ack_and_keeps_forwarding");
        f.model.set_system_capture_status(true,true);f.events.clear();f.menu_click(.035f+.445f*.80f,y);
        require(f.events.size()==1&&f.events[0].type==UiEventType::SystemCaptureChanged&&!f.events[0].pressed,"manual_capture_exit");
        require(f.model.input_preferences().vr_controllers_enabled,"system_exit_keeps_controller_forwarding");
        f.events.clear();f.menu_click(.515f+.445f*.93f,.41f);
        require(std::any_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::InputPreferenceChanged&&e.input_preference.id==InputPreferenceId::SyncRawBattery&&e.input_preference.value==1;}),"raw_battery_switch_hit");
        require(!f.model.input_preferences().sync_raw_battery,"raw_battery_waits_for_accepted_snapshot");
        RemoteSettings state;RemoteSettingsCapabilities caps;caps.connected=true;state.set_capabilities(caps);
        f.model.set_remote_settings(state.snapshot());f.events.clear();f.menu_click(.025f+.445f*.93f,.41f);
        f.menu_click(.915f,.075f);f.hands[0].menu=true;f.tick();f.hands[0].menu=false;f.tick();
        require(f.model.keyboard_panel().visible&&!f.model.settings_visible(),"original_menu_shortcut_during_capture");}++groups;
    {Fixture f;f.model.show_settings(true);f.hands[0].aim=aim_at(f.model.keyboard_panel(),.5f,.5f);f.hands[0].stick_y=-1;f.events.clear();
        for(unsigned i=0;i<24;++i)f.tick(.05f);
        require(near(f.model.settings_scroll_offset(),0)&&f.model.input_capture_mask()==std::array<bool,2>{true,true},"categorized_pages_fit_without_scroll_and_capture_game_input");
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::Scroll||e.type==UiEventType::PointerMove||e.type==UiEventType::PointerButton||e.type==UiEventType::Key;}),"settings_scroll_never_scrolls_pc_desktop");
        f.hands[0].stick_y=0;f.menu_click(.915f,.075f);require(!f.model.settings_visible(),"fixed_close_button_works_at_scroll_bottom");
        f.model.show_settings(true);require(near(f.model.settings_scroll_offset(),0),"reopening_settings_resets_viewport");
        f.menu_click(.98f,.75f);f.menu_click(.98f,.75f);
        f.hands[0].stick_y=1;for(unsigned i=0;i<24;++i)f.tick(.05f);
        require(near(f.model.settings_scroll_offset(),0),"stick_scroll_clamps_at_top");
        f.hands[0].stick_y=0;f.menu_click(.98f,.75f);f.menu_click(.38f,.075f);
        require(f.model.image_quality_page()&&near(f.model.settings_scroll_offset(),0),"tab_change_resets_viewport");
        f.hands[0].stick_y=-1;f.tick(.05f);require(near(f.model.settings_scroll_offset(),0),"short_local_image_page_has_no_empty_scroll");
        f.hands[0].stick_y=0;f.menu_click(.915f,.075f);require(!f.model.settings_visible(),"fixed_close_button_works_after_scroll");}++groups;
    {Fixture f;f.model.show_settings(true);f.menu_click(.38f,.075f);require(f.model.image_quality_page(),"local_quality_tab");
        const auto& rows=VrUiModel::image_quality_rows();
        require(rows.size()==3&&rows[0].id==ImageQualityId::DesktopFilter&&rows[1].id==ImageQualityId::VrUpscaling&&rows[2].id==ImageQualityId::VrTimewarp,"local_quality_visible_controls");
        ImageQualityPreferences preferences;preferences.desktop_filter=0;preferences.vr_timewarp=false;preferences.vr_sharpening_enabled=true;preferences.vr_sharpening=70;f.model.set_image_quality(preferences);
        for(const auto& row:rows){
            f.events.clear();f.menu_click(row.x+row.width*.93f,row.y+row.height/2);
            require(std::count_if(f.events.begin(),f.events.end(),[&](const auto& e){return e.type==UiEventType::ImageQualityChanged&&e.image_quality.id==row.id&&e.image_quality.value==1;})==1,"local_visible_toggle_event");
            require(image_quality_value(f.model.image_quality(),row.id)==0,"local_quality_waits_for_accepted_snapshot");
        }
        preferences.vr_timewarp=true;f.model.set_image_quality(preferences);f.events.clear();
        const auto& warp=rows[2];f.menu_click(warp.x+warp.width*.80f,warp.y+warp.height/2);
        require(std::count_if(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ImageQualityChanged&&e.image_quality.id==ImageQualityId::VrTimewarp&&e.image_quality.value==0;})==1,"timewarp_off_event");
        f.events.clear();f.menu_click(.91f,.71f);f.menu_click(.91f,.86f);
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::ImageQualityChanged;}),"hidden_sharpening_regions_are_inert");
        require(f.model.image_quality().vr_sharpening_enabled&&f.model.image_quality().vr_sharpening==70,"hidden_sharpening_preferences_retained");
        require(std::none_of(f.events.begin(),f.events.end(),[](const auto& e){return e.type==UiEventType::RemoteSettingChanged||e.type==UiEventType::Key||e.type==UiEventType::Text||e.type==UiEventType::PointerButton;}),"local_quality_never_emits_pc_input_or_settings");
        constexpr unsigned w=2048,h=768,stride=w*4+16;std::vector<std::uint8_t> pixels(stride*h+32,0x5a);f.model.raster_keyboard(pixels.data()+16,w,h,stride);
        require(std::all_of(pixels.begin(),pixels.begin()+16,[](auto b){return b==0x5a;})&&std::all_of(pixels.end()-16,pixels.end(),[](auto b){return b==0x5a;}),"local_quality_raster_guards");
        for(unsigned row=0;row<h;++row)require(std::all_of(pixels.begin()+16+row*stride+w*4,pixels.begin()+16+(row+1)*stride,[](auto b){return b==0x5a;}),"local_quality_raster_stride");
        f.menu_click(.25f,.075f);require(!f.model.image_quality_page(),"return_to_pc_settings_tab");}++groups;
    UiPanel a;a.source_id=3;UiPanel b;b.source_id=8;VrUiModel model;model.set_panels({a,b});model.reset_anchor({{1,1.7f,2},{}});
    auto saved=model.export_layout();require(validate_ui_layout(saved),"exported_layout_valid");
    saved.panels[0].curvature_radians=1.2f;saved.panels[0].width_m=2.4f;saved.panels[0].anchor_pose.position={.2f,.3f,-2.7f};saved.panels[0].visible=false;
    saved.keyboard_width_m=1.1f;saved.keyboard_visible=false;
    saved.keyboard_binding=2;saved.keyboard_offset={{.1f,.2f,-.3f},{0,std::sin(.2f),0,std::cos(.2f)}};saved.keyboard_space_width_m=2.2f;
    VrUiModel restored;restored.set_panels({b,a});
    require(!restored.restore_layout(saved),"restore_requires_anchor");
    const UiPose new_anchor{{10,2,-7},{0,std::sin(.6f),0,std::cos(.6f)}};restored.reset_anchor(new_anchor);
    require(restored.restore_layout(saved),"restore_layout");
    const auto expected=ui_compose(new_anchor,saved.panels[0].anchor_pose);
    require(restored.panels()[1].source_id==3&&!restored.panels()[1].visible&&near(restored.panels()[1].width,2.4f)&&
        near(restored.panels()[1].pose.position.x,expected.position.x)&&near(restored.panels()[1].pose.position.z,expected.position.z),"layout_matches_source_and_new_anchor");
    restored.set_aspect(1,.75f,.08f);require(near(restored.panels()[1].height,1.8f)&&!restored.keyboard_panel().visible,"layout_recomputes_video_aspect");
    const auto roundtrip_layout=restored.export_layout();require(near(roundtrip_layout.panels[1].anchor_pose.position.z,-2.7f),"layout_roundtrip_relative_pose");
    auto invalid=saved;invalid.panels[0].width_m=0;require(!validate_ui_layout(invalid)&&!restored.restore_layout(invalid)&&near(restored.panels()[1].width,2.4f),"invalid_restore_is_atomic");
    invalid=saved;invalid.version=4;require(!validate_ui_layout(invalid),"layout_version");
    invalid=saved;invalid.panels[1].source_id=invalid.panels[0].source_id;require(!validate_ui_layout(invalid),"duplicate_source");
    invalid=saved;invalid.keyboard_pose.position.x=11;require(!validate_ui_layout(invalid),"layout_position_bound");
    invalid=saved;invalid.panels[0].anchor_pose.orientation.w=0;require(!validate_ui_layout(invalid),"layout_unit_quaternion");
    invalid=saved;invalid.panels[0].width_m=std::numeric_limits<float>::quiet_NaN();require(!validate_ui_layout(invalid),"layout_nonfinite");
    auto largest=saved;largest.panels[0].width_m=kUiMaxPanelWidthM;
    require(validate_ui_layout(largest)&&restored.restore_layout(largest)&&near(restored.panels()[1].width,8.f)&&validate_ui_layout(restored.export_layout()),"eight_meter_layout_restore");
    auto oversized=largest;oversized.panels[0].width_m=8.01f;require(!validate_ui_layout(oversized),"layout_rejects_above_eight_meters");
#ifdef __linux__
    LayoutTestDirectory directory;const auto path=directory.root+"/state/panels.layout";UiLayout loaded;
    require(load_layout(path,loaded)==LayoutStoreStatus::Missing,"missing_layout_default");
    require(save_layout(path,saved)==LayoutStoreStatus::Ok,"layout_atomic_save");
    struct stat info{};require(::stat(path.c_str(),&info)==0&&(info.st_mode&0777)==0600,"layout_private_mode");
    require(load_layout(path,loaded)==LayoutStoreStatus::Ok&&loaded.panels.size()==2&&near(loaded.panels[0].width_m,2.4f)&&near(loaded.panels[0].curvature_radians,1.2f)&&!loaded.keyboard_visible,"layout_file_roundtrip");
    require(loaded.keyboard_binding==2&&near(loaded.keyboard_space_width_m,2.2f)&&near(loaded.keyboard_offset.position.z,-.3f)&&near(loaded.keyboard_offset.orientation.y,std::sin(.2f)),"keyboard_binding_file_roundtrip");
    require(save_layout(path,invalid)==LayoutStoreStatus::Invalid&&load_layout(path,loaded)==LayoutStoreStatus::Ok&&near(loaded.panels[0].width_m,2.4f),"invalid_save_preserves_previous");
    require(save_layout(path,largest)==LayoutStoreStatus::Ok&&load_layout(path,loaded)==LayoutStoreStatus::Ok&&near(loaded.panels[0].width_m,8.f),"eight_meter_file_roundtrip");
    auto changed=saved;changed.panels[0].width_m=1.3f;
    require(save_layout(path,changed)==LayoutStoreStatus::Ok&&load_layout(path,loaded)==LayoutStoreStatus::Ok&&near(loaded.panels[0].width_m,1.3f),"atomic_replace_existing");
    const auto bad=directory.root+"/bad.layout";
    {std::ofstream out(bad);out<<char(0x56)<<char(0x44)<<"_FRAME_LAYOUT 1 PANELS 1 PANEL 3 1 1.8 0 0 -2 0 0 0 1 KEYBOARD 0 1.55 0 -.78 -1.65 0 0 0 1 END";}
    UiLayout legacy;require(load_layout(bad,legacy)==LayoutStoreStatus::Ok&&legacy.version==1&&legacy.panels[0].curvature_radians==0,"legacy_v1_loads_flat");
    require(save_layout(bad,legacy)==LayoutStoreStatus::Ok&&load_layout(bad,legacy)==LayoutStoreStatus::Ok&&legacy.version==3&&legacy.panels[0].curvature_radians==0,"legacy_migrates_to_v3");
    for(const auto& data:std::vector<std::string>{"SURFXR_FRAME_LAYOUT 2\n", "SURFXR_FRAME_LAYOUT 1\nPANELS 99\n",std::string(16385,'x'),
        "SURFXR_FRAME_LAYOUT 1 PANELS 1 PANEL -1 1 1.8 0 0 -2 0 0 0 1 KEYBOARD 1 1.55 0 0 -1 0 0 0 1 END",
        "SURFXR_FRAME_LAYOUT 1 PANELS 1 PANEL 1 1 1.8 0 0 -2 0 0 0 1 KEYBOARD 1 1.55 0 0 -1 0 0 0 1 END EXTRA"}){
        {std::ofstream out(bad,std::ios::binary|std::ios::trunc);out<<data;}
        require(load_layout(bad,loaded)==LayoutStoreStatus::Invalid&&near(loaded.panels[0].width_m,1.3f),"malformed_file_leaves_output");
    }
    const auto link=directory.root+"/link.layout";require(::symlink(path.c_str(),link.c_str())==0&&load_layout(link,loaded)==LayoutStoreStatus::IoError,"layout_symlink_rejected");
    const auto fifo=directory.root+"/pipe.layout";require(::mkfifo(fifo.c_str(),0600)==0&&load_layout(fifo,loaded)==LayoutStoreStatus::Invalid,"layout_fifo_does_not_block");
    require(std::strcmp(layout_store_status_name(LayoutStoreStatus::Missing),"missing")==0,"fixed_store_status");
    return groups+4;
#else
    return groups+2;
#endif
}
} // namespace surfxr
int main(int argc,char** argv){
    if(argc>=3&&(std::strcmp(argv[1],"--preview-settings")==0||std::strcmp(argv[1],"--preview-keyboard")==0)){
        try{
            bool bottom=false,local=false,devices=false,diagnostics=false;unsigned hud_page=0;
            for(int i=3;i<argc;++i){
                if(std::strcmp(argv[i],"--scroll-bottom")==0)bottom=true;
                else if(std::strcmp(argv[i],"--local-image")==0)local=true;
                else if(std::strcmp(argv[i],"--devices")==0)devices=true;
                else if(std::strcmp(argv[i],"--diagnostics")==0)diagnostics=true;
                else if(std::strcmp(argv[i],"--mini-hud")==0&&i+1<argc){
                    const std::string page=argv[++i];diagnostics=true;
                    if(page=="content")hud_page=1;else if(page=="layout")hud_page=2;else if(page=="gaze")hud_page=3;
                    else throw std::runtime_error("unknown_mini_hud_preview_page");
                }
                else throw std::runtime_error("unknown_settings_preview_option");
            }
            surfxr::VrUiModel model;model.set_panels({surfxr::UiPanel{}});model.reset_anchor({});
            surfxr::RemoteSettings settings;surfxr::RemoteSettingsCapabilities caps;caps.connected=true;caps.can_launch_vr=true;
            caps.vr_ready=true;caps.stage_active=true;caps.active_codec=2;caps.eye_gaze_available=true;caps.microphone_available=true;caps.vr_framerates={72,90,120};
            caps.can_add_monitor=true;caps.can_remove_monitor=true;caps.desktop_monitor_count=2;
            settings.set_capabilities(caps);model.set_remote_settings(settings.snapshot());model.set_input_preferences({false});model.show_settings(true);
            std::array<surfxr::UiHand,2> hands{};hands[0].active=true;
            const auto click=[&](float u,float v){
                hands[0].aim=surfxr::aim_at(model.keyboard_panel(),u,v);
                hands[0].select=false;model.update(hands,.02f,{});hands[0].select=true;model.update(hands,.02f,{});hands[0].select=false;model.update(hands,.02f,{});
            };
            if(local)click(.38f,.075f);
            if(devices)click(.50f,.075f);
            if(diagnostics)click(.63f,.075f);
            if(hud_page)click(.13f+.245f*hud_page,.72f);
            if(bottom)for(unsigned i=0;i<8;++i)click(.98f,.75f);
            if(std::strcmp(argv[1],"--preview-keyboard")==0){model.show_settings(false);model.bind_keyboard(1);}
            constexpr unsigned w=2048,h=768;std::vector<std::uint8_t> rgba(w*h*4);
            model.raster_keyboard(rgba.data(),w,h,w*4);
            std::ofstream output(argv[2],std::ios::binary);if(!output)throw std::runtime_error("settings_preview_open_failed");
            output<<"P6\n"<<w<<" "<<h<<"\n255\n";
            for(std::size_t i=0;i<rgba.size();i+=4)output.write(reinterpret_cast<const char*>(rgba.data()+i),3);
            if(!output)throw std::runtime_error("settings_preview_write_failed");
            return 0;
        }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    }
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::cout<<"surfxr-ui-test --self-test (offline model only)\n";return argc==1?0:1;}
    try{const auto groups=surfxr::vr_ui_self_test();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"network_used\":false,\"openxr_session_started\":false}\n";return 0;}
    catch(const std::exception& e){std::cerr<<"ui_self_test: "<<e.what()<<'\n';return 1;}
}
