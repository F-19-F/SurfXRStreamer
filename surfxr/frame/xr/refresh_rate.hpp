#pragma once
#include "surfxr/remote_settings.hpp"
#include <cmath>

namespace surfxr::xr_detail {
// A PC setting may still be pending/restarting. SURFXR display synchronization
inline double display_refresh_target(const RemoteSettingsSnapshot* settings) noexcept {
    if(!settings)return 0;
    const auto& c=settings->capabilities;
    if(!c.vr_requested&&!c.vr_active)return 0;
    const double rate=c.vr_only?(c.connected?c.vr_stream_framerate:0):
        (c.server_video_controls?settings->at(SettingId::VrFramerate).requested:0);
    return std::isfinite(rate)&&rate>0&&rate<=1000?rate:0;
}
}
