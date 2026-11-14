#include "surfxr/vr_ui.hpp"
#include "ui_helpers.hpp"
#include "surfxr/ui_input.hpp"
#include "surfxr/localization.hpp"
#include <algorithm>
#include <cmath>
namespace surfxr {
ScanCode ui_scan_code(UiKey key){
    // Set-1 scan codes; E0 prefixes preserve right modifiers and navigation.
    constexpr std::array<unsigned,26> letters{0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    constexpr std::array<unsigned,10> digits{0x0b,2,3,4,5,6,7,8,9,10};
    constexpr std::array<unsigned,10> numpad{0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49};
    const auto at=[&](UiKey first){return static_cast<unsigned>(key)-static_cast<unsigned>(first);};
    if(key>=UiKey::A&&key<=UiKey::Z)return static_cast<ScanCode>(letters[at(UiKey::A)]);
    if(key>=UiKey::Digit0&&key<=UiKey::Digit9)return static_cast<ScanCode>(digits[at(UiKey::Digit0)]);
    if(key>=UiKey::Numpad0&&key<=UiKey::Numpad9)return static_cast<ScanCode>(numpad[at(UiKey::Numpad0)]);
    if(key>=UiKey::F1&&key<=UiKey::F10)return static_cast<ScanCode>(0x3b+at(UiKey::F1));
    switch(key){
#define MAP(name,code) case UiKey::name:return static_cast<ScanCode>(code)
        MAP(Backspace,0x0e);MAP(Enter,0x1c);MAP(Tab,0x0f);MAP(Escape,1);
        MAP(Left,0xe04b);MAP(Right,0xe04d);MAP(Up,0xe048);MAP(Down,0xe050);
        MAP(Grave,0x29);MAP(Minus,0x0c);MAP(Equal,0x0d);MAP(LeftBracket,0x1a);MAP(RightBracket,0x1b);
        MAP(Backslash,0x2b);MAP(Semicolon,0x27);MAP(Apostrophe,0x28);MAP(Comma,0x33);MAP(Period,0x34);MAP(Slash,0x35);MAP(Space,0x39);
        MAP(F11,0x57);MAP(F12,0x58);MAP(PrintScreen,0xe037);MAP(ScrollLock,0x46);MAP(Pause,0xe045);
        MAP(Insert,0xe052);MAP(Delete,0xe053);MAP(Home,0xe047);MAP(End,0xe04f);MAP(PageUp,0xe049);MAP(PageDown,0xe051);
        MAP(CapsLock,0x3a);MAP(NumLock,0x45);MAP(LeftShift,0x2a);MAP(RightShift,0x36);
        MAP(LeftControl,0x1d);MAP(RightControl,0xe01d);MAP(LeftAlt,0x38);MAP(RightAlt,0xe038);
        MAP(LeftWindows,0xe05b);MAP(RightWindows,0xe05c);MAP(Menu,0xe05d);
        MAP(NumpadDecimal,0x53);MAP(NumpadDivide,0xe035);MAP(NumpadMultiply,0x37);MAP(NumpadSubtract,0x4a);MAP(NumpadAdd,0x4e);MAP(NumpadEnter,0xe01c);
        MAP(VolumeMute,0xe020);MAP(VolumeDown,0xe02e);MAP(VolumeUp,0xe030);MAP(Calculator,0xe021);
#undef MAP
        default:throw std::runtime_error("unsupported_ui_key");
    }
}

UiPanel ui_candidate_panel(const UiPanel& keyboard,unsigned width,unsigned height){
    UiPanel panel=keyboard;panel.width=keyboard.width*.9f;panel.height=width?panel.width*height/width:0;
    panel.visible=keyboard.visible&&width&&height;
    panel.pose=ui_compose(keyboard.pose,{{0,keyboard.height/2+panel.height/2+.015f,.005f},{}});return panel;
}
namespace {void emit(const UiEventCallback& fn,const UiEvent& e){if(fn)fn(e);}}
void VrUiModel::build_keys(){
    keys_.clear();
    for(std::size_t i=0;i<panels_.size();++i)keys_.push_back({.012f+.12f*i,.015f,.11f,.075f,"CAPTURE "+std::to_string(i+1),"",UiKey::None,100+static_cast<int>(i)});
    keys_.push_back({.493f,.015f,.22f,.075f,"SETTINGS","",UiKey::None,5});
    keys_.push_back({.724f,.015f,.13f,.075f,"TAKE OVER","",UiKey::None,9});
    keys_.push_back({.865f,.015f,.12f,.075f,"DASHBOARD","",UiKey::None,3});
    if(shortcut_recording()){
        keys_.push_back({.012f,.92f,.60f,.07f,"", "",UiKey::None,10});
        keys_.push_back({.63f,.92f,.11f,.07f,"SAVE","",UiKey::None,13});
        keys_.push_back({.75f,.92f,.11f,.07f,"CANCEL","",UiKey::None,14});
        keys_.push_back({.87f,.92f,.115f,.07f,static_cast<unsigned>(shortcut_slot_)<input_preferences_.input_shortcuts.size()?"DELETE":"CLEAR","",UiKey::None,15});
    }else{
        for(unsigned i=0;i<3;++i)keys_.push_back({.012f+.113f*i,.92f,.105f,.07f,i==0?"IN SPACE":i==1?"LEFT HAND":"RIGHT HAND","",UiKey::None,6+static_cast<int>(i)});
        keys_.push_back({.36f,.92f,.04f,.07f,"←","",UiKey::None,16});
        for(unsigned i=0;i<3&&shortcut_offset_+i<input_preferences_.input_shortcuts.size();++i)
            keys_.push_back({.405f+.15f*i,.92f,.145f,.07f,"","",UiKey::Shortcut,200+static_cast<int>(shortcut_offset_+i)});
        keys_.push_back({.855f,.92f,.04f,.07f,"→","",UiKey::None,17});
        keys_.push_back({.90f,.92f,.04f,.07f,"+","",UiKey::None,11});
        keys_.push_back({.945f,.92f,.04f,.07f,"EDIT","",UiKey::None,12});
    }
    const float rows[]={.14f,.28f,.405f,.53f,.655f,.78f};
    const auto key=[&](float x,unsigned row,float width,const std::string& label,UiKey code,bool modifier=false,float height=1.f){
        keys_.push_back({(x+.15f)/18.8f,rows[row],(width-.07f)/18.8f,(row==0?.09f:.11f)+(height-1)*.125f,label,"",code,0,modifier});};
    key(0,0,1,"ESC",UiKey::Escape);
    for(unsigned i=0;i<12;++i)key(2+i+(i>=4?.5f:0)+(i>=8?.5f:0),0,1,"F"+std::to_string(i+1),static_cast<UiKey>(static_cast<int>(UiKey::F1)+i));
    key(15.5f,0,1,"PRTSC",UiKey::PrintScreen);key(16.5f,0,1,"SCRLK",UiKey::ScrollLock);key(17.5f,0,1,"PAUSE",UiKey::Pause);
    key(0,1,1,"`",UiKey::Grave);
    for(unsigned i=0;i<10;++i)key(1.f+static_cast<float>(i),1,1,std::to_string((i+1)%10),static_cast<UiKey>(static_cast<int>(UiKey::Digit0)+(i+1)%10));
    key(11,1,1,"-",UiKey::Minus);key(12,1,1,"=",UiKey::Equal);key(13,1,2,"BKSP",UiKey::Backspace);
    const auto letters=[&](const std::string& text,unsigned row,float x){for(char c:text){key(x++,row,1,std::string(1,c),static_cast<UiKey>(static_cast<int>(UiKey::A)+c-'A'));}};
    key(0,2,1.5f,"TAB",UiKey::Tab);letters("QWERTYUIOP",2,1.5f);key(11.5f,2,1,"[",UiKey::LeftBracket);key(12.5f,2,1,"]",UiKey::RightBracket);key(13.5f,2,1.5f,"\\",UiKey::Backslash);
    key(0,3,1.75f,"CAPS",UiKey::CapsLock);letters("ASDFGHJKL",3,1.75f);key(10.75f,3,1,";",UiKey::Semicolon);key(11.75f,3,1,"'",UiKey::Apostrophe);key(12.75f,3,2.25f,"ENTER",UiKey::Enter);
    key(0,4,2.25f,"LSHIFT",UiKey::LeftShift,true);letters("ZXCVBNM",4,2.25f);key(9.25f,4,1,",",UiKey::Comma);key(10.25f,4,1,".",UiKey::Period);key(11.25f,4,1,"/",UiKey::Slash);key(12.25f,4,2.75f,"RSHIFT",UiKey::RightShift,true);
    key(0,5,1.25f,"LCTRL",UiKey::LeftControl,true);key(1.25f,5,1.25f,"LWIN",UiKey::LeftWindows,true);key(2.5f,5,1.25f,"LALT",UiKey::LeftAlt,true);key(3.75f,5,6.25f,"SPACE",UiKey::Space);
    key(10,5,1.25f,"RALT",UiKey::RightAlt,true);key(11.25f,5,1.25f,"RWIN",UiKey::RightWindows,true);key(12.5f,5,1.25f,"MENU",UiKey::Menu);key(13.75f,5,1.25f,"RCTRL",UiKey::RightControl,true);
    key(15.5f,1,1,"INS",UiKey::Insert);key(16.5f,1,1,"HOME",UiKey::Home);key(17.5f,1,1,"PGUP",UiKey::PageUp);
    key(15.5f,2,1,"DEL",UiKey::Delete);key(16.5f,2,1,"END",UiKey::End);key(17.5f,2,1,"PGDN",UiKey::PageDown);
    key(16.5f,4,1,"↑",UiKey::Up);key(15.5f,5,1,"←",UiKey::Left);key(16.5f,5,1,"↓",UiKey::Down);key(17.5f,5,1,"→",UiKey::Right);
    for(auto& item:keys_)if(item.key==UiKey::Shortcut)item.label=shortcut_label(input_preferences_.input_shortcuts.at(item.command-200));
}
void VrUiModel::activate_key(std::size_t index,const UiEventCallback& events){
    if(index>=keys_.size())return;
    const auto key=keys_[index];
    if(activate_shortcut_key(key,events))return;
    if(key.command==1){shift_=!shift_;++revision_;return;}
    if(key.command==2){release_inputs(events);layout();return;}
    if(key.command==3){release_inputs(events);UiEvent e;e.type=UiEventType::ShowDashboard;emit(events,e);return;}
    if(key.command>=6&&key.command<=8){bind_keyboard(static_cast<unsigned>(key.command-6),events);return;}
    if(key.command==9){
        if(!system_capture_active_&&!system_capture_available_)return;
        release_inputs(events);UiEvent e;e.type=UiEventType::SystemCaptureChanged;
        e.pressed=!system_capture_active_;emit(events,e);return;
    }
    if(key.command==5){show_settings(true,events);return;}
    if(key.command>=100){const auto id=static_cast<std::uint32_t>(key.command-100);set_visible(id,!panels_.at(id).visible,events);return;}

    UiEvent e;e.source_id=focus_source_;e.panel_id=focus_panel_;
    if(key.key==UiKey::None)return;
    e.type=UiEventType::Key;e.key=key.key;
    if(key.modifier){
        const auto held=std::find(held_modifiers_.begin(),held_modifiers_.end(),key.key);
        e.pressed=held==held_modifiers_.end();if(e.pressed)held_modifiers_.push_back(key.key);else held_modifiers_.erase(held);
        shift_=std::find(held_modifiers_.begin(),held_modifiers_.end(),UiKey::LeftShift)!=held_modifiers_.end()||std::find(held_modifiers_.begin(),held_modifiers_.end(),UiKey::RightShift)!=held_modifiers_.end();++revision_;emit(events,e);
    }else{e.pressed=true;emit(events,e);e.pressed=false;emit(events,e);}

}

const char* VrUiModel::shortcut_hint() const {
    if(shortcut_recording())return "RECORDING: SELECT KEYS, THEN SAVE";
    if(shortcut_edit_)return "EDIT: SELECT A SHORTCUT";
    for(const auto& hit:hits_)if(hit.valid&&hit.region==UiHitRegion::Keyboard&&hit.v>=.92f&&hit.u>=.35f)return "SHORTCUTS: STICK TO SCROLL | TRIGGER TO RUN";
    return nullptr;
}
std::string VrUiModel::shortcut_label(const KeyboardShortcut& chord) const {
    std::string text;
    for(auto code:chord){if(!code)break;
        const auto found=std::find_if(keys_.begin(),keys_.end(),[&](const auto& k){return k.command==0&&k.key!=UiKey::None&&static_cast<unsigned>(ui_scan_code(k.key))==code;});
        if(!text.empty())text+=" + ";
        switch(code){case 0x1d:text+="Ctrl";break;case 0xe01d:text+="RCtrl";break;case 0x2a:text+="Shift";break;case 0x36:text+="RShift";break;
            case 0x38:text+="Alt";break;case 0xe038:text+="RAlt";break;case 0xe05b:text+="Win";break;case 0xe05c:text+="RWin";break;case 0x39:text+="Space";break;
            default:text+=found==keys_.end()?"?":found->label;break;}
    }
    return text.empty()?i18n::translate("SELECT KEYS"):text;
}
void VrUiModel::scroll_shortcuts(int direction){
    const auto limit=input_preferences_.input_shortcuts.size()>3?input_preferences_.input_shortcuts.size()-3:0;
    const auto next=static_cast<unsigned>(std::clamp(static_cast<int>(shortcut_offset_)+direction,0,static_cast<int>(limit)));
    if(next!=shortcut_offset_){shortcut_offset_=next;build_keys();++revision_;}
}
void VrUiModel::save_shortcut(const KeyboardShortcut& chord,const UiEventCallback& events){
    if(!shortcut_recording()||!valid_keyboard_shortcut(chord))return;
    const auto slot=static_cast<unsigned>(shortcut_slot_);
    if(slot>input_preferences_.input_shortcuts.size()||slot>=kMaxKeyboardShortcuts)return;
    if(slot==input_preferences_.input_shortcuts.size())input_preferences_.input_shortcuts.push_back(chord);else input_preferences_.input_shortcuts[slot]=chord;
    UiEvent event;event.type=UiEventType::ShortcutChanged;event.button=slot;event.shortcut=chord;event.pressed=true;emit(events,event);
    shortcut_slot_=-1;shortcut_draft_={};shortcut_edit_=false;scroll_shortcuts(static_cast<int>(slot)-static_cast<int>(shortcut_offset_));build_keys();++revision_;
}
bool VrUiModel::activate_shortcut_key(const UiKeyRect& key,const UiEventCallback& events){
    if(shortcut_recording()){
        if(key.command==13)save_shortcut(shortcut_draft_,events);
        else if(key.command==15){const auto slot=static_cast<unsigned>(shortcut_slot_);
            if(slot<input_preferences_.input_shortcuts.size()){
                input_preferences_.input_shortcuts.erase(input_preferences_.input_shortcuts.begin()+slot);
                UiEvent event;event.type=UiEventType::ShortcutChanged;event.button=slot;event.pressed=false;emit(events,event);
                shortcut_slot_=-1;shortcut_draft_={};shortcut_edit_=false;scroll_shortcuts(-1);build_keys();
            }else shortcut_draft_={};++revision_;
        }else if(key.command==14){shortcut_slot_=-1;shortcut_draft_={};shortcut_edit_=false;build_keys();++revision_;}
        else if(key.command==0&&key.key!=UiKey::None){
            const auto code=static_cast<std::uint16_t>(ui_scan_code(key.key));
            const auto found=std::find(shortcut_draft_.begin(),shortcut_draft_.end(),code);
            if(found!=shortcut_draft_.end()){std::move(found+1,shortcut_draft_.end(),found);shortcut_draft_.back()=0;}
            else if(const auto empty=std::find(shortcut_draft_.begin(),shortcut_draft_.end(),0);empty!=shortcut_draft_.end())*empty=code;
            ++revision_;
        }
        return true;
    }
    if(key.command==16||key.command==17){scroll_shortcuts(key.command==16?-1:1);return true;}
    if(key.command==12){release_inputs(events);shortcut_edit_=!shortcut_edit_;++revision_;return true;}
    if(key.command==11){
        if(input_preferences_.input_shortcuts.size()>=kMaxKeyboardShortcuts)return true;
        release_inputs(events);shortcut_slot_=static_cast<int>(input_preferences_.input_shortcuts.size());shortcut_draft_={};build_keys();++revision_;return true;
    }
    if(key.key!=UiKey::Shortcut)return false;
    const auto slot=static_cast<unsigned>(key.command-200);if(slot>=input_preferences_.input_shortcuts.size())return true;
    release_inputs(events);
    if(shortcut_edit_){shortcut_slot_=static_cast<int>(slot);shortcut_draft_=input_preferences_.input_shortcuts[slot];build_keys();++revision_;}
    else {UiEvent event;event.type=UiEventType::Key;event.key=UiKey::Shortcut;event.pressed=true;event.shortcut=input_preferences_.input_shortcuts[slot];emit(events,event);}
    return true;
}
void VrUiModel::bind_keyboard(unsigned binding,const UiEventCallback& events){
    if(binding>2||binding==keyboard_binding_)return;
    release_inputs(events);
    if(!keyboard_binding_){keyboard_space_pose_=keyboard_.pose;keyboard_space_width_=keyboard_.width;}
    keyboard_binding_=binding;
    const auto ratio=keyboard_.height/keyboard_.width;
    keyboard_.width=binding?.85f:keyboard_space_width_;keyboard_.height=keyboard_.width*ratio;
    if(!binding){keyboard_.pose=keyboard_space_pose_;keyboard_.available=true;}
    else keyboard_.available=false; // wait for a valid controller sample
    ++revision_;
}
void VrUiModel::update_keyboard_attachment(const std::array<UiHand,2>& input,const std::array<bool,2>& active){
    if(!keyboard_binding_)return;
    const auto hand=keyboard_binding_-1;
    const auto& in=input[hand];
    const auto pose=in.attachment;
    keyboard_.available=active[hand]&&in.attachment_valid&&ui_detail::valid_pose(pose);
    if(!keyboard_.available)return;
    keyboard_attachment_=ui_detail::normalized_pose(pose);
    // The other hand may reposition the board; store its full rigid transform
    // relative to the physical grip, including angle, not an absolute XR pose.
    bool dragging=false;for(const auto& h:hands_)dragging|=h.drag==4;
    if(!dragging)keyboard_.pose=ui_compose(keyboard_attachment_,keyboard_offset_);
}
} // namespace surfxr
