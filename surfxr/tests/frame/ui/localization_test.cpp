#include "surfxr/localization.hpp"
#include "surfxr/vr_ui.hpp"
#include "surfxr/startup_hud.hpp"
#include "surfxr/keyboard_hints.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdlib>
#ifdef SURFXR_FRAME_TEXT
#include <pango/pangocairo.h>
#endif

int main(int argc,char** argv) {
    using namespace surfxr;
    using namespace surfxr::i18n;
    const auto require=[](bool ok){if(!ok)throw std::runtime_error("localization_check_failed");};
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("surfxr-language-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(root));
    struct Cleanup{fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
#ifdef _WIN32
    _putenv_s("XDG_CONFIG_HOME",root.string().c_str());
#else
    setenv("XDG_CONFIG_HOME",root.c_str(),1);
#endif
    for(const auto* locale:{"zh","zh_CN.UTF-8","zh-Hans","ZH-tw"})
        require(language_from_locale(locale)==Language::Chinese);
    for(const auto* locale:{"","en_US.UTF-8","fr-FR","C","POSIX","zhong"})
        require(language_from_locale(locale)==Language::English);
    const bool chinese=!(argc>1&&std::string(argv[1])=="--english");
    require(steamvr_language_from_log("old Web language is english\nnew Web language is schinese\n")=="schinese");
    require(language_from_locale("schinese")==Language::Chinese);
    require(language_from_locale("tchinese")==Language::Chinese);
    require(steamvr_language_from_log("Web language is ../../private\n").empty());
#ifdef SURFXR_FRAME_TEXT
    fs::create_directories(root/"openvr");
    {std::ofstream paths(root/"openvr/openvrpaths.vrpath");paths<<"{\"log\":[\""<<root.generic_string()<<"\"]}";}
    {std::ofstream log(root/"vrserver.txt");log<<"Web language is "<<(chinese?"schinese":"german")<<'\n';}
    select_language("auto"); // Linux language environment cannot override SteamVR.
#else
    select_language(chinese?"zh-CN":"en");
#endif
    require(system_language()==(chinese?Language::Chinese:Language::English));
    require(translate("Desktop")== (chinese?"桌面":"Desktop"));
    require(translate("Dynamic Foveated Streaming")== (chinese?"动态注视点串流":"Dynamic Foveated Streaming"));
    require(translate("IME MODE")== (chinese?"中文 / 英文":"IME MODE"));
    for(const auto text:{"SAVE","CANCEL","EDIT","DELETE","CLEAR","CLOSE SETTINGS","RESET ALL LAYOUT?","RESET DISPLAY","ALL DISPLAYS: POSITION, SIZE, CURVATURE AND VISIBILITY","KEYBOARD: POSITION, SIZE AND ATTACHMENT","SHORTCUTS, IMAGE QUALITY AND CONNECTION ARE KEPT","CONFIRM RESET","EDIT: SELECT A SHORTCUT","SHORTCUTS: STICK TO SCROLL | TRIGGER TO RUN","SELECT KEYS","RECORDING: SELECT KEYS, THEN SAVE"})require((translate(text)!=text)==chinese);
    require(translate("REC")== (chinese?"录制":"REC"));
    require(translate("INPUT METHOD")== (chinese?"切换输入法":"INPUT METHOD"));
    for(const auto* hint:{keyboard_controller_hint(true,true),keyboard_controller_hint(false,false),keyboard_controller_hint(true,false),"GRIP: MOVE / TILT","STICK: DEPTH / SIZE"})
        require(!chinese||translate(hint)!=hint);
    if(chinese)for(const auto* word:{"TRIGGER","BUMPER","TYPE","GRIP","STICK","DISTANCE","SIZE"})require(translate(keyboard_controller_hint(true,true)).find(word)==std::string::npos);
    for(const auto* arrow:{"↑","↓","←","→"}){
        std::vector<std::uint8_t> pixels(32*32*4);ui_text(pixels.data(),32,32,128,0,0,arrow,3,{255,255,255,255});
        require(std::any_of(pixels.begin(),pixels.end(),[](auto v){return v!=0;}));
        require(ui_text_size(arrow,3).width==15&&ui_text_size(arrow,3).height==21);
    }
    require(translate("FRAME RATE")== (chinese?"帧率":"FRAME RATE"));
    require(translate("CAPTURE 1")== (chinese?"采集屏幕 1":"CAPTURE 1"));
    require(translate("TAKE OVER")== (chinese?"接管控制":"TAKE OVER"));
    require(translate("DASHBOARD")== (chinese?"控制面板":"DASHBOARD"));
    require(translate("user-headset.local")=="user-headset.local");
    require(translate("192.0.2.5")=="192.0.2.5");
    require(translate("unknown diagnostic: 0x1234")=="unknown diagnostic: 0x1234");
    require(translate("Timewarp | Desktop")== (chinese?"旋转校正 | 桌面":"Timewarp | Desktop"));
    select_language("en");require(system_language()==Language::English);
    select_language("zh-CN");require(system_language()==Language::Chinese);
    select_language(chinese?"zh-CN":"en");
    require(translate("Switching to SteamVR")== (chinese?"正在切换到 SteamVR":"Switching to SteamVR"));
    require(translate("Switching to desktop")== (chinese?"正在切换到桌面":"Switching to desktop"));
    require(translate("Right Menu: hide")== (chinese?"右侧菜单键：隐藏":"Right Menu: hide"));
#ifndef SURFXR_FRAME_TEXT
    select_language("en"); // Windows runs the portable bitmap path; CJK is checked on Frame/Linux.
#endif
    for(const auto* label:{"Settings","Close","VR hand","ON","OFF","Keyboard","IN SPACE","LEFT HAND","RIGHT HAND","CAPTURE 2","TAKE OVER","DASHBOARD"}) {
        constexpr unsigned w=240,h=96,stride=w*4;
        std::vector<std::uint8_t> pixels(stride*h,0);
        ui_text_centered(pixels.data(),w,h,stride,20,16,200,64,label,3,{255,255,255,255});
        unsigned left=w,right=0,top=h,bottom=0;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)if(pixels[y*stride+x*4+3]){
            require(x>=20&&x<220&&y>=16&&y<80);
            left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
        }
        require(left<=right&&top<=bottom);
        require(std::abs(static_cast<int>(left+right)-239)<=1);
        require(std::abs(static_cast<int>(top+bottom)-95)<=1);
    }
    select_language(chinese?"zh-CN":"en");
    if(argc==3&&(std::string(argv[1])=="--preview-ppm"||std::string(argv[1])=="--keyboard-preview-ppm"||std::string(argv[1])=="--shortcut-preview-ppm"||std::string(argv[1])=="--reset-preview-ppm"||std::string(argv[1])=="--display-reset-preview-ppm")) {
        constexpr unsigned w=2048,h=928,stride=w*4;
        std::vector<std::uint8_t> pixels(stride*h,0);VrUiModel model;std::vector<UiPanel> panels(4);for(unsigned i=0;i<panels.size();++i)panels[i].source_id=i;model.set_panels(std::move(panels));
        model.set_desktop_capture_status({true,15,15});model.set_system_capture_status(true,true);
        model.show_settings(std::string(argv[1])=="--preview-ppm");
        if(std::string(argv[1])=="--shortcut-preview-ppm"){
            model.reset_anchor({});model.show_settings(true);model.show_settings(false);
            // Exercise the same hit/activation path as the headset controller.
            auto layout=model.export_layout();layout.keyboard_visible=true;require(model.restore_layout(layout));
            std::array<UiHand,2> hands{};hands[0].active=true;
            const auto click=[&](float u,float v){
                const auto point=ui_panel_point(model.keyboard_panel(),u,v);
                hands[0].aim={point,model.keyboard_panel().pose.orientation};hands[0].aim.position.z+=.3f;
                hands[0].select=false;model.update(hands,.02f,{});hands[0].select=true;model.update(hands,.02f,{});hands[0].select=false;model.update(hands,.02f,{});
            };
            click(.92f,.95f);require(model.shortcut_recording());
            for(const auto label:{"LCTRL","SPACE"}){const auto key=*std::find_if(model.keys().begin(),model.keys().end(),[&](const auto& k){return k.label==label;});click(key.x+key.width/2,key.y+key.height/2);}
        }
        if(std::string(argv[1])=="--reset-preview-ppm"||std::string(argv[1])=="--display-reset-preview-ppm"){
            model.reset_anchor({});std::array<UiHand,2> hands{};hands[0].active=true;
            if(std::string(argv[1])=="--reset-preview-ppm"){
                model.show_settings(true);const auto point=ui_panel_point(model.keyboard_panel(),.86f,.95f);hands[0].aim={point,model.keyboard_panel().pose.orientation};hands[0].aim.position.z+=.3f;
            }else{
                auto panel=model.panel_controls(0).bottom;panel.visible=true;const auto point=ui_panel_point(panel,.12f,.31f);hands[0].aim={point,panel.pose.orientation};hands[0].aim.position.z+=.3f;
            }
            model.update(hands,.02f,{});hands[0].select=true;model.update(hands,.02f,{});hands[0].select=false;model.update(hands,.02f,{});require(model.settings_visible()==(std::string(argv[1])=="--reset-preview-ppm"));
        }
        model.raster_keyboard(pixels.data(),w,768,stride);
        if(!model.settings_visible())ui_text(pixels.data(),w,768,stride,24,79,model.shortcut_hint()?model.shortcut_hint():keyboard_controller_hint(true,true),3,{255,238,223,255});
        if(std::string(argv[1])=="--display-reset-preview-ppm"){
            std::fill(pixels.begin(),pixels.end(),0);model.raster_controls(0,false,pixels.data(),768,170,stride);
        }
        for(unsigned i=0;i<2;++i){const auto hud=StartupHud::raster(i!=0,i+1,i*3);
            for(unsigned y=0;y<hud.height;++y)std::copy_n(hud.rgba.data()+y*hud.stride,hud.stride,
                pixels.data()+(y+788)*stride+(i*500+16)*4);}
        std::ofstream image(argv[2],std::ios::binary);image<<"P6\n"<<w<<' '<<h<<"\n255\n";
        for(unsigned i=0;i<w*h;++i)image.write(reinterpret_cast<const char*>(pixels.data()+i*4),3);
        require(bool(image));
    }
#ifdef SURFXR_FRAME_TEXT
    require(initialize_fonts());
    auto* fontmap=pango_cairo_font_map_get_default();
    auto* context=pango_font_map_create_context(fontmap);
    auto* layout=pango_layout_new(context);
    auto* font=pango_font_description_from_string("SurfXR UI Sans 20");
    pango_layout_set_font_description(layout,font);
    pango_layout_set_text(layout,"桌面帧率中文",-1);
    require(pango_layout_get_unknown_glyphs_count(layout)==0);
    pango_font_description_free(font);g_object_unref(layout);g_object_unref(context);
    constexpr unsigned width=320,height=48,stride=width*4;
    std::vector<std::uint8_t> pixels(stride*height,0);
    ui_text(pixels.data(),width,height,stride,0,0,"Desktop",3,{255,255,255,255});
    require(std::any_of(pixels.begin(),pixels.end(),[](auto v){return v!=0;}));
    const auto first=pixels;
    std::fill(pixels.begin(),pixels.end(),0);
    ui_text(pixels.data(),width,height,stride,0,0,"Desktop",3,{255,255,255,255});
    require(pixels==first); // Cached glyphs preserve pixels and alpha.
#endif
    std::cout<<"Localization and raster checks passed\n";
}
