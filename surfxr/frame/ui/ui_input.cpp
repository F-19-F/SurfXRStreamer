#include "surfxr/ui_input.hpp"
#include <array>
#include <set>
#include <stdexcept>
namespace surfxr {
void queue_ui_input(const UiEvent& event,SessionInputQueue& queue){
    switch(event.type){
        case UiEventType::PointerMove:queue.pointer_move(event.source_id,event.u,event.v);break;
        case UiEventType::PointerButton:
            if(event.candidate){queue.candidate_click(event.candidate,event.u,event.v);break;}
            queue.pointer_move(event.source_id,event.u,event.v);queue.pointer_button(event.button,event.pressed);break;
        case UiEventType::Scroll:queue.scroll_wheel(0,event.scroll_y);break;
        case UiEventType::Text:queue.text_ascii(event.text);break;
        case UiEventType::Key:
            if(event.key==UiKey::Shortcut){
                if(event.pressed&&valid_keyboard_shortcut(event.shortcut)){
                    std::vector<ScanCode> keys;for(auto code:event.shortcut){if(!code)break;keys.push_back(static_cast<ScanCode>(code));}
                    queue.shortcut(keys);
                }
            }else if(event.key!=UiKey::None)queue.key(ui_scan_code(event.key),event.pressed);
            break;
        case UiEventType::ReleaseAll:case UiEventType::ShowDashboard:case UiEventType::Exit:queue.release_all();break;
        case UiEventType::ShortcutChanged:case UiEventType::InputPreferenceChanged:case UiEventType::ImageQualityChanged:case UiEventType::RemoteSettingChanged:case UiEventType::LaunchVr:case UiEventType::ExitVr:
        case UiEventType::ToggleMode:case UiEventType::Recenter:case UiEventType::ShowDesktop:
        case UiEventType::AddMonitor:case UiEventType::RemoveMonitor:case UiEventType::SystemCaptureChanged:break; // Viewer owns session controls.
    }
}
void ui_input_self_test(){
    VrUiModel model;model.set_panels({UiPanel{}});
    std::set<unsigned> codes;
    SessionInputQueue queue;
    for(const auto& key:model.keys())if(key.key!=UiKey::None&&key.key!=UiKey::Shortcut){
        const auto code=ui_scan_code(key.key);
        if(!codes.insert(static_cast<unsigned>(code)).second||!queue.key(code,true)||!queue.key(code,false))
            throw std::runtime_error("keyboard_mapping_duplicate_or_invalid");
    }
    queue.release_all();transport::desktop::Event event;queue.next_desktop_event(event);
    for(unsigned slot=0;slot<2;++slot){
        UiEvent action;action.type=UiEventType::Key;action.key=UiKey::Shortcut;action.shortcut=kDefaultInputShortcuts[slot];action.pressed=true;
        queue_ui_input(action,queue);action.pressed=false;queue_ui_input(action,queue);
        unsigned downs=0,ups=0,releases=0;
        while(queue.next_desktop_event(event)){if(event.kind==transport::desktop::EventKind::Key){if(event.b)++downs;else ++ups;}else if(event.kind==transport::desktop::EventKind::Release)++releases;else throw std::runtime_error("shortcut_non_key_event");}
        if(downs!=2||downs!=ups||releases!=2)throw std::runtime_error("shortcut_ui_dispatch");
    }
    if(codes.size()!=87||ui_scan_code(UiKey::Pause)!=ScanCode::Pause||ui_scan_code(UiKey::RightControl)!=ScanCode::RightControl)
        throw std::runtime_error("keyboard_mapping_incomplete");
}
}
