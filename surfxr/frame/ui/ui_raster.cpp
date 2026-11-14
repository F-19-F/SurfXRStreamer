#include "ui_helpers.hpp"
#include "localized_text.hpp"
#include "surfxr/localization.hpp"
using namespace surfxr::ui_detail;
#include "surfxr/vr_ui.hpp"
#include "surfxr/ui_input.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace surfxr {
namespace {
const char* arrow_glyph(const std::string& text){
    if(text=="↑")return "00100" "01110" "10101" "00100" "00100" "00100" "00100";
    if(text=="↓")return "00100" "00100" "00100" "00100" "10101" "01110" "00100";
    if(text=="←")return "00000" "00100" "01000" "11111" "01000" "00100" "00000";
    if(text=="→")return "00000" "00100" "00010" "11111" "00010" "00100" "00000";
    return nullptr;
}
const char* glyph(char c){
    // Original 5x7 glyphs. One character per pixel, rows separated only here for
    // readability; no font assets, external library, or text network requests.
    switch(c){
    case 'A':return "01110100011000111111100011000110001";
    case 'B':return "11110100011000111110100011000111110";
    case 'C':return "01111100001000010000100001000001111";
    case 'D':return "11110100011000110001100011000111110";
    case 'E':return "11111100001000011110100001000011111";
    case 'F':return "11111100001000011110100001000010000";
    case 'G':return "01111100001000010111100011000101111";
    case 'H':return "10001100011000111111100011000110001";
    case 'I':return "11111001000010000100001000010011111";
    case 'J':return "00111000100001000010000101001001100";
    case 'K':return "10001100101010011000101001001010001";
    case 'L':return "10000100001000010000100001000011111";
    case 'M':return "10001110111010110101100011000110001";
    case 'N':return "10001110011010110011100011000110001";
    case 'O':return "01110100011000110001100011000101110";
    case 'P':return "11110100011000111110100001000010000";
    case 'Q':return "01110100011000110001101011001001101";
    case 'R':return "11110100011000111110101001001010001";
    case 'S':return "01111100001000001110000010000111110";
    case 'T':return "11111001000010000100001000010000100";
    case 'U':return "10001100011000110001100011000101110";
    case 'V':return "10001100011000110001100010101000100";
    case 'W':return "10001100011000110101101011010101010";
    case 'X':return "10001100010101000100010101000110001";
    case 'Y':return "10001100010101000100001000010000100";
    case 'Z':return "11111000010001000100010001000011111";
    case '0':return "01110100011001110101110011000101110";
    case '1':return "00100011000010000100001000010001110";
    case '2':return "01110100010000100010001000100011111";
    case '3':return "11110000010000101110000010000111110";
    case '4':return "00010001100101010010111110001000010";
    case '5':return "11111100001000011110000010000111110";
    case '6':return "01110100001000011110100011000101110";
    case '7':return "11111000010001000100010000100001000";
    case '8':return "01110100011000101110100011000101110";
    case '9':return "01110100011000101111000010000101110";
    case '-':return "00000000000000011111000000000000000";
    case '=':return "00000000001111100000111110000000000";
    case '.':return "00000000000000000000000000011000110";
    case ',':return "00000000000000000000001100011000100";
    case '/':return "00001000010001000100010001000010000";
    case '\\':return "10000100000100000100000100000100001";
    case '[':return "01110010000100001000010000100001110";
    case ']':return "01110000100001000010000100001001110";
    case ';':return "00000001100011000000001100011000100";
    case ':':return "00000001100011000000001100011000000";
    case '\'':return "00100001000010000000000000000000000";
    case '"':return "01010010100101000000000000000000000";
    case '?':return "01110100010000100010001000000000100";
    case '!':return "00100001000010000100001000000000100";
    case '+':return "00000001000010011111001000010000000";
    case '_':return "00000000000000000000000000000011111";
    case '(':return "00010001000100001000010000010000010";
    case ')':return "01000001000001000010000100010001000";
    case '<':return "00001000100010001000001000001000001";
    case '>':return "10000010000010000010001000100010000";
    case '*':return "00000101010111011111011101010100000";
    case '#':return "01010010101111101010111110101001010";
    case '$':return "00100011111010001110001011111000100";
    case '%':return "11001110100001000100010001001110011";
    case '&':return "01100100101010001000101011001001101";
    case '@':return "01110100011011110101101111000001111";
    case '^':return "00100010101000100000000000000000000";
    case '{':return "00011001000010001000001000010000011";
    case '}':return "11000001000010000010001000010011000";
    case '|':return "00100001000010000100001000010000100";
    case '`':return "01000001000001000000000000000000000";
    case '~':return "00000000000110110010000000000000000";
    default:return "00000000000000000000000000000000000";
    }
}
std::string setting_value(SettingId id,double value){
    if(!std::isfinite(value))return "--";
    if(id==SettingId::DesktopPointerPriority)return value==0?"VR HAND":"PC MOUSE";
    if(id==SettingId::MicrophoneEnabled||id==SettingId::FoveatedStreaming)return value==0?"OFF":"ON";
    if(id==SettingId::VrGraphicsQuality&&value==-2)return "CUSTOM";
    if(id==SettingId::VrBitrateMbps&&value==0)return "UNLIMITED";
    if(id==SettingId::VrGraphicsQuality&&value>=-1&&value<=7&&std::floor(value)==value)
        return surfxr_vr_quality_profile(static_cast<int>(value))->name;
    char text[32]{};
    if(id==SettingId::MicrophoneVolume)std::snprintf(text,sizeof(text),"%.0f%%",value*100);
    else if(std::abs(value-std::round(value))<.001)std::snprintf(text,sizeof(text),"%.0f",value);
    else std::snprintf(text,sizeof(text),"%.1f",value);
    return text;
}

} // namespace
void ui_fill(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,unsigned x,unsigned y,unsigned rw,unsigned rh,std::array<std::uint8_t,4> c){
    if(!rgba||stride<std::uint64_t{w}*4||x>=w||y>=h)return;
    for(unsigned py=y;py<y+std::min(rh,h-y);++py)for(unsigned px=x;px<x+std::min(rw,w-x);++px)std::memcpy(rgba+std::size_t{py}*stride+std::size_t{px}*4,c.data(),4);
}
void ui_text(std::uint8_t* rgba,unsigned w,unsigned h,unsigned stride,unsigned x,unsigned y,const std::string& text,unsigned size,std::array<std::uint8_t,4> color){
    if(!size||size>16)return;
    const auto translated=i18n::translate(text);
    if(const auto* arrow=arrow_glyph(translated)){
        for(unsigned row=0;row<7;++row)for(unsigned col=0;col<5;++col)if(arrow[row*5+col]=='1')ui_fill(rgba,w,h,stride,x+col*size,y+row*size,size,size,color);
        return;
    }
    if(localized_text(rgba,w,h,stride,x,y,translated,size,color))return;
    for(char c:translated){if(x>=w)break;if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');const char* pixels=glyph(c);
        for(unsigned row=0;row<7;++row)for(unsigned column=0;column<5;++column)if(pixels[row*5+column]=='1')ui_fill(rgba,w,h,stride,x+column*size,y+row*size,size,size,color);
        x+=6*size;
    }
}
UiTextSize ui_text_size(const std::string& text,unsigned scale) {
    if(!scale||scale>16)return {};
    const auto translated=i18n::translate(text);UiTextSize size;
    if(arrow_glyph(translated))return {5*scale,7*scale};
    if(localized_size(translated,scale,size))return size;
    return {translated.empty()?0u:static_cast<unsigned>(translated.size()*6-1)*scale,7*scale};
}
void ui_text_centered(std::uint8_t* pixels,unsigned w,unsigned h,unsigned stride,unsigned x,unsigned y,unsigned rw,unsigned rh,const std::string& text,unsigned maximum_scale,std::array<std::uint8_t,4> color) {
    if(!pixels||stride<std::uint64_t{w}*4||x>=w||y>=h||!rw||!rh)return;
    rw=std::min(rw,w-x);rh=std::min(rh,h-y);
    unsigned scale=std::clamp(maximum_scale,1u,16u);auto size=ui_text_size(text,scale);
    while(scale>1&&(size.width+4>rw||size.height+4>rh))size=ui_text_size(text,--scale);
    // A sub-rectangle preserves clipping to the button even for a tiny viewport.
    ui_text(pixels+std::size_t(y)*stride+std::size_t(x)*4,rw,rh,stride,
        rw>size.width?(rw-size.width)/2:0,rh>size.height?(rh-size.height)/2:0,text,scale,color);
}
void VrUiModel::raster_keyboard(std::uint8_t* pixels,unsigned w,unsigned h,unsigned stride) const {
    if(settings_visible_){raster_settings(pixels,w,h,stride);return;}
    ui_fill(pixels,w,h,stride,0,0,w,h,{12,20,31,255});
    for(const auto& k:keys_){
        bool hover=false;for(const auto& hit:hits_)hover|=hit.valid&&hit.region==UiHitRegion::Keyboard&&hit.u>=k.x&&hit.u<=k.x+k.width&&hit.v>=k.y&&hit.v<=k.y+k.height;
        bool active=k.command>=6&&k.command<=8&&keyboard_binding_==static_cast<unsigned>(k.command-6);active|=k.modifier&&std::find(held_modifiers_.begin(),held_modifiers_.end(),k.key)!=held_modifiers_.end();if(k.command>=100&&k.command<200){const auto& p=panels_.at(static_cast<std::size_t>(k.command-100));active=p.visible&&p.available;}
        active|=k.command==9&&system_capture_active_;active|=k.command==12&&shortcut_edit_;
        const bool disabled=(k.command==11&&input_preferences_.input_shortcuts.size()>=kMaxKeyboardShortcuts)||(k.command==16&&shortcut_offset_==0)||(k.command==17&&shortcut_offset_+3>=input_preferences_.input_shortcuts.size())||(k.command==13&&!valid_keyboard_shortcut(shortcut_draft_))||(k.command==9&&!system_capture_active_&&!system_capture_available_)||(k.command>=100&&k.command<200&&!capture_status_.supported);
        const auto color=!disabled&&hover?std::array<std::uint8_t,4>{30,144,196,255}:(active?std::array<std::uint8_t,4>{38,98,122,255}:std::array<std::uint8_t,4>{38,49,67,255});
        const auto x=static_cast<unsigned>(k.x*w),y=static_cast<unsigned>(k.y*h),kw=static_cast<unsigned>(k.width*w),kh=static_cast<unsigned>(k.height*h);
        ui_fill(pixels,w,h,stride,x,y,kw,kh,color);auto label=k.label;
        if(shortcut_recording()&&k.command==10)label=shortcut_label(shortcut_draft_);
        if(shortcut_recording()&&k.command==0&&k.key!=UiKey::None){
            const auto code=static_cast<std::uint16_t>(ui_scan_code(k.key));
            if(std::find(shortcut_draft_.begin(),shortcut_draft_.end(),code)!=shortcut_draft_.end())ui_fill(pixels,w,h,stride,x,y,kw,kh,{38,98,122,255});
        }
        if(k.key==UiKey::Shortcut)label=shortcut_label(input_preferences_.input_shortcuts.at(k.command-200));
        if(k.command>=100&&k.command<200&&capture_status_.supported&&panels_.at(k.command-100).source_id<4&&((capture_status_.requested^capture_status_.accepted)&(1u<<panels_.at(k.command-100).source_id)))label=i18n::translate(label)+"...";
        ui_text_centered(pixels,w,h,stride,x,y,kw,kh,label,4,disabled?std::array<std::uint8_t,4>{107,121,139,255}:std::array<std::uint8_t,4>{237,246,255,255});
    }
}
void VrUiModel::raster_reset_confirmation(std::uint8_t* pixels,unsigned w,unsigned h,unsigned stride) const {
    ui_fill(pixels,w,h,stride,0,0,w,h,{12,20,31,255});
    const auto text=[&](float x,float y,float width,float height,const std::string& label,unsigned scale){
        ui_text_centered(pixels,w,h,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(y*h),static_cast<unsigned>(width*w),static_cast<unsigned>(height*h),label,scale,{237,246,255,255});
    };
    text(.08f,.15f,.84f,.12f,"RESET ALL LAYOUT?",5);
    {
        text(.08f,.31f,.84f,.08f,"ALL DISPLAYS: POSITION, SIZE, CURVATURE AND VISIBILITY",3);
        text(.08f,.40f,.84f,.08f,"KEYBOARD: POSITION, SIZE AND ATTACHMENT",3);
        text(.08f,.49f,.84f,.08f,"SHORTCUTS, IMAGE QUALITY AND CONNECTION ARE KEPT",3);
    }
    ui_fill(pixels,w,h,stride,static_cast<unsigned>(.26f*w),static_cast<unsigned>(.65f*h),static_cast<unsigned>(.22f*w),static_cast<unsigned>(.12f*h),{38,70,91,255});
    ui_fill(pixels,w,h,stride,static_cast<unsigned>(.52f*w),static_cast<unsigned>(.65f*h),static_cast<unsigned>(.22f*w),static_cast<unsigned>(.12f*h),{142,55,49,255});
    text(.26f,.65f,.22f,.12f,"CANCEL",4);text(.52f,.65f,.22f,.12f,"CONFIRM RESET",4);
}
void VrUiModel::raster_settings(std::uint8_t* pixels,unsigned w,unsigned h,unsigned stride) const {
    if(pending_reset_==-1){raster_reset_confirmation(pixels,w,h,stride);return;}
    if(!pixels||stride<std::uint64_t{w}*4)return;
    ui_fill(pixels,w,h,stride,0,0,w,h,{12,20,31,255});
    bool body=false;
    const unsigned body_top=static_cast<unsigned>(std::ceil(settings_top*h)),body_bottom=static_cast<unsigned>(settings_bottom*h);
    const auto box=[&](float x,float y,float width,float height,std::array<std::uint8_t,4> color){
        const float screen_y=y-(body?settings_scroll_:0);
        const int first=std::max(body?static_cast<int>(body_top):0,static_cast<int>(screen_y*h));
        const int last=std::min(body?static_cast<int>(body_bottom):static_cast<int>(h),static_cast<int>((screen_y+height)*h));
        if(last>first)ui_fill(pixels,w,h,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(first),static_cast<unsigned>(width*w),static_cast<unsigned>(last-first),color);};
    const auto text=[&](float x,float y,const std::string& label,unsigned size,std::array<std::uint8_t,4> color,float available=0.f){
        if(available>0)while(size>1&&ui_text_size(label,size).width>available*w)--size;
        const int screen_y=static_cast<int>((y-(body?settings_scroll_:0))*h);
        if(body){
            if(screen_y<static_cast<int>(body_top)||screen_y+static_cast<int>(ui_text_size(label,size).height)>static_cast<int>(body_bottom))return;
            ui_text(pixels+std::size_t{body_top}*stride,w,body_bottom-body_top,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(screen_y)-body_top,label,size,color);
        }else if(screen_y>=0)ui_text(pixels,w,h,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(screen_y),label,size,color);};
    const auto button=[&](float x,float y,float width,float height,const std::string& label,bool enabled,bool selected=false){
        bool hover=false;for(const auto& hit:hits_)hover|=hit.valid&&hit.region==UiHitRegion::Keyboard&&(!body||(hit.v>=settings_top&&hit.v<settings_bottom))&&in_rect(hit.u,hit.v+(body?settings_scroll_:0),x,y,width,height);
        box(x,y,width,height,selected?std::array<std::uint8_t,4>{31,87,81,255}:!enabled?std::array<std::uint8_t,4>{31,39,50,255}:(hover?std::array<std::uint8_t,4>{30,144,196,255}:std::array<std::uint8_t,4>{38,70,91,255}));
        const int screen_y=static_cast<int>((y-(body?settings_scroll_:0))*h);
        if(screen_y<0)return;
        const unsigned top=body?body_top:0,bottom=body?body_bottom:h;
        if(static_cast<unsigned>(screen_y)<top||static_cast<unsigned>(screen_y)+height*h>bottom)return;
        ui_text_centered(pixels+std::size_t(top)*stride,w,bottom-top,stride,static_cast<unsigned>(x*w),
            static_cast<unsigned>(screen_y)-top,static_cast<unsigned>(width*w),static_cast<unsigned>(height*h),label,4,
            (enabled||selected)?std::array<std::uint8_t,4>{239,247,255,255}:std::array<std::uint8_t,4>{122,139,151,255});
    };
    const unsigned title_scale=std::clamp(w/500,1u,4u),label_scale=std::clamp(w/600,1u,3u),value_scale=std::clamp(w/800,1u,2u);
    text(.025f,.045f,"SETTINGS",title_scale,{235,248,255,255});
    for(unsigned i=0;i<tab_names.size();++i){
        const bool selected=settings_page_==static_cast<UiSettingsPage>(i);
        button(tab_x+tab_step*i,.025f,tab_width,.10f,tab_names[i],true,selected);
        if(selected)box(tab_x+tab_step*i,.116f,tab_width,.006f,{92,222,179,255});
    }
    button(.70f,.025f,.275f,.10f,"CLOSE SETTINGS",true);
    if(settings_page_==UiSettingsPage::Image){
        text(.025f,.135f,"IMAGE PROCESSING ON THIS FRAME",label_scale,{137,204,229,255});
        body=true;
        for(const auto& row:image_quality_rows()){
            const int value=image_quality_value(image_quality_,row.id);
            const char* label=row.id==ImageQualityId::DesktopFilter?"DESKTOP FILTER":row.id==ImageQualityId::VrTimewarp?"TIMEWARP":"VR SPATIAL UPSCALING";
            const std::string info=row.id==ImageQualityId::DesktopFilter?(value?"STABLE QUALITY":"OFF / BILINEAR"):(value?"ON":"OFF");
            const char* note=row.id==ImageQualityId::DesktopFilter?"REDUCES SHIMMER ON DISTANT TEXT":row.id==ImageQualityId::VrTimewarp?"ROTATION CORRECTION FOR REPEATED SURFXR FRAMES":"USED ONLY WHEN THE IMAGE NEEDS ENLARGING";
            box(row.x,row.y,row.width,row.height,{25,37,52,255});
            text(row.x+.012f,row.y+.014f,label,label_scale,{228,238,248,255});
            text(row.x+.012f,row.y+.054f,note,value_scale,{165,194,216,255});
            text(row.x+.56f,row.y+.035f,info,label_scale,{125,226,169,255});
            button(row.x+row.width*.745f,row.y+.012f,row.width*.115f,row.height-.024f,"OFF",value>0,value==0);
            button(row.x+row.width*.88f,row.y+.012f,row.width*.105f,row.height-.024f,"ON",value<1,value!=0);
        }
    }else{
    text(.025f,.145f,settings_page_==UiSettingsPage::Stream?"DESKTOP":settings_page_==UiSettingsPage::Devices?"INPUT & MICROPHONE":diagnostics_page_==UiDiagnosticsPage::Overlays?"DIAGNOSTIC OVERLAYS":"MINI HUD",label_scale,{137,204,229,255});
    text(.515f,.145f,settings_page_==UiSettingsPage::Stream?"STEAMVR":settings_page_==UiSettingsPage::Devices?"DEVICE SYNC":"",label_scale,{137,204,229,255});
    body=true;
    for(const auto& row:settings_rows()){
        if(!setting_visible(settings_page_,row.id))continue;
        const auto& value=remote_settings_.at(row.id);const auto choices=setting_choices(row.id,remote_settings_);const bool enabled=!choices.empty();
        box(row.x,row.y,row.width,row.height,{25,37,52,255});
        const float label_width=row.width*.72f;
        text(row.x+.012f,row.y+.015f,setting_name(row.id),label_scale,enabled?std::array<std::uint8_t,4>{228,238,248,255}:std::array<std::uint8_t,4>{135,152,167,255},label_width);
        const bool bitrate=row.id==SettingId::VrBitrateMbps;
        const auto color=value.pending&&!bitrate?std::array<std::uint8_t,4>{255,203,115,255}:std::array<std::uint8_t,4>{165,194,216,255};
        // This is the selected limit, not an assertion about measured traffic.
        std::string info=setting_value(row.id,value.requested);
        if(value.pending&&!bitrate)info+=" / APPLYING";
        text(row.x+.012f,row.y+.047f,info,value_scale,color,label_width);
        if(row.id==SettingId::VrGraphicsQuality){
            const auto* profile=surfxr_vr_quality_profile(static_cast<int>(value.requested));
            std::string dimensions="BASE "+(profile?std::to_string(profile->render_width)+"X"+std::to_string(profile->render_height):"--")+"  OBS ";
            const auto& observed=remote_settings_.vr_dimensions;
            dimensions+=observed?std::to_string(observed->render_width)+"X"+std::to_string(observed->render_height):"--";
            text(row.x+.012f,row.y+.084f,dimensions,value_scale,color);
        }else if(row.id==SettingId::VrFramerate)text(row.x+.012f,row.y+.084f,remote_settings_.capabilities.surfxr_video_controls?"SAVED / RESTART STEAMVR MANUALLY":"NEW HZ MAY REQUIRE STEAMVR RESTART",value_scale,color);
        else if(row.id==SettingId::DesktopPointerPriority)text(row.x+.012f,row.y+.084f,"WHEN BOTH POINTERS MOVE",value_scale,color);
        else if(bitrate)text(row.x+.012f,row.y+.084f,"STREAMING BITRATE LIMIT",value_scale,color);
        else text(row.x+.012f,row.y+.084f,value.pending?"WAITING FOR PC CONFIRMATION":(value.actual?"CONFIRMED BY PC":"WAITING FOR PC STATUS"),value_scale,color);
        const bool toggle=row.id==SettingId::MicrophoneEnabled||row.id==SettingId::FoveatedStreaming;
        button(row.x+row.width*.745f,row.y+.012f,row.width*.115f,row.height-.024f,toggle?"OFF":"-",enabled&&value.requested>choices.front(),toggle&&value.requested==0);
        button(row.x+row.width*.88f,row.y+.012f,row.width*.105f,row.height-.024f,toggle?"ON":"+",enabled&&value.requested<choices.back(),toggle&&value.requested!=0);
    }
    const auto& c=remote_settings_.capabilities;
    if(settings_page_==UiSettingsPage::Stream){
    box(monitors_x,monitors_y,monitors_width,controllers_height,{25,37,52,255});
    text(monitors_x+.012f,monitors_y+.015f,"PC DISPLAYS",label_scale,{228,238,248,255});
    const auto monitor_count=c.desktop_monitor_count?std::to_string(c.desktop_monitor_count):"--";
    text(monitors_x+.012f,monitors_y+.047f,"OBS "+monitor_count+"  "+(c.monitor_pending?"WAITING FOR PC":c.vr_requested?"SHOW DESKTOP TO CHANGE":"STREAMER DISPLAY COUNT"),value_scale,{165,194,216,255},monitors_width*.72f);
    text(monitors_x+.012f,monitors_y+.084f,c.monitor_timed_out?"NO PC DISPLAY CHANGE - RETRY":"+ ADD DISPLAY / - REMOVE DISPLAY",value_scale,c.monitor_timed_out?std::array<std::uint8_t,4>{255,203,115,255}:std::array<std::uint8_t,4>{165,194,216,255},monitors_width*.72f);
    const auto monitor_enabled=monitor_actions_enabled(c);
    button(monitors_x+monitors_width*.745f,monitors_y+.012f,monitors_width*.115f,controllers_height-.024f,"-",monitor_enabled[0]);
    button(monitors_x+monitors_width*.88f,monitors_y+.012f,monitors_width*.105f,controllers_height-.024f,"+",monitor_enabled[1]);
    }
    for(const auto& row:preference_rows){
        if(!preference_visible(row,settings_page_,diagnostics_page_))continue;
        const int value=input_preference_value(input_preferences_,row.id);
        const auto& spec=*input_preference_spec(row.id);
        const bool battery=row.id==InputPreferenceId::SyncRawBattery;
        box(row.x,row.y,monitors_width,controllers_height,{25,37,52,255});
        text(row.x+.012f,row.y+.015f,row.label,label_scale,{228,238,248,255});
        text(row.x+.012f,row.y+.054f,row.note,value_scale,{165,194,216,255},monitors_width*.72f);
        if(battery)text(row.x+.012f,row.y+.087f,value?"ACTUAL BATTERY":"HEADSET DISPLAY LEVEL",value_scale,{125,226,169,255});
        if(!spec.boolean)text(row.x+.012f,row.y+.087f,std::to_string(value)+"%",value_scale,{125,226,169,255});
        button(row.x+monitors_width*.745f,row.y+.012f,monitors_width*.115f,controllers_height-.024f,battery?"DISPLAY":spec.boolean?"OFF":"-",value>spec.minimum,spec.boolean&&value==0);
        button(row.x+monitors_width*.88f,row.y+.012f,monitors_width*.105f,controllers_height-.024f,battery?"ACTUAL":spec.boolean?"ON":"+",value<spec.maximum,spec.boolean&&value!=0);
        if(spec.boolean)box(row.x+monitors_width*(value?.88f:.745f),row.y+controllers_height-.018f,monitors_width*(value?.105f:.115f),.006f,{92,222,179,255});
    }
    if(settings_page_==UiSettingsPage::Diagnostics){
        if(diagnostics_page_==UiDiagnosticsPage::Gaze){
            text(.025f,.39f,"GAZE TRACKING REQUIRED. TURN GAZE TO SHOW OFF FOR AN ALWAYS VISIBLE HUD.",value_scale,{165,194,216,255},.93f);
            text(.025f,.44f,"THE MARKER SHOWS WHERE TO LOOK WHILE THE HUD IS HIDDEN.",value_scale,{165,194,216,255},.93f);
        }
        for(unsigned i=0;i<diagnostics_tab_names.size();++i)
            button(.025f+diagnostics_tab_step*i,diagnostics_tab_y,diagnostics_tab_width,diagnostics_tab_height,diagnostics_tab_names[i],true,diagnostics_page_==static_cast<UiDiagnosticsPage>(i));
    }
    if(settings_page_==UiSettingsPage::Devices){
    box(monitors_x,system_capture_y,monitors_width,controllers_height,{25,37,52,255});
    text(monitors_x+.012f,system_capture_y+.015f,"CONTROLLER TAKEOVER",label_scale,{228,238,248,255});
    text(monitors_x+.012f,system_capture_y+.047f,system_capture_active_?"ON - SYSTEM KEYS TO PC":system_capture_available_?"OFF - SYSTEM KEYS STAY LOCAL":"PC FRAME MODE / INPUT BRIDGE REQUIRED",value_scale,{165,194,216,255},monitors_width*.72f);
    text(monitors_x+.012f,system_capture_y+.084f,"4 SYSTEM CLICKS EXIT; SHORTCUTS KEPT",value_scale,{165,194,216,255},monitors_width*.72f);
    button(monitors_x+monitors_width*.745f,system_capture_y+.012f,monitors_width*.115f,controllers_height-.024f,"OFF",system_capture_active_,system_capture_available_&&!system_capture_active_);
    button(monitors_x+monitors_width*.88f,system_capture_y+.012f,monitors_width*.105f,controllers_height-.024f,"ON",system_capture_available_&&!system_capture_active_,system_capture_active_);
    }
    }
    body=false;
    const auto& c=remote_settings_.capabilities;
    const auto enabled=menu_actions_enabled(c);
    const std::array<std::string,4> labels{"START STEAMVR",c.vr_requested&&!c.vr_active?"CANCEL VR":"SHOW DESKTOP","EXIT STEAMVR","RECENTER"};
    for(unsigned i=0;i<4;++i)button(.025f+.245f*i,.815f,.225f,.10f,labels[i],enabled[i]);
    const auto status=c.vr_status==VrMenuStatus::Idle?(c.vr_active?"VR: STREAMING":(c.vr_ready?"VR: READY":"VR: NOT READY")):vr_menu_status_text(c.vr_status);
    text(.025f,.945f,status,value_scale,c.vr_status==VrMenuStatus::Idle?std::array<std::uint8_t,4>{125,226,169,255}:std::array<std::uint8_t,4>{255,203,115,255});
    const char* note=settings_page_==UiSettingsPage::Stream?"QUALITY / FPS CHANGES MAY REQUIRE A STEAMVR RESTART.":
      settings_page_==UiSettingsPage::Image?"IMAGE EFFECTS APPLY ON THIS FRAME.":
      settings_page_==UiSettingsPage::Devices?"LOCAL MENUS REMAIN AVAILABLE WHEN PC CONTROLLERS ARE OFF.":"OPTIONAL OVERLAYS FOR TROUBLESHOOTING.";
    text(.30f,.945f,note,value_scale,{165,194,216,255},.43f);
    button(.755f,.925f,.22f,.065f,"RESET ALL LAYOUT",true);

}
void VrUiModel::raster_controls(std::uint32_t id,bool top,std::uint8_t* pixels,unsigned w,unsigned h,unsigned stride) const {
    ui_fill(pixels,w,h,stride,0,0,w,h,{0,0,0,0});
    if(id>=panels_.size()||!pixels||stride<std::uint64_t{w}*4)return;
    const auto controls=panel_controls(id);const auto& strip=top?controls.top:controls.bottom;
    const auto text=[&](float x,float y,const std::string& label,unsigned size){ui_text(pixels,w,h,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(y*h),label,size,{242,248,255,255});};
    if(pending_reset_==static_cast<int>(id)){
        if(top)return;
        const auto box=[&](float x,float y,float width,float height,std::array<std::uint8_t,4> color){ui_fill(pixels,w,h,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(y*h),static_cast<unsigned>(width*w),static_cast<unsigned>(height*h),color);};
        const auto label=[&](float x,float y,float width,float height,const std::string& value){ui_text_centered(pixels,w,h,stride,static_cast<unsigned>(x*w),static_cast<unsigned>(y*h),static_cast<unsigned>(width*w),static_cast<unsigned>(height*h),value,3,{242,248,255,255});};
        box(.012f,.01f,.49f,.68f,{20,30,47,250});
        label(.02f,.02f,.47f,.22f,i18n::translate("RESET DISPLAY")+": "+std::to_string(id+1)+"?");
        for(unsigned i=0;i<2;++i){const float x=i?.26f:.025f,width=i?.225f:.21f;bool hover=false;
            for(const auto& hit:hits_)hover|=hit.valid&&hit.panel_id==id&&hit.region==UiHitRegion::BottomControls&&in_rect(hit.u,hit.v,x,.29f,width,.34f);
            box(x,.29f,width,.34f,hover?std::array<std::uint8_t,4>{30,144,196,255}:i?std::array<std::uint8_t,4>{142,55,49,255}:std::array<std::uint8_t,4>{38,70,91,255});
            label(x,.29f,width,.34f,i?"CONFIRM RESET":"CANCEL");
        }
        return;
    }
    if(top){
        for(unsigned i=0;i<2;++i){const float x=.025f+.5f*i;
            ui_fill(pixels,w,h,stride,static_cast<unsigned>(x*w),0,static_cast<unsigned>(.45f*w),h,{20,30,47,238});
            const float value=i==0?(panels_[id].width-kUiMinPanelWidthM)/(kUiMaxPanelWidthM-kUiMinPanelWidthM):panels_[id].curvature_radians/1.8f;
            ui_fill(pixels,w,h,stride,static_cast<unsigned>((x+.025f)*w),static_cast<unsigned>(.67f*h),static_cast<unsigned>(.40f*w),static_cast<unsigned>(.13f*h),{57,75,94,255});
            ui_fill(pixels,w,h,stride,static_cast<unsigned>((x+.025f)*w),static_cast<unsigned>(.67f*h),static_cast<unsigned>(.40f*w*std::clamp(value,0.f,1.f)),static_cast<unsigned>(.13f*h),{32,191,219,255});
            text(x+.025f,.16f,i==0?"SIZE":"CURVE",std::clamp(h/28,1u,4u));
        }
        return;
    }
    const float centers[]={.12f,.375f,.625f,.88f};const char* labels[]={"RESET","KEYBOARD","CLOSE","SETTINGS"};
    for(unsigned i=0;i<4;++i){bool hover=false;
        for(const auto& hit:hits_)if(hit.valid&&hit.panel_id==id&&hit.region==UiHitRegion::BottomControls){const float x=(hit.u-centers[i])*strip.width,y=(hit.v-.31f)*strip.height;hover|=x*x+y*y<=.09f*.09f;}
        const auto color=hover?std::array<std::uint8_t,4>{30,155,193,255}:std::array<std::uint8_t,4>{32,49,70,245};
        for(unsigned y=0;y<h;++y){const float dy=((y+.5f)/h-.31f)*strip.height;if(std::abs(dy)>.09f)continue;
            for(unsigned x=0;x<w;++x){const float dx=((x+.5f)/w-centers[i])*strip.width;if(dx*dx+dy*dy<=.09f*.09f)std::memcpy(pixels+std::size_t{y}*stride+std::size_t{x}*4,color.data(),4);}}
        const unsigned bw=static_cast<unsigned>(.16f*w/strip.width),bh=std::max(1u,std::min(h/2,32u));
        const unsigned cx=static_cast<unsigned>(centers[i]*w),cy=static_cast<unsigned>(.31f*h);
        ui_text_centered(pixels,w,h,stride,cx>bw/2?cx-bw/2:0,cy>bh/2?cy-bh/2:0,bw,bh,labels[i],3,{245,249,255,255});
    }
    ui_fill(pixels,w,h,stride,static_cast<unsigned>(.12f*w),static_cast<unsigned>(.76f*h),static_cast<unsigned>(.76f*w),static_cast<unsigned>(.22f*h),{30,45,62,245});
    text(.21f,.80f,"MOVE  -  GRIP OR TRIGGER",std::clamp(h/50,1u,3u));
}
} // namespace surfxr
