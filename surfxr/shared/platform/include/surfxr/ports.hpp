#pragma once

namespace surfxr::ports {
inline constexpr unsigned osc_local = 19942;
inline constexpr unsigned vr_control = 19943;
inline constexpr unsigned vr_media = 19944;
inline constexpr unsigned vr_priority = 19945;
inline constexpr unsigned dashboard = 19946;
inline constexpr unsigned gaze_osc = 19947;
inline constexpr unsigned desktop_control = 19950;
inline constexpr unsigned desktop_media = 19951;
inline constexpr unsigned desktop_priority = 19952;
inline constexpr unsigned web = 18082;
// Older installed service command lines persist their former default explicitly.
inline constexpr unsigned migrate_service_desktop(unsigned port) {
    return port == 9950 ? desktop_control : port;
}
}
