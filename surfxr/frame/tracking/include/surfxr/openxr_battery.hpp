#pragma once
#include "surfxr/battery.hpp"
#include <openxr/openxr.h>

// Minimal ABI compatibility for Frame's older development headers. Values and
// field order match the Khronos OpenXR SDK 1.1.63 registry-generated header:
// https://github.com/KhronosGroup/OpenXR-SDK/blob/main/include/openxr/openxr.h
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrBatteryStateDisplayEXT.html
// This header does not enable an extension or claim runtime availability.
#ifndef XR_EXT_interaction_profile_battery_state_display
#define XR_EXT_interaction_profile_battery_state_display 1
#define XR_EXT_interaction_profile_battery_state_display_SPEC_VERSION 1
#define XR_EXT_INTERACTION_PROFILE_BATTERY_STATE_DISPLAY_EXTENSION_NAME "XR_EXT_interaction_profile_battery_state_display"
inline constexpr XrStructureType XR_TYPE_BATTERY_STATE_DISPLAY_EXT=static_cast<XrStructureType>(1000836000);
typedef XrFlags64 XrBatteryStateDisplayStateFlagsEXT;
inline constexpr XrBatteryStateDisplayStateFlagsEXT XR_BATTERY_STATE_DISPLAY_STATE_VALID_BIT_EXT=0x00000001;
inline constexpr XrBatteryStateDisplayStateFlagsEXT XR_BATTERY_STATE_DISPLAY_STATE_CHARGING_BIT_EXT=0x00000002;
inline constexpr XrBatteryStateDisplayStateFlagsEXT XR_BATTERY_STATE_DISPLAY_STATE_PLUGGED_IN_BIT_EXT=0x00000004;
inline constexpr XrBatteryStateDisplayStateFlagsEXT XR_BATTERY_STATE_DISPLAY_STATE_NO_BATTERY_BIT_EXT=0x00000008;
typedef struct XrBatteryStateDisplayEXT {
    XrStructureType type;
    void* next;
    XrBatteryStateDisplayStateFlagsEXT stateFlags;
    float batteryLevel;
} XrBatteryStateDisplayEXT;
#endif

namespace surfxr {
// Only call after enabling the extension and a successful per-hand
// xrGetCurrentInteractionProfile(session, hand_path, &profile) with this
// structure in profile.next. XR_NULL_PATH invalidates even apparently valid
// flags. The spec forbids reading batteryLevel without VALID.
inline DeviceBattery battery_from_openxr(XrPath profile,const XrBatteryStateDisplayEXT& state) noexcept {
    if(profile==XR_NULL_PATH||!(state.stateFlags&XR_BATTERY_STATE_DISPLAY_STATE_VALID_BIT_EXT)||
       (state.stateFlags&XR_BATTERY_STATE_DISPLAY_STATE_NO_BATTERY_BIT_EXT))return {};
    return battery_from_xr_state(true,state.stateFlags,state.batteryLevel);
}
} // namespace surfxr
