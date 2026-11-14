#include "surfxr/remote_settings.hpp"
#include "remote_setting_values.hpp"
#include <cmath>

namespace surfxr {
bool valid_setting_change(SettingChange c) noexcept {
    if(!std::isfinite(c.value))return false;
    switch(c.id){
    case SettingId::VrFramerate:case SettingId::DesktopFramerate:return setting_values::known_rate(c.value);
    case SettingId::VrGraphicsQuality:return c.value>=-1&&c.value<=7&&std::floor(c.value)==c.value;
    case SettingId::VrBitrateMbps:return c.value>=10&&c.value<=500;
    case SettingId::DesktopBitrateMbps:return c.value>=4&&c.value<=120;
    case SettingId::DesktopPointerPriority:case SettingId::MicrophoneEnabled:case SettingId::FoveatedStreaming:return c.value==0||c.value==1;
    case SettingId::Reserved:return c.value==0;
    case SettingId::MicrophoneVolume:return c.value>=0&&c.value<=1;
    default:return false;
    }
}
std::vector<int> protocol_vr_framerates(){return {setting_values::framerates.begin(),setting_values::framerates.end()};}
}
