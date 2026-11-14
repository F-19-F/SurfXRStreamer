#pragma once
#include "surfxr/vr_ui.hpp"
#include "surfxr/battery.hpp"
#include "surfxr/boundary.hpp"
#include <memory>
#include <array>
#include <cstdint>

namespace surfxr {
enum class TrackingOrigin {Unavailable,Stage};
enum class TrackingProfile {None,SteamFrame,OculusTouch,ValveIndex,Simple,Other};
// Values deliberately match OpenXR location bits, without an OpenXR dependency.
enum TrackingPoseFlag : std::uint32_t {OrientationValid=1,PositionValid=2,OrientationTracked=4,PositionTracked=8};
enum TrackingAnalogFlag : std::uint32_t {TriggerActive=1,SqueezeActive=2,ThumbstickActive=4};
// Physical controls, not wire bits. Frame ABXY are physically on the right.
enum TrackingButton : std::uint32_t {
    ButtonA=1u<<0,ButtonB=1u<<1,ButtonX=1u<<2,ButtonY=1u<<3,
    ButtonThumbstick=1u<<4,ButtonTrigger=1u<<5,ButtonSqueeze=1u<<6,
    ButtonMenu=1u<<7,ButtonView=1u<<8,ButtonBumper=1u<<9,
    ButtonDpadUp=1u<<10,ButtonDpadDown=1u<<11,ButtonDpadLeft=1u<<12,ButtonDpadRight=1u<<13,
    ButtonThumbrest=1u<<14,
    ButtonSystem=1u<<15 // physical system key; distinct from Frame left View
};
struct TrackingPose {
    UiPose pose{}; // meters; quaternion xyzw; right-handed +Y up, -Z forward
    UiVec3 linear_velocity{},angular_velocity{}; // base-space m/s and rad/s
    std::uint32_t flags=0;
    bool linear_velocity_valid=false,angular_velocity_valid=false;
};
struct TrackingController {
    TrackingProfile profile=TrackingProfile::None;
    TrackingPose aim{},grip{};
    bool aim_active=false,grip_active=false;
    // Physical link state, independent of OpenXR focus or temporary tracking loss.
    // Group with the pose flags to use their existing alignment gap.
    bool connection_known=false,connected=false;
    float trigger=0,squeeze=0,stick_x=0,stick_y=0;
    std::uint32_t analog_active=0;
    std::uint32_t buttons=0,buttons_active=0,touches=0,touches_active=0;
};
inline bool tracking_controller_connected(const TrackingController& hand) noexcept {
    return !hand.connection_known||hand.connected;
}
struct ControllerSkeleton {
    std::uint64_t sampled_monotonic_ns=0;
    std::array<UiPose,31> bones{}; // OpenVR parent-relative joints, including auxiliary bones.
};
struct GazeSample {
    std::array<std::array<float,2>,2> positions{};
    std::array<bool,2> valid{};
    std::uint64_t sampled_monotonic_ns=0,sequence=0,origin_epoch=0;
};
struct TrackingFrame {
    TrackingOrigin origin=TrackingOrigin::Unavailable;
    std::int64_t predicted_xr_time=0; // head/eye remote target in XrTime, not local display time
    std::int64_t query_xr_time=0; // XR now converted at this pose query; zero if unavailable
    // Start of the original remote pose query, in the Frame steady-clock
    // domain. It follows the frame ID unchanged through send and stereo pairing.
    std::uint64_t query_started_monotonic_ns=0;
    float prediction_lead_seconds=0; // pose time minus converted XR now at sampling
    bool prediction_time_valid=false; // finite, nonnegative and no more than 66ms
    // Local steady-clock midpoint around the CLOCK_MONOTONIC read used above.
    // A sender may subtract elapsed local age; it must not invent network age.
    std::uint64_t sampled_monotonic_ns=0,sequence=0,origin_epoch=0;
    bool visible=false,focused=false,stage_supported=false,stage_locatable=false;
    bool stage_bounds_valid=false;float stage_width=0,stage_depth=0;
    std::shared_ptr<const StageBoundary> boundary;
    bool boundary_sync_enabled=true; // Local preference; no new public wire field.
    TrackingPose head{};
    bool optics_valid=false;float ipd_meters=0;
    GazeSample gaze{};
    std::array<UiPose,2> eye_poses{};
    std::array<std::array<float,4>,2> eye_fov{}; // OpenXR radians: left,right,up,down
    // Physical left/right: SURFXR samples at query_xr_time for PC prediction;
    std::array<TrackingController,2> hands{};
    std::array<ControllerSkeleton,2> controller_skeletons{};
    std::array<bool,2> ui_captured{}; // raw data retained; VR forwarding must honor capture
    bool system_capture_active=false; // Session-only system-key routing; never persisted.
    BatterySnapshot battery{}; // physical HMD/left/right; unknown is local-only -1
};
bool tracking_pose_valid(const TrackingPose& pose) noexcept;
bool tracking_pose_tracked(const TrackingPose& pose) noexcept;
enum class TrackingPredictionStatus {Valid,InvalidTime,Past,TooFar};
// Both arguments are in the same runtime's XrTime domain. Invalid intervals
// clear the flag/value; they never silently clamp a differently timed pose.
TrackingPredictionStatus set_tracking_prediction_time(TrackingFrame& frame,std::int64_t now_xr_time) noexcept;
// Older desktop-only fallbacks use A for the shortcut. Keep that game button
// out of the local shortcut detector while VR controller forwarding is active.
bool tracking_shortcut_reserved(TrackingProfile profile,unsigned hand,bool vr_mode) noexcept;
// Explicit logical Touch button mapping; preserves raw poses/analogs. Protocol
// pose conversion is separate. Frame right X/Y become logical left X/Y.
// Frame left View becomes logical left Menu; optional left System is an alias.
// Right Menu remains local. An inactive/reserved System input is never invented.
std::array<TrackingController,2> touch_controller_mapping(const TrackingFrame& frame) noexcept;
} // namespace surfxr
