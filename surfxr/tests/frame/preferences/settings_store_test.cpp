#include "surfxr/settings_store.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
void require(bool condition,const char* label){if(!condition)throw std::runtime_error(label);}
surfxr::RemoteSettingsSnapshot snapshot(const surfxr::SettingsPreferences& preferences){
    surfxr::RemoteSettingsSnapshot result;
    for(std::size_t i=0;i<preferences.values.size();++i){
        result.values[i].requested=preferences.values[i];
        result.values[i].actual=99999; // Never persist received/actual values.
        result.values[i].pending=true;
    }
    result.capabilities.active_codec=255;
    result.vr_dimensions=surfxr::VrObservedDimensions{1,2,3,4};
    return result;
}
std::string replace(std::string text,const std::string& from,const std::string& to){
    const auto position=text.find(from);require(position!=std::string::npos,"test_fixture_replace");
    text.replace(position,from.size(),to);return text;
}
#ifndef _WIN32
struct Directory {
    std::string path;
    Directory(){char name[]="/tmp/surfxr-settings-test-XXXXXX";const auto* value=::mkdtemp(name);require(value!=nullptr,"test_directory");path=value;}
    ~Directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void write(const std::string& path,const std::string& text){
    std::ofstream output(path,std::ios::binary);output<<text;require(static_cast<bool>(output),"write_test_fixture");
}
struct Environment {
    std::string name;
    std::optional<std::string> prior;
    Environment(const char* key,const char* value):name(key){
        if(const auto* old=std::getenv(key))prior=old;
        const int result=value ? ::setenv(key,value,1) : ::unsetenv(key);
        require(result==0,"test_environment");
    }
    ~Environment(){if(prior)::setenv(name.c_str(),prior->c_str(),1);else ::unsetenv(name.c_str());}
};
#endif
unsigned self_test(){
    unsigned groups=0;
    surfxr::SettingsPreferences defaults;
    surfxr::RemoteSettings remote;
    require(defaults.at(surfxr::SettingId::VrFramerate)==120&&defaults.at(surfxr::SettingId::VrGraphicsQuality)==4,"godlike_120_defaults");
    require(defaults.at(surfxr::SettingId::VrBitrateMbps)==100&&defaults.at(surfxr::SettingId::DesktopFramerate)==60&&
        defaults.at(surfxr::SettingId::DesktopBitrateMbps)==60&&defaults.at(surfxr::SettingId::DesktopPointerPriority)==0,"pc_stream_defaults");
    const auto* profile=surfxr::surfxr_vr_quality_profile(static_cast<int>(defaults.at(surfxr::SettingId::VrGraphicsQuality)));
    require(profile&&profile->render_width==surfxr::kDefaultVrViewWidth&&profile->render_height==surfxr::kDefaultVrViewHeight,"default_resolution_matches_tier");
    surfxr::SettingsPreferences requested;
    require(surfxr::requested_settings_preferences(remote.snapshot(),requested)&&requested.values==defaults.values,"remote_and_saved_defaults_agree");
    for(const auto& value:remote.snapshot().values)require(!value.actual&&!value.pending,"defaults_are_neither_observations_nor_edits");
    const auto encoded=surfxr::encode_settings_preferences(defaults);
    surfxr::SettingsPreferences parsed;
    require(surfxr::parse_settings_preferences(encoded,parsed)&&parsed.values==defaults.values,"default_roundtrip");
    const auto historical=std::string({char(0x56),char(0x44)})+encoded.substr(std::string("SURFXR").size());
    require(surfxr::parse_settings_preferences(historical,parsed),"historical_product_tag_preserves_settings");

    auto version2=replace(encoded,"SURFXR_FRAME_SETTINGS 5\nEVO_VIDEO 0","SURFXR_FRAME_SETTINGS 2");
    version2=replace(version2,"DESKTOP_POINTER_PRIORITY 0\n","");
    version2=replace(version2,"RESERVED 0\n","");
    require(surfxr::parse_settings_preferences(version2,parsed)&&parsed.at(surfxr::SettingId::Reserved)==0,"version2_defaults_reserved_zero");
    auto legacy=replace(version2,"SURFXR_FRAME_SETTINGS 2","SURFXR_FRAME_SETTINGS 1");
    legacy=replace(legacy,"FOVEATED_STREAMING 1\n","");
    require(surfxr::parse_settings_preferences(legacy,parsed)&&parsed.at(surfxr::SettingId::FoveatedStreaming)==1,"legacy_settings_default_gaze_on");
    ++groups;
    {
        surfxr::SettingsPreferences custom;
        custom.values={{120,7,191.234567890123,30,117.876543210987,1,.12345678901234,1,0}};
        const auto text=surfxr::encode_settings_preferences(custom);
        require(surfxr::parse_settings_preferences(text,parsed)&&parsed.values==custom.values,"all_values_exact_roundtrip");
        auto received=snapshot(custom);
        require(surfxr::requested_settings_preferences(received,parsed)&&parsed.values==custom.values,"only_requested_values_extracted");
        require(text.find("99999")==std::string::npos&&text.find("actual")==std::string::npos&&text.find("codec")==std::string::npos,"no_server_fields_saved");
        received.values[0].requested=std::numeric_limits<double>::quiet_NaN();
        require(!surfxr::requested_settings_preferences(received,parsed)&&parsed.values==custom.values,"invalid_snapshot_does_not_replace");
        ++groups;
    }
    {
        const std::array<double,9> invalid_values{{144,8,501,42,121,2,1.01,2,3}};
        for(std::size_t i=0;i<invalid_values.size();++i){
            auto invalid=defaults;invalid.values[i]=invalid_values[i];
            require(!surfxr::valid_settings_preferences(invalid),"out_of_range_rejected");
            bool threw=false;
            try{(void)surfxr::encode_settings_preferences(invalid);}catch(const std::invalid_argument&){threw=true;}
            require(threw,"invalid_cannot_encode");
        }
        auto invalid=defaults;invalid.values[2]=std::numeric_limits<double>::infinity();
        require(!surfxr::valid_settings_preferences(invalid),"nonfinite_rejected");
        ++groups;
    }
    {
        auto plus=defaults;plus.values[static_cast<std::size_t>(surfxr::SettingId::VrBitrateMbps)]=500;
        require(surfxr::parse_settings_preferences(surfxr::encode_settings_preferences(plus),parsed)&&parsed.values==plus.values,"absolute_bitrate_preference_roundtrip");
        ++groups;
    }
    {
        const std::vector<std::string> malformed{
            "",encoded.substr(0,encoded.size()-5),encoded+"EXTRA 1\n",
            replace(encoded,"SURFXR_FRAME_SETTINGS 5","SURFXR_FRAME_SETTINGS 6"),
            replace(encoded,"VR_FRAMERATE 120","VR_FRAMERATE 144"),
            replace(encoded,"VR_QUALITY 4","VR_QUALITY 2.5"),
            replace(encoded,"VR_QUALITY 4","VR_FRAMERATE 90"),
            replace(encoded,"MICROPHONE_ENABLED 0","MICROPHONE_ENABLED true"),
            replace(encoded,"VR_QUALITY 4","VR_QUALITY nan"),
            replace(encoded,"VR_QUALITY 4","ACCOUNT 2"),
            encoded+std::string(1,'\0'),encoded+std::string(1,static_cast<char>(0xff)),
            encoded+std::string(4096,' ')};
        for(const auto& text:malformed){
            auto preserved=defaults;preserved.values[1]=6;
            require(!surfxr::parse_settings_preferences(text,preserved)&&preserved.values[1]==6,"malformed_file_preserves_defaults");
        }
        ++groups;
    }
    {
        surfxr::SettingsStore disabled("");
        require(!disabled.submit(snapshot(defaults)),"disabled_rejects_submit");
        const auto state=disabled.finish();
        require(!state.enabled&&state.finished&&state.writes==0&&state.last_status==surfxr::SettingsStoreStatus::Disabled,"disabled_has_no_worker_or_writes");
        require(surfxr::load_settings("",parsed)==surfxr::SettingsStoreStatus::Disabled,"disabled_load");
        require(surfxr::load_settings("relative/settings.conf",parsed)==surfxr::SettingsStoreStatus::Invalid,"relative_path_rejected");
        ++groups;
    }
#ifndef _WIN32
    Directory directory;
    const auto path=directory.path+"/state/settings.conf";
    {
        auto preserved=defaults;preserved.values[1]=6;
        require(surfxr::load_settings(path,preserved)==surfxr::SettingsStoreStatus::Missing&&preserved.values[1]==6,"missing_preserves_defaults");
        const auto bad=directory.path+"/bad.conf";
        write(bad,"invalid");
        require(surfxr::load_settings(bad,preserved)==surfxr::SettingsStoreStatus::Invalid&&preserved.values[1]==6,"bad_file_preserves_defaults");
        write(bad,std::string(4097,' '));
        require(surfxr::load_settings(bad,preserved)==surfxr::SettingsStoreStatus::Invalid,"oversize_file_rejected");
        require(::symlink(bad.c_str(),(directory.path+"/link").c_str())==0,"symlink_fixture");
        require(surfxr::load_settings(directory.path+"/link",preserved)==surfxr::SettingsStoreStatus::IoError,"symlink_not_followed");
        require(::mkfifo((directory.path+"/fifo").c_str(),0600)==0,"fifo_fixture");
        require(surfxr::load_settings(directory.path+"/fifo",preserved)==surfxr::SettingsStoreStatus::Invalid,"fifo_not_read_or_blocked");
        ++groups;
    }
    {
        surfxr::SettingsStore store(path);
        require(surfxr::load_settings(path,parsed)==surfxr::SettingsStoreStatus::Missing,"construction_does_not_write");
        auto latest=defaults;
        for(unsigned i=0;i<500;++i){
            latest.values[2]=10+static_cast<double>(i%191);
            require(store.submit(snapshot(latest)),"valid_submit");
        }
        const auto state=store.finish();
        require(state.enabled&&state.finished&&state.submitted==500&&state.writes>0&&state.failures==0,"writer_flushes_on_finish");
        require(state.submitted==state.coalesced+state.writes,"bounded_coalescing_accounting");
        require(surfxr::load_settings(path,parsed)==surfxr::SettingsStoreStatus::Ok&&parsed.values==latest.values,"newest_preference_wins");
        require(!store.submit(snapshot(defaults))&&store.finish().writes==state.writes,"finish_idempotent_rejects_late_submit");
        struct stat file{},parent{};
        require(::stat(path.c_str(),&file)==0&&(file.st_mode&0777)==0600,"private_file_permissions");
        require(::stat((directory.path+"/state").c_str(),&parent)==0&&(parent.st_mode&0777)==0700,"private_directory_permissions");
        for(const auto& entry:std::filesystem::directory_iterator(directory.path+"/state"))
            require(entry.path().filename()=="settings.conf","temporary_files_removed");
        ++groups;
    }
    {
        auto latest=defaults;latest.values[1]=5;
        {surfxr::SettingsStore store(path);require(store.submit(snapshot(latest)),"destructor_submit");}
        require(surfxr::load_settings(path,parsed)==surfxr::SettingsStoreStatus::Ok&&parsed.values==latest.values,"destructor_flushes");
        surfxr::SettingsStore store(path);
        auto invalid=snapshot(latest);invalid.values[2].requested=-1;
        require(!store.submit(invalid)&&store.finish().writes==0,"invalid_submit_never_writes");
        require(surfxr::load_settings(path,parsed)==surfxr::SettingsStoreStatus::Ok&&parsed.values==latest.values,"invalid_submit_preserves_file");
        ++groups;
    }
    {
        const auto blocked=directory.path+"/blocked.conf";
        require(std::filesystem::create_directory(blocked),"blocked_target_fixture");
        write(blocked+"/sentinel","preserve");
        surfxr::SettingsStore store(blocked);
        require(store.submit(snapshot(defaults)),"failure_record_accepted");
        const auto state=store.finish();
        require(state.failures==1&&state.writes==0&&state.last_status==surfxr::SettingsStoreStatus::IoError,"write_failure_reported");
        require(std::filesystem::is_regular_file(blocked+"/sentinel"),"failed_rename_preserves_target");
        for(const auto& entry:std::filesystem::directory_iterator(directory.path))
            require(entry.path().filename().string().find(".blocked.conf.tmp.")!=0,"failed_temporary_removed");
        ++groups;
    }
    {
        surfxr::SettingsStore store(path);
        std::atomic<bool> stop{false},invalid_read{false};
        std::thread reader([&]{
            while(!stop.load()){
                surfxr::SettingsPreferences observed;
                if(surfxr::load_settings(path,observed)!=surfxr::SettingsStoreStatus::Ok)invalid_read.store(true);
            }
        });
        auto latest=defaults;
        bool accepted=true;
        for(unsigned i=0;i<300;++i){latest.values[2]=10+static_cast<double>(i%191);accepted=store.submit(snapshot(latest))&&accepted;}
        const auto state=store.finish();stop.store(true);reader.join();
        require(accepted&&!invalid_read.load()&&state.failures==0,"readers_only_see_complete_replacements");
        require(surfxr::load_settings(path,parsed)==surfxr::SettingsStoreStatus::Ok&&parsed.values==latest.values,"concurrent_final_snapshot");
        ++groups;
    }
    {
        Environment home("HOME","/test-home");
        {Environment xdg("XDG_STATE_HOME","/test-state");require(surfxr::default_settings_path()=="/test-state/surfxr-client/settings.conf","xdg_state_path");}
        {Environment xdg("XDG_STATE_HOME","relative");require(surfxr::default_settings_path()=="/test-home/.local/state/surfxr-client/settings.conf","relative_xdg_uses_home");}
        {Environment xdg("XDG_STATE_HOME",nullptr);Environment invalid_home("HOME","relative");require(surfxr::default_settings_path().empty(),"invalid_environment_disables_default");}
        ++groups;
    }
#endif
    return groups;
}
} // namespace
int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0){std::cerr<<"usage: surfxr-settings-test --self-test\n";return 2;}
    try{
        const auto groups=self_test();
        std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"success\":true,\"groups\":"<<groups<<",\"posix_io_tested\":"
#ifdef _WIN32
                 <<"false"
#else
                 <<"true"
#endif
                 <<"}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"settings_test_failed: "<<error.what()<<'\n';return 1;}
}
