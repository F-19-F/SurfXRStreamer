#include "viewer_private.hpp"
#include "surfxr/surfxr_modes.hpp"

namespace surfxr::viewer {
ViewerOptions options(int argc,char** argv){
    ViewerOptions result;result.session.seconds=0;result.session.microphone_enabled=true;result.session.vr_enabled=true;
    if(const char* base=std::getenv("XDG_STATE_HOME");base && base[0]=='/')result.layout_path=std::string(base)+"/surfxr-client/panels.layout";
    else if(const char* base=std::getenv("HOME");base && base[0]=='/')result.layout_path=std::string(base)+"/.local/state/surfxr-client/panels.layout";
    result.settings_path=surfxr::default_settings_path();
    result.image_quality_path=surfxr::default_image_quality_path();bool image_quality_set=false;
    result.input_preferences_path=surfxr::default_input_preferences_path();bool input_preferences_set=false;
    bool seconds_set=false,backend_set=false,audio_set=false,input_set=false,layout_set=false,microphone_set=false,settings_set=false,prediction_set=false,affinity_set=false,motion_set=false;
    bool protocol_set=false,surfxr_port_set=false,surfxr_width_set=false,surfxr_height_set=false,surfxr_version_set=false;
    bool desktop_port_set=false,media_encryption_set=false;
    for(int i=1;i<argc;++i){
        if(std::strcmp(argv[i],"--start-vr")==0&&!result.start_vr){result.start_vr=true;continue;}
        if(std::strcmp(argv[i],"--surfxr-desktop")==0&&!result.session.surfxr_desktop){result.session.surfxr_desktop=true;continue;}
        if(i+1>=argc)throw std::runtime_error("missing_option_value");
        const std::string key=argv[i];const char* value=argv[++i];
        if(key=="--backend"&&!protocol_set){
            if(std::strcmp(value,"surfxr")==0)result.session.backend=surfxr::SessionBackend::Surfxr;
            else throw std::runtime_error("invalid_session_backend");
            protocol_set=true;
        }else if(key=="--surfxr-media-encryption"&&!media_encryption_set){
            if(std::strcmp(value,"aes")!=0&&std::strcmp(value,"off")!=0)throw std::runtime_error("invalid_surfxr_media_encryption");
            result.session.surfxr_media_aes=std::strcmp(value,"aes")==0;media_encryption_set=true;
        }else if(key=="--surfxr-version"&&!surfxr_version_set){
            if(std::strcmp(value,"21.0.0-dev14")!=0)throw std::runtime_error("unsupported_surfxr_version");
            result.session.surfxr_version=value;surfxr_version_set=true;
        }else if((key=="--surfxr-control-port"&&!surfxr_port_set)||(key=="--surfxr-view-width"&&!surfxr_width_set)||(key=="--surfxr-view-height"&&!surfxr_height_set)){
            unsigned number=0;const auto end=value+std::strlen(value);const auto parsed=std::from_chars(value,end,number);
            const bool port=key=="--surfxr-control-port",width=key=="--surfxr-view-width";
            if(parsed.ec!=std::errc{}||parsed.ptr!=end||number<(port?1024u:32u)||number>(port?65535u:(width?4096u:8192u)))throw std::runtime_error("invalid_surfxr_option");
            if(port){result.session.surfxr_control_port=number;surfxr_port_set=true;}
            else if(width){result.session.surfxr_view_width=number;surfxr_width_set=true;}
            else{result.session.surfxr_view_height=number;surfxr_height_set=true;}
        }else if(key=="--surfxr-desktop-key-file"&&result.session.surfxr_desktop_key_file.empty()){
            if(value[0]!='/')throw std::runtime_error("surfxr_desktop_key_path_must_be_absolute");
            result.session.surfxr_desktop_key_file=value;
        }else if(key=="--surfxr-desktop-port"&&!desktop_port_set){
            unsigned n=0;const auto end=value+std::strlen(value);const auto parsed=std::from_chars(value,end,n);if(parsed.ec!=std::errc{}||parsed.ptr!=end||n<1024||n>65533)throw std::runtime_error("surfxr_desktop_invalid_port");result.session.surfxr_desktop_port=n;desktop_port_set=true;
        }else if(key=="--host" && result.session.host.empty())result.session.host=value;
        else if(key=="--local-address"&&result.session.local_address.empty()){
            if(!surfxr::valid_local_ipv4(value))throw std::runtime_error("invalid_local_address");
            result.session.local_address=value;
        }
        else if(key=="--seconds" && !seconds_set){
            const auto end=value+std::strlen(value);const auto parsed=std::from_chars(value,end,result.session.seconds);
            if(parsed.ec!=std::errc{} || parsed.ptr!=end || result.session.seconds>3600)throw std::runtime_error("duration_out_of_range");
            seconds_set=true;
        }else if(key=="--decoder" && !backend_set){
            if(std::strcmp(value,"software")==0)result.backend=surfxr::DecoderBackend::Software;
            else if(std::strcmp(value,"v4l2")==0)result.backend=surfxr::DecoderBackend::V4l2;
            else throw std::runtime_error("invalid_decoder_backend");
            backend_set=true;
        }else if(key=="--cpu-affinity"&&!affinity_set){
            if(std::strcmp(value,"available")==0)result.available_cpu_affinity=true;
            else if(std::strcmp(value,"inherit")==0)result.available_cpu_affinity=false;
            else throw std::runtime_error("invalid_cpu_affinity_option");
            affinity_set=true;
        }else if(key=="--test-head-motion"&&!motion_set){
            if(std::strcmp(value,"yaw-pitch")!=0)throw std::runtime_error("invalid_test_head_motion");
            result.test_head_motion=true;motion_set=true;
        }else if(key=="--audio" && !audio_set){
            if(std::strcmp(value,"on")==0)result.audio=true;
            else if(std::strcmp(value,"off")==0)result.audio=false;
            else throw std::runtime_error("invalid_audio_option");
            audio_set=true;
        }else if(key=="--microphone" && !microphone_set){
            if(std::strcmp(value,"on")==0)result.session.microphone_enabled=true;
            else if(std::strcmp(value,"off")==0)result.session.microphone_enabled=false;
            else throw std::runtime_error("invalid_microphone_option");
            microphone_set=true;
            result.microphone_override=true;
        }else if(key=="--input" && !input_set){
            if(std::strcmp(value,"on")==0)result.input=true;
            else if(std::strcmp(value,"off")==0)result.input=false;
            else throw std::runtime_error("invalid_input_option");
            input_set=true;
        }else if(key=="--vr-prediction" && !prediction_set){
            if(std::strcmp(value,"adaptive")==0)result.adaptive_prediction=true;
            else if(std::strcmp(value,"display")==0)result.adaptive_prediction=false;
            else throw std::runtime_error("invalid_vr_prediction_option");
            prediction_set=true;
        }else if(key=="--layout" && !layout_set){
            if(std::strcmp(value,"none")==0)result.layout_path.clear();
            else if(value[0]=='/')result.layout_path=value;
            else throw std::runtime_error("layout_path_must_be_absolute_or_none");
            layout_set=true;
        }else if(key=="--input-preferences"&&!input_preferences_set){
            if(std::strcmp(value,"none")==0)result.input_preferences_path.clear();
            else if(value[0]=='/')result.input_preferences_path=value;
            else throw std::runtime_error("input_preferences_path_must_be_absolute_or_none");
            input_preferences_set=true;
        }else if(key=="--image-quality"&&!image_quality_set){
            if(std::strcmp(value,"none")==0)result.image_quality_path.clear();
            else if(value[0]=='/')result.image_quality_path=value;
            else throw std::runtime_error("image_quality_path_must_be_absolute_or_none");
            image_quality_set=true;
        }else if(key=="--settings" && !settings_set){
            if(std::strcmp(value,"none")==0)result.settings_path.clear();
            else if(value[0]=='/')result.settings_path=value;
            else throw std::runtime_error("settings_path_must_be_absolute_or_none");
            settings_set=true;
        }else if(key=="--status-file"&&result.status_path.empty()){
            if(value[0]!='/'||std::strlen(value)>4000)throw std::runtime_error("status_path_must_be_absolute");
            result.status_path=value;
        }else throw std::runtime_error("unknown_or_duplicate_option");
    }
    in_addr address{};
    if(!result.session.surfxr_desktop&&!result.session.surfxr_desktop_key_file.empty())throw std::runtime_error("surfxr_desktop_options_require_desktop_mode");
    if(result.session.surfxr_desktop){
        if(result.session.backend!=surfxr::SessionBackend::Surfxr||result.session.surfxr_version!="21.0.0-dev14"||result.start_vr)throw std::runtime_error("surfxr_desktop_requires_v21_desktop_mode");
    }else if(result.session.backend==surfxr::SessionBackend::Surfxr)result.start_vr=true;
    else if(surfxr_port_set||surfxr_width_set||surfxr_height_set||surfxr_version_set)throw std::runtime_error("surfxr_options_require_backend");
    if((desktop_port_set||media_encryption_set)&&result.session.backend!=surfxr::SessionBackend::Surfxr)throw std::runtime_error("surfxr_options_require_backend");
    if(result.session.backend==surfxr::SessionBackend::Surfxr&&result.session.surfxr_version=="21.0.0-dev14"&&result.session.surfxr_desktop_key_file.empty())
        result.session.surfxr_modes=std::make_shared<surfxr::transport::Modes>(result.session.surfxr_desktop);
    if(result.session.host.empty() || inet_pton(AF_INET,result.session.host.c_str(),&address)!=1)throw std::runtime_error("ipv4_host_required");
    return result;
}
surfxr::SettingsPreferences prepare_initial_settings(surfxr::RemoteSettings& settings){
    const auto initial=settings.snapshot();surfxr::SettingsPreferences preferences;
    surfxr::requested_settings_preferences(initial,preferences);
    // Accepted SurfXR snapshots retain their full negotiated range.
    for(std::size_t i=0;i<surfxr::kRemoteSettingCount;++i){
        if(initial.capabilities.surfxr_video_controls&&i<3)continue;
        settings.seed_requested({static_cast<surfxr::SettingId>(i),preferences.values[i]});
    }
    return preferences;
}
unsigned restore_settings(surfxr::RemoteSettings& settings,const surfxr::SettingsPreferences& saved){
    const auto caps=settings.snapshot().capabilities;unsigned adjusted=0;
    if(saved.surfxr_video&&caps.surfxr_video_controls)
        settings.synchronize_surfxr({saved.values[0],saved.values[1],saved.values[2]},{});
    for(std::size_t i=0;i<surfxr::kRemoteSettingCount;++i){
        if(saved.surfxr_video&&i<3)continue;
        const auto id=static_cast<surfxr::SettingId>(i);double value=saved.values[i];
        if(!surfxr::valid_setting_change({id,value})){++adjusted;continue;}
        if(id==surfxr::SettingId::VrFramerate&&std::find(caps.vr_framerates.begin(),caps.vr_framerates.end(),value)==caps.vr_framerates.end()){++adjusted;continue;}
        if(id==surfxr::SettingId::VrGraphicsQuality&&value>caps.max_vr_quality){
            ++adjusted;if(caps.max_vr_quality<-1)continue;value=caps.max_vr_quality;
        }
        if(id==surfxr::SettingId::MicrophoneEnabled&&!caps.microphone_available&&value!=0){value=0;++adjusted;}
        settings.seed_requested({id,value});
    }
    return adjusted;
}

}
