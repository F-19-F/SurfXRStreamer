#include "surfxr/remote_settings.hpp"
#include "remote_setting_values.hpp"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <string>
#include <stdexcept>
#include <limits>

namespace surfxr {
namespace {
using setting_values::framerates;
using setting_values::known_rate;
bool equal_value(SettingId id,double a,double b,int codec=2){
    (void)id;(void)codec;return std::abs(a-b)<=.0001;
}
bool supported(SettingId id,const RemoteSettingsCapabilities& c){
    if(!c.connected)return false;
    if(id==SettingId::DesktopPointerPriority)return c.surfxr_pointer_controls;
    if(c.surfxr_video_controls&&static_cast<unsigned>(id)<=2)return true;
    if(!c.server_video_controls&&id!=SettingId::MicrophoneEnabled&&id!=SettingId::MicrophoneVolume)return false;
    switch(id){
    case SettingId::VrFramerate:return (c.has_vr||c.can_launch_vr)&&c.vr_ready&&!c.vr_framerates.empty();
    case SettingId::VrGraphicsQuality:return (c.has_vr||c.can_launch_vr)&&c.vr_ready&&c.max_vr_quality>=-1;
    case SettingId::VrBitrateMbps:return (c.has_vr||c.can_launch_vr)&&c.vr_ready;
    case SettingId::DesktopFramerate:case SettingId::DesktopBitrateMbps:return true;
    case SettingId::MicrophoneEnabled:case SettingId::MicrophoneVolume:return c.microphone_available;
    case SettingId::FoveatedStreaming:return true; // Off is always available; on needs gaze below.
    default:return false;
    }
}
}
const std::array<VrQualityProfile,9>& surfxr_vr_quality_profiles() noexcept {
    // SurfXR per-eye resolution tiers shared with the PC Dashboard.
    static constexpr std::array<VrQualityProfile,9> profiles{{
        {-1,"POTATO",896,960,1344,1440},
        {0,"LOW",1152,1216,1728,1824},
        {1,"MEDIUM",1408,1536,2112,2304},
        {2,"HIGH",1664,1792,2496,2688},
        {3,"ULTRA",1792,1920,2688,2880},
        {4,"GODLIKE",2048,2176,3072,3264},
        {5,"MONSTER",2432,2688,3648,4032},
        {6,"RAMPAGE",2816,3072,4224,4608},
        {7,"UNSTOPPABLE",3328,3584,4992,5376}
    }};
    return profiles;
}
const VrQualityProfile* surfxr_vr_quality_profile(int quality) noexcept {
    return quality>=-1&&quality<=7?&surfxr_vr_quality_profiles()[static_cast<std::size_t>(quality+1)]:nullptr;
}
int surfxr_max_vr_quality(unsigned ew,unsigned eh,std::uint64_t ep,unsigned rw,unsigned rh,double sx,double sy) noexcept {
    if(!std::isfinite(sx)||!std::isfinite(sy)||sx<=0||sy<=0)return -2;
    int maximum=-2;
    for(const auto& profile:surfxr_vr_quality_profiles()){
        if(profile.encoded_width>ew||profile.encoded_height>eh||std::uint64_t{profile.encoded_width}*profile.encoded_height>ep||std::ceil(profile.render_width*sx)>rw||std::ceil(profile.render_height*sy)>rh)break;
        maximum=profile.quality;
    }
    return maximum;
}
const char* setting_name(SettingId id) noexcept {
    switch(id){
    case SettingId::DesktopPointerPriority:return "CURSOR PRIORITY";
    case SettingId::VrFramerate:return "VR FPS";
    case SettingId::VrGraphicsQuality:return "VR QUALITY";
    case SettingId::VrBitrateMbps:return "VR MBPS";
    case SettingId::DesktopFramerate:return "DESKTOP FPS";
    case SettingId::DesktopBitrateMbps:return "DESKTOP MBPS";
    case SettingId::MicrophoneEnabled:return "MICROPHONE";
    case SettingId::MicrophoneVolume:return "MIC VOLUME";
    case SettingId::FoveatedStreaming:return "Dynamic Foveated Streaming";
    default:return "UNKNOWN";
    }
}
const char* vr_menu_status_text(VrMenuStatus status) noexcept {
    switch(status){
    case VrMenuStatus::Idle:return "VR: READY";
    case VrMenuStatus::Starting:return "VR: STARTING - PLEASE WAIT";
    case VrMenuStatus::StageNotReady:return "VR: STAGE TRACKING NOT READY";
    case VrMenuStatus::NoSupportedRefreshRate:return "VR: NO SUPPORTED REFRESH RATE";
    case VrMenuStatus::SteamVrUnavailable:return "VR: STEAMVR UNAVAILABLE";
    case VrMenuStatus::ControlQueueFull:return "VR: CONTROL QUEUE FULL";
    case VrMenuStatus::StartTimedOut:return "VR: START TIMED OUT - RETRY";
    }
    return "VR: UNKNOWN STATUS";
}
double vr_bitrate_max_mbps(int codec) noexcept {(void)codec;return 1000.0;}
bool setting_request_allowed(SettingChange change,const RemoteSettingsSnapshot& state) noexcept {
    if(state.capabilities.surfxr_video_controls&&static_cast<unsigned>(change.id)<=2){
        if(!supported(change.id,state.capabilities)||!std::isfinite(change.value))return false;
        if(change.id==SettingId::VrBitrateMbps)return change.value>=5&&change.value<=1000&&std::floor(change.value)==change.value;
        if(change.id==SettingId::VrGraphicsQuality)return change.value>=-1&&change.value<=state.capabilities.max_vr_quality&&std::floor(change.value)==change.value;
        return std::find(state.capabilities.vr_framerates.begin(),state.capabilities.vr_framerates.end(),change.value)!=state.capabilities.vr_framerates.end();
    }
    if(!valid_setting_change(change)||!supported(change.id,state.capabilities))return false;
    if(change.id==SettingId::FoveatedStreaming&&change.value!=0&&!state.capabilities.eye_gaze_available)return false;
    if(change.id==SettingId::VrBitrateMbps&&change.value>vr_bitrate_max_mbps(state.capabilities.active_codec))return false;
    if(change.id==SettingId::VrGraphicsQuality&&change.value>state.capabilities.max_vr_quality)return false;
    if(change.id==SettingId::VrFramerate&&std::find(state.capabilities.vr_framerates.begin(),state.capabilities.vr_framerates.end(),change.value)==state.capabilities.vr_framerates.end())return false;
    return true;
}
std::vector<double> setting_choices(SettingId id,const RemoteSettingsSnapshot& state){
    if(!supported(id,state.capabilities))return {};
    std::vector<double> result;
    switch(id){
    case SettingId::VrFramerate:for(int rate:state.capabilities.vr_framerates)if(known_rate(rate)||state.capabilities.surfxr_video_controls)result.push_back(rate);break;
    case SettingId::DesktopFramerate:for(int rate:framerates)result.push_back(rate);break;
    case SettingId::VrGraphicsQuality:for(int quality=-1;quality<=std::clamp(state.capabilities.max_vr_quality,-2,7);++quality)result.push_back(quality);break;
    case SettingId::VrBitrateMbps:if(state.capabilities.surfxr_video_controls)result.push_back(5);for(int mbps=10;mbps<=(state.capabilities.surfxr_video_controls?1000:vr_bitrate_max_mbps(state.capabilities.active_codec));mbps+=10)result.push_back(mbps);break;
    case SettingId::DesktopBitrateMbps:result={4,10,20,30,40,50,60,70,80,90,100,110,120};break;
    case SettingId::DesktopPointerPriority:case SettingId::MicrophoneEnabled:result={0,1};break;
    case SettingId::FoveatedStreaming:result=state.capabilities.eye_gaze_available?std::vector<double>{0,1}:std::vector<double>{0};break;
    case SettingId::MicrophoneVolume:for(int value=0;value<=10;++value)result.push_back(value/10.0);break;
    default:break;
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
RemoteSettings::RemoteSettings(){
    for(std::size_t i=0;i<kDefaultRemoteSettings.size();++i)state_.values[i].requested=kDefaultRemoteSettings[i];
}
void RemoteSettings::update_support(){
    for(std::size_t i=0;i<kRemoteSettingCount;++i)state_.values[i].supported=supported(static_cast<SettingId>(i),state_.capabilities);
}
void RemoteSettings::set_capabilities(RemoteSettingsCapabilities c){
    c.max_vr_quality=std::clamp(c.max_vr_quality,-2,7);
    c.vr_framerates.erase(std::remove_if(c.vr_framerates.begin(),c.vr_framerates.end(),[&](int rate){return !known_rate(rate)&&!(c.surfxr_video_controls&&rate==144);}),c.vr_framerates.end());
    std::sort(c.vr_framerates.begin(),c.vr_framerates.end());c.vr_framerates.erase(std::unique(c.vr_framerates.begin(),c.vr_framerates.end()),c.vr_framerates.end());
    std::lock_guard<std::mutex> lock(mutex_);state_.capabilities=std::move(c);
    const auto quality=static_cast<std::size_t>(SettingId::VrGraphicsQuality);
    if(!requested_[quality]&&!state_.values[quality].actual&&state_.capabilities.max_vr_quality>=-1)
        state_.values[quality].requested=std::min(state_.values[quality].requested,double(state_.capabilities.max_vr_quality));
    update_support();++state_.revision;
}
RemoteSettingsSnapshot RemoteSettings::snapshot() const {std::lock_guard<std::mutex> lock(mutex_);return state_;}
void RemoteSettings::synchronize_surfxr(const std::array<double,3>& values,const std::array<bool,3>& pending){
    std::lock_guard<std::mutex> lock(mutex_);
    for(std::size_t i=0;i<3;++i){auto& value=state_.values[i];
        value.actual=values[i];if(!pending[i]){value.requested=values[i];requested_[i]=false;seeded_[i]=false;}
        value.pending=pending[i];
    }
    ++state_.revision;
}
void RemoteSettings::synchronize_pointer(double actual,bool pending,bool supported){
    std::lock_guard<std::mutex> lock(mutex_);
    state_.capabilities.surfxr_pointer_controls=supported;
    const auto i=static_cast<std::size_t>(SettingId::DesktopPointerPriority);
    auto& value=state_.values[i];value.supported=supported&&state_.capabilities.connected;
    if(supported){value.actual=actual;if(!pending){value.requested=actual;requested_[i]=seeded_[i]=false;}}
    else {if(value.actual)value.requested=*value.actual;value.actual.reset();requested_[i]=seeded_[i]=false;}
    value.pending=pending;++state_.revision;
}
bool RemoteSettings::request(SettingChange c){
    std::lock_guard<std::mutex> lock(mutex_);if(!setting_request_allowed(c,state_))return false;
    const auto i=static_cast<std::size_t>(c.id);auto& value=state_.values[i];requested_[i]=true;value.requested=c.value;
    value.pending=!value.actual||!equal_value(c.id,*value.actual,c.value,state_.capabilities.active_codec);++state_.revision;return true;
}
bool RemoteSettings::observe(SettingChange c){
    if(!valid_setting_change(c))return false;
    std::lock_guard<std::mutex> lock(mutex_);const auto i=static_cast<std::size_t>(c.id);auto& value=state_.values[i];value.actual=c.value;
    if(!requested_[i]&&!seeded_[i])value.requested=c.value;
    value.pending=requested_[i]&&!equal_value(c.id,value.requested,c.value,state_.capabilities.active_codec);++state_.revision;return true;
}
bool RemoteSettings::observe_vr_dimensions(unsigned ew,unsigned eh,unsigned rw,unsigned rh,bool /*foveated*/){
    if(!ew||!eh||!rw||!rh||ew>16384||eh>16384||rw>16384||rh>16384)return false;
    std::lock_guard<std::mutex> lock(mutex_);state_.vr_dimensions=VrObservedDimensions{ew,eh,rw,rh};
    ++state_.revision;return true;
}

void RemoteSettings::seed_requested(SettingChange c){
    if(!valid_setting_change(c))throw std::invalid_argument("invalid_initial_remote_setting");
    std::lock_guard<std::mutex> lock(mutex_);const auto i=static_cast<std::size_t>(c.id);
    if(requested_[i]||state_.values[i].actual)throw std::logic_error("remote_setting_already_used");
    state_.values[i].requested=c.value;seeded_[i]=true;++state_.revision;
}
} // namespace surfxr
