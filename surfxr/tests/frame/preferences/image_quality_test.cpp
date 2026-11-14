#include "surfxr/image_quality.hpp"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
int main(){
    using namespace surfxr;
    const auto require=[](bool value){if(!value)throw std::runtime_error("image_quality_test_failed");};
    ImageQualityPreferences preferences;require(valid_image_quality(preferences));
    require(preferences.desktop_filter==1&&!preferences.vr_upscaling&&preferences.vr_sharpening==25&&!preferences.vr_sharpening_enabled);
    require(apply_image_quality_change(preferences,{ImageQualityId::VrSharpeningEnabled,0}));
    require(preferences.vr_sharpening==25&&!preferences.vr_sharpening_enabled);
    require(apply_image_quality_change(preferences,{ImageQualityId::VrUpscaling,0}));
    require(apply_image_quality_change(preferences,{ImageQualityId::VrSharpening,65}));
    require(apply_image_quality_change(preferences,{ImageQualityId::VrSharpeningEnabled,1}));
    require(!preferences.vr_upscaling&&preferences.vr_sharpening_enabled&&preferences.vr_sharpening==65);
    require(preferences.vr_timewarp);
    require(apply_image_quality_change(preferences,{ImageQualityId::VrTimewarp,0})&&!preferences.vr_timewarp);
    const auto expected=preferences;
    for(const auto& change:{ImageQualityChange{ImageQualityId::DesktopFilter,2},{ImageQualityId::VrUpscaling,-1},
                           {ImageQualityId::VrSharpening,101},{ImageQualityId::VrSharpeningEnabled,2},{ImageQualityId::VrTimewarp,2},{static_cast<ImageQualityId>(5),0}}){
        require(!apply_image_quality_change(preferences,change)&&preferences==expected);
    }
    ImageQualityPreferences legacy;
    require(parse_image_quality("SURFXR_FRAME_IMAGE_QUALITY 1\nDESKTOP_FILTER 0\nVR_UPSCALING 0\nVR_SHARPENING 70\nVR_SHARPENING_ENABLED 1\nEND\n",legacy));
    require(legacy.vr_timewarp&&legacy.desktop_filter==0&&legacy.vr_sharpening==70&&legacy.vr_sharpening_enabled);
    const auto encoded=encode_image_quality(preferences);ImageQualityPreferences parsed;
    require(parse_image_quality(encoded,parsed)&&parsed==preferences);
    const auto historical=std::string({char(0x56),char(0x44)})+encoded.substr(std::string("SURFXR").size());
    require(parse_image_quality(historical,parsed));

    for(const auto& bad:{encoded+"EXTRA\n",std::string(1025,' '),std::string("SURFXR_FRAME_IMAGE_QUALITY 2\n"),
                        std::string("SURFXR_FRAME_IMAGE_QUALITY 1\nDESKTOP_FILTER 1.0\nVR_UPSCALING 1\nVR_SHARPENING 25\nVR_SHARPENING_ENABLED 1\nEND\n")}){
        require(!parse_image_quality(bad,parsed)&&parsed==preferences);
    }
    ImageQualityStore disabled("");require(!disabled.submit(preferences)&&disabled.finish().finished);
#ifndef _WIN32
    char directory[]="/tmp/surfxr-image-quality-test-XXXXXX";require(::mkdtemp(directory)!=nullptr);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}} cleanup{directory};
    const auto path=(cleanup.path/"image-quality.conf").string();
    require(load_image_quality(path,parsed)==ImageQualityStoreStatus::Missing&&parsed==preferences);
    ImageQualityStore store(path);
    for(int i=0;i<=100;++i){preferences.vr_sharpening=i;require(store.submit(preferences));}
    const auto stats=store.finish();require(stats.finished&&stats.writes>0&&stats.failures==0&&!store.submit(preferences));
    require(load_image_quality(path,parsed)==ImageQualityStoreStatus::Ok&&parsed==preferences);
    struct stat info{};require(::stat(path.c_str(),&info)==0&&(info.st_mode&0777)==0600);
    const auto link=(cleanup.path/"link.conf").string();require(::symlink(path.c_str(),link.c_str())==0);
    require(load_image_quality(link,parsed)==ImageQualityStoreStatus::IoError);
#endif
    std::cout<<"{\"status\":\"self_test_passed\",\"network_used\":false,\"openxr_session_started\":false}\n";return 0;
}
