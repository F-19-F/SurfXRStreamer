#include "surfxr/vr_ui.hpp"
#include "surfxr/keyboard_image.hpp"
#include <cmath>
#include <stdexcept>
#ifdef __linux__
#include "dashboard_wakeup_checks.hpp"
#endif

namespace surfxr {
void keyboard_checks(){
#ifdef __linux__
    dashboard_wakeup_checks();
#endif
    const auto check=[](bool v,const char* label){if(!v)throw std::runtime_error(label);};
    VrUiModel model;model.set_panels({UiPanel{}});model.reset_anchor({});
    std::array<UiHand,2> hands{};std::vector<UiEvent> events;
    const auto tick=[&]{model.update(hands,.02f,[&](const auto& e){events.push_back(e);});};
    hands[0].active=hands[1].active=true;hands[0].menu=true;tick();hands[0].menu=false;tick();
    model.bind_keyboard(1);hands[0].attachment_valid=true;
    hands[0].attachment={{-.2f,1.f,-.5f},{}};tick();
    check(model.keyboard_panel().available&&std::abs(model.keyboard_panel().width-.85f)<.001f,"keyboard_hand_size");
    check(std::abs(model.keyboard_panel().pose.position.y-1.16f)<.001f,"keyboard_uses_grip_pose");
    const auto key=*std::find_if(model.keys().begin(),model.keys().end(),[](const auto& k){return k.key==UiKey::A;});
    auto point=ui_panel_point(model.keyboard_panel(),key.x+key.width/2,key.y+key.height/2);
    hands[1].aim={point,model.keyboard_panel().pose.orientation};hands[1].aim.position.z+=.3f;
    hands[0].aim=hands[1].aim;tick();events.clear();
    hands[0].select=true;tick();
    check(!model.hits()[0].valid&&events.empty(),"keyboard_holder_cannot_type");
    hands[1].select=true;tick();
    check(events.size()==2&&events[0].key==UiKey::A&&events[0].pressed&&!events[1].pressed,"keyboard_opposite_hand_types_once");
    tick();check(events.size()==2,"keyboard_held_trigger_no_repeat");
    hands[0].select=hands[1].select=false;tick();
    model.candidate(600,100,42);
    const auto candidate=model.candidate_panel();
    point=ui_panel_point(candidate,.3f,.6f);hands[1].aim={point,candidate.pose.orientation};hands[1].aim.position.z+=.3f;
    hands[0].aim=hands[1].aim;tick();events.clear();hands[0].select=true;tick();
    check(events.empty()&&!model.hits()[0].valid,"candidate_holder_cannot_click");
    hands[1].select=true;tick();tick();
    check(events.size()==1&&events[0].candidate==42&&events[0].type==UiEventType::PointerButton&&std::abs(events[0].u-.3f)<.001f&&std::abs(events[0].v-.6f)<.001f,"candidate_click_once_with_image_coordinates");
    model.candidate(0,0,0);hands[0].select=hands[1].select=false;tick();events.clear();hands[1].select=true;tick();
    check(events.empty()&&!model.hits()[1].valid,"candidate_disappearance_does_not_click_through");
    hands[1].select=false;point=ui_panel_point(model.keyboard_panel(),key.x+key.width/2,key.y+key.height/2);
    hands[1].aim={point,model.keyboard_panel().pose.orientation};hands[1].aim.position.z+=.3f;tick();
    hands[1].grip=true;tick();hands[1].aim.position.x+=.1f;tick();
    hands[1].grip=false;tick();const auto offset=model.export_layout().keyboard_offset;
    check(std::abs(offset.position.x-.1f)<.001f,"keyboard_drag_updates_relative_offset");
    hands[0].attachment.position.x+=.2f;tick();
    check(std::abs(model.keyboard_panel().pose.position.x-.1f)<.001f,"keyboard_follows_with_saved_offset");
    // Rotate the physical grip while leaving the pointing ray unchanged.
    // Roll must rotate the board's horizontal axis as well as its offset.
    const auto close=[](UiVec3 a,UiVec3 b){return std::abs(a.x-b.x)<.001f&&std::abs(a.y-b.y)<.001f&&std::abs(a.z-b.z)<.001f;};
    const float half=std::sqrt(.5f);
    hands[0].attachment.orientation={0,0,half,half};tick();
    check(close(model.keyboard_panel().pose.position,{-.16f,1.1f,-.78f})&&
        close(ui_rotate(model.keyboard_panel().pose.orientation,{1,0,0}),{0,1,0}),"keyboard_grip_roll_rotates_axes_and_offset");
    hands[0].attachment.orientation={half,0,0,half};tick();
    check(close(model.keyboard_panel().pose.position,{.1f,1.28f,-.34f})&&
        close(ui_rotate(model.keyboard_panel().pose.orientation,{0,0,1}),{0,-1,0}),"keyboard_grip_pitch");
    hands[0].attachment.orientation={0,half,0,half};tick();
    check(close(model.keyboard_panel().pose.position,{-.28f,1.16f,-.6f})&&
        close(ui_rotate(model.keyboard_panel().pose.orientation,{0,0,1}),{1,0,0}),"keyboard_grip_yaw");
    hands[0].attachment_valid=false;tick();
    check(!model.keyboard_panel().available,"keyboard_invalid_grip_does_not_switch_to_aim_axes");
    hands[0].attachment_valid=true;hands[0].attachment.orientation={};tick();
    const auto saved=model.export_layout();VrUiModel restored;restored.set_panels({UiPanel{}});restored.reset_anchor({});
    check(restored.restore_layout(saved)&&restored.keyboard_binding()==1,"keyboard_binding_restore");
    hands[0].active=false;events.clear();tick();
    check(!model.keyboard_panel().available&&!model.hits()[1].valid,"keyboard_tracking_loss_hides_board");
    check(std::any_of(events.begin(),events.end(),[](const auto& e){return e.type==UiEventType::ReleaseAll;}),"keyboard_tracking_loss_releases");
    model.bind_keyboard(0);check(model.keyboard_panel().available&&std::abs(model.keyboard_panel().width-2.05f)<.001f,"keyboard_returns_to_space");
    model.bind_keyboard(2);hands[1].attachment_valid=true;hands[1].attachment={{.2f,1.f,-.5f},{}};tick();
    check(model.keyboard_panel().available&&!model.hits()[1].valid,"keyboard_right_binding");

    // Production lossless codec, trailer removal and strict allocation bounds.
    const std::uint8_t pixels[]{1,2,3,0,1,2,3,0,4,5,6,0};
    const auto image=keyboard_image::encode(3,1,pixels);KeyboardOverlay overlay;
    const auto identified=keyboard_image::identify(image,42);const auto tagged=keyboard_image::decode(identified.data(),identified.size());
    check(tagged->token==42&&tagged->width==3&&tagged->height==1,"candidate_image_identity_roundtrip");
    auto packet=transport::Bytes{9,8,7};packet.insert(packet.end(),image.begin(),image.end());
    transport::Writer footer;footer.integer(image.size(),4);packet.insert(packet.end(),footer.bytes.begin(),footer.bytes.end());
    packet.insert(packet.end(),keyboard_image::magic.begin(),keyboard_image::magic.end());
    check(keyboard_image::extract(packet,1,overlay)&&packet==transport::Bytes({9,8,7}),"keyboard_suffix_preserves_video");
    const auto decoded=overlay.snapshot();check(decoded&&decoded->width==3&&decoded->rgba==transport::Bytes({3,2,1,255,3,2,1,255,6,5,4,255}),"keyboard_lossless_rgb");
    check(!keyboard_image::extract(packet,1,overlay),"keyboard_legacy_packet_unchanged");
    for(auto bad:std::vector<transport::Bytes>{{0,0,1,0},{1,0,1,0,2,0,0,0,0},{1,0,1,0,0,0,0,0,0},{1,0,1,0}}){
        bool rejected=false;try{keyboard_image::decode(bad.data(),bad.size());}catch(const std::runtime_error&){rejected=true;}
        check(rejected,"keyboard_malformed_image_rejected");
    }
    packet={9};packet.resize(5,0);packet.insert(packet.end(),keyboard_image::magic.begin(),keyboard_image::magic.end());
    check(keyboard_image::extract(packet,0,overlay)&&!overlay.snapshot()&&packet==transport::Bytes{9},"keyboard_clear_suffix");
}
}
