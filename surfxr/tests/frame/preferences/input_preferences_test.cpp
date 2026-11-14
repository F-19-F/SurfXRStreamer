#include "surfxr/input_preferences.hpp"
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
int main(){
    using namespace surfxr;
    const auto require=[](bool value){if(!value)throw std::runtime_error("input_preferences_test_failed");};
    InputPreferences preferences;require(preferences.vr_controllers_enabled);
    require(apply_input_preference_change(preferences,{InputPreferenceId::VrControllersEnabled,0}));
    require(!preferences.vr_controllers_enabled);
    for(const auto change:{InputPreferenceChange{InputPreferenceId::VrControllersEnabled,-1},
                           {InputPreferenceId::VrControllersEnabled,2},{static_cast<InputPreferenceId>(16),0}})
        require(!apply_input_preference_change(preferences,change)&&!preferences.vr_controllers_enabled);
    require(!preferences.performance_overlay);
    require(apply_input_preference_change(preferences,{InputPreferenceId::PerformanceOverlay,1})&&preferences.performance_overlay&&!preferences.vr_controllers_enabled);
    InputPreferences legacy;require(parse_input_preferences("SURFXR_FRAME_INPUT_PREFERENCES 1\nVR_CONTROLLERS_ENABLED 0\nEND\n",legacy)&&!legacy.performance_overlay&&!legacy.vr_controllers_enabled);
    require(!legacy.gaze_debug_overlay&&parse_input_preferences("SURFXR_FRAME_INPUT_PREFERENCES 2\nVR_CONTROLLERS_ENABLED 0\nPERFORMANCE_OVERLAY 1\nEND\n",legacy)&&legacy.performance_overlay&&!legacy.gaze_debug_overlay);
    require(apply_input_preference_change(preferences,{InputPreferenceId::GazeDebugOverlay,1})&&preferences.gaze_debug_overlay&&preferences.performance_overlay&&!preferences.vr_controllers_enabled);
    require(preferences.boundary_sync_enabled&&legacy.boundary_sync_enabled);
    require(parse_input_preferences("SURFXR_FRAME_INPUT_PREFERENCES 3\nVR_CONTROLLERS_ENABLED 1\nPERFORMANCE_OVERLAY 0\nGAZE_DEBUG_OVERLAY 1\nEND\n",legacy)&&legacy.boundary_sync_enabled);
    require(apply_input_preference_change(preferences,{InputPreferenceId::BoundarySyncEnabled,0})&&!preferences.boundary_sync_enabled);
    require(!preferences.sync_raw_battery&&!legacy.sync_raw_battery);
    require(parse_input_preferences("SURFXR_FRAME_INPUT_PREFERENCES 4\nVR_CONTROLLERS_ENABLED 0\nPERFORMANCE_OVERLAY 1\nGAZE_DEBUG_OVERLAY 1\nBOUNDARY_SYNC_ENABLED 0\nEND\n",legacy)&&!legacy.sync_raw_battery&&!legacy.boundary_sync_enabled&&legacy.performance_overlay);
    require(apply_input_preference_change(preferences,{InputPreferenceId::SyncRawBattery,1})&&preferences.sync_raw_battery);
    require(!preferences.mini_hud_enabled&&preferences.mini_hud_fps&&preferences.mini_hud_latency&&
        !preferences.mini_hud_frame_time&&!preferences.mini_hud_decode&&!preferences.mini_hud_gaze_only&&
        preferences.mini_hud_gaze_marker&&preferences.mini_hud_x==80&&preferences.mini_hud_y==20&&preferences.mini_hud_scale==100);
    // Every historical version retains its prior settings and gets the new defaults,
    // even when the caller's output already contains customized mini HUD values.
    const std::string old_rows[]{"VR_CONTROLLERS_ENABLED 0\n","PERFORMANCE_OVERLAY 1\n","GAZE_DEBUG_OVERLAY 1\n",
        "BOUNDARY_SYNC_ENABLED 0\n","SYNC_RAW_BATTERY 1\n"};
    for(unsigned version=1;version<=5;++version){
        std::string text="SURFXR_FRAME_INPUT_PREFERENCES "+std::to_string(version)+"\n";
        for(unsigned i=0;i<version;++i)text+=old_rows[i];
        legacy.mini_hud_enabled=true;legacy.mini_hud_fps=false;legacy.mini_hud_x=5;legacy.mini_hud_scale=200;
        require(parse_input_preferences(text+"END\n",legacy)&&!legacy.vr_controllers_enabled);
        for(unsigned i=5;i<kInputPreferenceCount;++i){const auto id=static_cast<InputPreferenceId>(i);
            require(input_preference_value(legacy,id)==input_preference_value(InputPreferences{},id));}
        require(legacy.performance_overlay==(version>=2)&&legacy.gaze_debug_overlay==(version>=3)&&
            legacy.boundary_sync_enabled==(version<4)&&legacy.sync_raw_battery==(version>=5));
    }
    for(unsigned i=5;i<kInputPreferenceCount;++i){
        const auto id=static_cast<InputPreferenceId>(i);const auto* spec=input_preference_spec(id);require(spec!=nullptr);
        require(apply_input_preference_change(preferences,{id,spec->minimum})&&input_preference_value(preferences,id)==spec->minimum);
        require(apply_input_preference_change(preferences,{id,spec->maximum})&&input_preference_value(preferences,id)==spec->maximum);
        const auto saved=preferences;
        require(!apply_input_preference_change(preferences,{id,spec->minimum-1})&&preferences==saved);
        require(!apply_input_preference_change(preferences,{id,spec->maximum+1})&&preferences==saved);
    }
    preferences.mini_hud_x=23;preferences.mini_hud_y=67;preferences.mini_hud_scale=135;
    const auto encoded=encode_input_preferences(preferences);InputPreferences parsed;
    require(encoded.find("SURFXR_FRAME_INPUT_PREFERENCES 9\n")==0&&encoded.size()<=1024);
    require(parse_input_preferences(encoded,parsed)&&parsed==preferences);
    auto v7=encoded;v7.replace(v7.find("PREFERENCES 9"),13,"PREFERENCES 7");
    v7.erase(v7.find("SHORTCUTS"),v7.find("END")-v7.find("SHORTCUTS"));
    parsed.input_shortcuts[0]={0x1d,0x39};require(parse_input_preferences(v7,parsed)&&parsed.input_shortcuts==kDefaultInputShortcuts);
    auto v6=v7;v6.replace(v6.find("PREFERENCES 7"),13,"PREFERENCES 6");
    v6.erase(v6.find("TRANSITION_HUD_ENABLED"),std::string("TRANSITION_HUD_ENABLED 1\n").size());
    parsed.transition_hud_enabled=false;
    require(parse_input_preferences(v6,parsed)&&parsed==preferences&&parsed.transition_hud_enabled);
    auto missing=encoded;missing.erase(missing.find("TRANSITION_HUD_ENABLED"),std::string("TRANSITION_HUD_ENABLED 1\n").size());
    require(!parse_input_preferences(missing,parsed));
    auto disabled_hud=preferences;disabled_hud.transition_hud_enabled=false;
    require(parse_input_preferences(encode_input_preferences(disabled_hud),parsed)&&parsed==disabled_hud);
    require(parse_input_preferences(encoded,parsed)&&parsed==preferences);
    const auto historical=std::string({char(0x56),char(0x44)})+encoded.substr(std::string("SURFXR").size());
    require(parse_input_preferences(historical,parsed));
    for(const auto& invalid:std::array<std::pair<const char*,const char*>,4>{{
        {"MINI_HUD_X 23","MINI_HUD_X 101"},{"MINI_HUD_Y 67","MINI_HUD_Y -1"},
        {"MINI_HUD_SCALE 135","MINI_HUD_SCALE 49"},{"MINI_HUD_ENABLED 1","MINI_HUD_ENABLED 2"}}}){
        auto bad=encoded;bad.replace(bad.find(invalid.first),std::string(invalid.first).size(),invalid.second);
        require(!parse_input_preferences(bad,parsed)&&parsed==preferences);
    }

    for(const auto& bad:{encoded+"EXTRA\n",std::string(4097,' '),std::string("SURFXR_FRAME_INPUT_PREFERENCES 2\n"),
                        std::string("SURFXR_FRAME_INPUT_PREFERENCES 1\nVR_CONTROLLERS_ENABLED 1.0\nEND\n"),
                        std::string("SURFXR_FRAME_INPUT_PREFERENCES 1\nVR_CONTROLLERS_ENABLED 2\nEND\n")})
        require(!parse_input_preferences(bad,parsed)&&parsed==preferences);
    auto custom=preferences;custom.input_shortcuts={KeyboardShortcut{0x1d,0x38,0x39},KeyboardShortcut{0x36}};
    require(custom!=preferences&&parse_input_preferences(encode_input_preferences(custom),parsed)&&parsed==custom);
    for(const KeyboardShortcut bad:{KeyboardShortcut{},KeyboardShortcut{0x1d,0,0x39},KeyboardShortcut{0x1d,0x1d},KeyboardShortcut{65535}}){
        require(!valid_keyboard_shortcut(bad));auto invalid=encoded;
        const auto at=invalid.find("SHORTCUT ");const auto end=invalid.find('\n',at);
        std::string row="SHORTCUT";for(auto code:bad)row+=" "+std::to_string(code);
        invalid.replace(at,end-at,row);require(!parse_input_preferences(invalid,parsed)&&parsed==custom);
    }
    parsed=preferences;
    auto v8=v7;v8.replace(v8.find("PREFERENCES 7"),13,"PREFERENCES 8");
    v8.insert(v8.find("END"),"INPUT_METHOD_SHORTCUT 57435 57 0 0 0 0 0 0\nINPUT_MODE_SHORTCUT 42 0 0 0 0 0 0 0\n");
    require(parse_input_preferences(v8,parsed)&&parsed.input_shortcuts==kDefaultInputShortcuts);
    v8.replace(v8.find("57435 57"),8,"29 57");require(parse_input_preferences(v8,parsed)&&parsed.input_shortcuts[0]==KeyboardShortcut{29,57}&&std::find(parsed.input_shortcuts.begin(),parsed.input_shortcuts.end(),KeyboardShortcut{29,47})!=parsed.input_shortcuts.end());
    auto empty=preferences;empty.input_shortcuts.clear();require(parse_input_preferences(encode_input_preferences(empty),parsed)&&parsed==empty);
    auto full=preferences;full.input_shortcuts.assign(kMaxKeyboardShortcuts,KeyboardShortcut{0x1d,0x39});require(parse_input_preferences(encode_input_preferences(full),parsed)&&parsed==full);
    auto overflow=encode_input_preferences(full);overflow.replace(overflow.find("SHORTCUTS 24"),12,"SHORTCUTS 25");require(!parse_input_preferences(overflow,parsed)&&parsed==full);
    parsed=preferences;
    InputPreferencesStore disabled("");require(!disabled.submit(preferences)&&disabled.finish().finished);
#ifndef _WIN32
    char directory[]="/tmp/surfxr-input-preferences-test-XXXXXX";require(::mkdtemp(directory)!=nullptr);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    const auto path=(cleanup.path/"input-preferences.conf").string();
    require(load_input_preferences(path,parsed)==InputPreferencesStoreStatus::Missing&&parsed==preferences);
    InputPreferencesStore store(path);
    for(int i=0;i<=100;++i){preferences.vr_controllers_enabled=(i%2)!=0;require(store.submit(preferences));}
    const auto stats=store.finish();require(stats.finished&&stats.writes>0&&stats.failures==0&&!store.submit(preferences));
    require(load_input_preferences(path,parsed)==InputPreferencesStoreStatus::Ok&&parsed==preferences);
    struct stat info{};require(::stat(path.c_str(),&info)==0&&(info.st_mode&0777)==0600);
    const auto link=(cleanup.path/"link.conf").string();require(::symlink(path.c_str(),link.c_str())==0);
    require(load_input_preferences(link,parsed)==InputPreferencesStoreStatus::IoError);
#endif
    std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"openxr_session_started\":false}\n";return 0;
}
