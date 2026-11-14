#pragma once
// Private XR state; decoder storage stays alive until the GPU completion fence.
// OpenXR display of immutable latest RGBA snapshots.
// This library owns no network connection or decoder and installs no signal handlers.
// XR_KHR_vulkan_enable2 reference:
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <time.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "surfxr/xr_display.hpp"
#include "current_tracking.hpp"
#include "surfxr/projection_renderer.hpp"
#include "surfxr/foveation_renderer.hpp"
#include "surfxr/image_enhancement_renderer.hpp"
#include "surfxr/postprocess_plan.hpp"
#include "surfxr/motion_sync.hpp"
#include "surfxr/shortcut.hpp"
#include "surfxr/system_input.hpp"
#include "steamvr_boundary.hpp"
#include "ipd_sampling.hpp"
#include "surfxr/vr_wire.hpp"
#include "surfxr/rgba_upload.hpp"
#include "surfxr/vulkan_rgba_storage.hpp"
#include "surfxr/vulkan_nv12_storage.hpp"
#include "surfxr/openxr_battery.hpp"
#include "surfxr/gaze.hpp"
#include "surfxr/performance_hud.hpp"
#include "surfxr/mini_hud.hpp"
#include "surfxr/startup_hud.hpp"
#include "presentation_pacing.hpp"
#include "rotational_timewarp.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace surfxr::xr_detail {
using Clock = std::chrono::steady_clock;
inline std::uint64_t clock_ns(Clock::time_point value) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}

inline void xr_check(XrResult result, const char* call) {
    if (XR_FAILED(result))
        throw std::runtime_error(std::string(call) + ": XrResult " + std::to_string(result));
}
inline void vk_check(VkResult result, const char* call) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(call) + ": VkResult " + std::to_string(result));
}
template<class T> T xr_info(XrStructureType type) { T value{}; value.type = type; return value; }
template<class T> T vk_info(VkStructureType type) { T value{}; value.sType = type; return value; }
template<class T> T extension(XrInstance instance, const char* name) {
    PFN_xrVoidFunction value = nullptr;
    xr_check(xrGetInstanceProcAddr(instance, name, &value), name);
    if (!value) throw std::runtime_error(std::string("Missing OpenXR function: ") + name);
    return reinterpret_cast<T>(value);
}

} // namespace

namespace surfxr {
using namespace xr_detail;
struct XrDisplay::Impl {
    bool dmabuf_device_enabled_=false,mip_format_list_enabled_=false;
    struct Surface {
        XrSwapchain swapchain=XR_NULL_HANDLE;
        VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; void* mapped=nullptr;
        std::size_t buffer_bytes=0;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        std::vector<VkImageView> views;
        std::vector<VkImageView> raw_views; // Same sRGB storage, encoded UNORM access.
        std::vector<std::uint64_t> image_serials;
        std::uint32_t width=0,height=0; bool coherent=false,cached=false,have_frame=false,raw_requested=false;
        std::uint64_t generation=0,submitted_generation=0,serial=0,decoded_ns=0,released_serial=0;
        VkBuffer direct_buffer=VK_NULL_HANDLE;
        std::shared_ptr<const VideoFrame> direct_frame;
        VulkanRgbaFrameStorage* sampled_storage=nullptr; // Owned by direct_frame; VR-only.
        VulkanNv12FrameStorage* nv12_storage=nullptr; // Same frame/fence ownership.
        std::array<float,4> sample_rect{0,0,1,1};
        VkDeviceSize direct_offset=0;
        std::uint32_t direct_row_length=0;
        RgbaUploadEdgePlan edge_plan;
        std::unique_ptr<FoveationRenderer> desktop_nv12_renderer;
        bool desktop_nv12_pending=false;
    };
    struct Acquired {Surface* surface=nullptr;std::uint32_t index=0;bool waited=false,released=false,suppress_upload=false;};
    struct Layer {
        XrCompositionLayerQuad quad{};
        XrCompositionLayerCylinderKHR cylinder{};
        UiVec3 center;
        bool is_cylinder=false;
        explicit Layer(XrCompositionLayerQuad value):quad(value),center{value.pose.position.x,value.pose.position.y,value.pose.position.z}{}
        Layer(XrCompositionLayerCylinderKHR value,UiVec3 surface_center):cylinder(value),center(surface_center),is_cylinder(true){}
        const XrCompositionLayerBaseHeader* header() const {
            return is_cylinder?reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder):
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        }
    };
    XrInstance instance_=XR_NULL_HANDLE;XrSystemId system_=XR_NULL_SYSTEM_ID;
    std::atomic_bool decoder_frames_retire_requested_{false};
    void retire_decoder_frames();
    XrSession session_=XR_NULL_HANDLE;XrSpace space_=XR_NULL_HANDLE,head_space_=XR_NULL_HANDLE,stage_space_=XR_NULL_HANDLE;
    XrActionSet action_set_=XR_NULL_HANDLE;
    XrAction aim_action_=XR_NULL_HANDLE,select_action_=XR_NULL_HANDLE,grip_action_=XR_NULL_HANDLE,stick_action_=XR_NULL_HANDLE,menu_action_=XR_NULL_HANDLE;
    XrAction right_click_action_=XR_NULL_HANDLE;
    XrAction grip_pose_action_=XR_NULL_HANDLE;
    XrAction haptic_action_=XR_NULL_HANDLE;
    XrAction gaze_action_=XR_NULL_HANDLE;
    XrSpace gaze_space_=XR_NULL_HANDLE;
    std::array<bool,2> haptic_active_{};
    struct RawButtonAction {std::uint32_t bit;const char* name;XrAction click=XR_NULL_HANDLE,touch=XR_NULL_HANDLE;};
    std::array<RawButtonAction,16> raw_buttons_{{
        {ButtonA,"a"},{ButtonB,"b"},{ButtonX,"x"},{ButtonY,"y"},
        {ButtonThumbstick,"thumbstick"},{ButtonTrigger,"trigger"},{ButtonSqueeze,"squeeze"},
        {ButtonMenu,"menu"},{ButtonView,"view"},{ButtonBumper,"bumper"},
        {ButtonDpadUp,"dpad_up"},{ButtonDpadDown,"dpad_down"},{ButtonDpadLeft,"dpad_left"},{ButtonDpadRight,"dpad_right"},{ButtonThumbrest,"thumbrest"},{ButtonSystem,"system"}}};
    std::array<XrPath,2> hand_paths_{};std::array<XrSpace,2> aim_spaces_{},grip_spaces_{};
    std::vector<XrTime> stage_changes_;std::uint64_t stage_epoch_=1,tracking_sequence_=0;
    XrTime stage_epoch_begin_time_=0;
    MotionSync motion_sync_;
    xr_detail::CurrentTracking current_tracking_;
    std::atomic_bool timestamped_tracking_{false};
    bool stage_bounds_valid_=false,stage_locatable_=false;XrExtent2Df stage_bounds_{};
    std::unique_ptr<xr_detail::SteamVrBoundary> boundary_;
    bool boundary_requested_=false;
    xr_detail::IpdSampling ipd_fallback_{1'000'000'000};
    frame_input::SystemInputClient system_input_;
    ShortcutGesture shortcuts_;
    bool shortcut_vr_requested_=false;
    FrameTimingRecord shortcut_timing_;
    std::array<TrackingProfile,2> shortcut_profiles_{};
    VkInstance vk_instance_=VK_NULL_HANDLE;VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDevice device_=VK_NULL_HANDLE;VkQueue queue_=VK_NULL_HANDLE;std::uint32_t queue_family_=0;
    VkCommandPool pool_=VK_NULL_HANDLE;VkCommandBuffer command_=VK_NULL_HANDLE;VkFence fence_=VK_NULL_HANDLE;
    bool copy_pending_=false;
    Surface projection_surface_;
    Surface transition_surface_;
    StartupHud startup_hud_;bool startup_hud_presented_=false;
    std::uint64_t transition_generation_=0;
    unsigned transition_card_kind_=0,transition_animation_phase_=12,transition_language_=2;
    std::array<Surface,2> vr_surfaces_;
    std::array<Surface,2> vr_focus_surfaces_;
    std::array<std::array<float,4>,2> vr_focus_rects_{};
    bool vr_foveated_=false,vr_linear_layout_=false;
    std::array<Surface,2> vr_unwarped_surfaces_;
    std::array<Surface,2> vr_enhanced_surfaces_;
    std::unique_ptr<FoveationRenderer> foveation_renderer_;
    std::unique_ptr<ImageEnhancementRenderer> enhancement_renderer_;
    bool vr_render_pending_=false,vr_measuring_frame_=false;
    bool vr_timewarp_reconstruct_=false;
    std::array<xr_detail::RotationalTimewarp,2> vr_timewarp_{};
    void late_timewarp(XrTime,std::array<XrCompositionLayerProjectionView,2>&);
    bool vr_pending_reconstruction_=false,vr_pending_enhancement_=false;
    FrameTimingRecorder timing_;
    std::vector<Acquired> vr_pending_uploads_;
    // A sampled decoder allocation must remain immutable through reconstruction,
    // which can outlive the earlier upload fence and next frame's CPU staging.
    std::array<std::shared_ptr<const VideoFrame>,4> vr_pending_source_frames_{};
    struct MotionFeedback {
        std::uint64_t epoch=0,timestamp=0,query_ns=0;
        std::uint64_t submitted_ns=0,display_ns=0;
    };
    ImageQualityPreferences image_quality_;
    bool desktop_nv12_fused_=true;
    std::uint64_t content_serial_=0;
    bool vr_enhancement_active_=false,vr_upscale_active_=false;
    bool vr_fused_sharpen_=false;
    PostprocessPlan vr_postprocess_plan_{};
    std::shared_ptr<const VrStereoFrame> vr_released_pair_;
    Clock::time_point vr_previous_submission_{};
    bool have_vr_submission_time_=false;
    std::unique_ptr<ProjectionRenderer> projection_renderer_;
    bool desktop_measuring_frame_=false;
    Clock::time_point desktop_previous_submission_{};
    std::array<Clock::time_point,4> desktop_previous_new_submission_{};
    std::array<bool,2> previous_right_click_{};
    bool previous_left_system_=false;
    std::array<Surface,12> surfaces_; // four monitors, keyboard, two pointers, four transparent control overlays
    XrSessionState state_=XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blend_=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool running_=false,finished_=false,exit_requested_=false,used_=false;
    std::uint32_t max_width_=0,max_height_=0,max_image_=0,max_layers_=0;
    VkFormat format_=VK_FORMAT_UNDEFINED;
    XrDisplayStats stats_;Clock::time_point started_{},previous_frame_{};
    Surface performance_surface_;
    Surface mini_hud_surface_;
    InputPreferences mini_hud_preferences_;
    MiniHudVisibility mini_hud_visibility_;
    UiPose mini_hud_gaze_;
    std::uint64_t mini_hud_gaze_ns_=0,mini_hud_gaze_query_ns_=0,mini_hud_generation_=0;
    Clock::time_point next_mini_hud_update_{};
    bool mini_hud_texture_values_=false;
    Surface gaze_debug_surface_;
    PerformanceOverlay performance_;
    SurfxrPerformance surfxr_performance_;
    std::uint64_t local_render_query_ns_=0;
    VrPresentationPacer vr_pacer_;
    std::uint64_t vr_previous_wait_return_ns_=0,vr_previous_predicted_display_ns_=0;
    std::int64_t vr_previous_display_period_=0;
    BandwidthOverlay bandwidth_;
    bool performance_enabled_=false;
    Clock::time_point next_performance_update_{};
    std::uint64_t performance_generation_=0;
    VrUiModel ui_;UiPose head_pose_;std::vector<XrPanelSource> sources_;XrUiCallbacks callbacks_;
    bool renderable_notified_=false,last_renderable_notification_=false;
    void notify_renderable(bool value);
    std::optional<UiLayout> initial_layout_;
    std::array<UiHand,2> ui_hands_{};
    std::uint64_t keyboard_revision_=0,keyboard_generation_=0;
    unsigned keyboard_language_=2;
    std::array<std::uint64_t,4> controls_revision_{},controls_generation_{};
    bool keyboard_input_active_=false,candidate_visible_=false,candidate_presented_=false;
    std::shared_ptr<const KeyboardImage> candidate_image_;std::uint64_t candidate_generation_=0;
    bool pc_input_blocked_=false;
    bool refresh_rate_extension_=false,vr_was_active_=false,have_vr_timestamp_=false;
    PFN_xrConvertTimespecTimeToTimeKHR timespec_to_xr_=nullptr;
    PFN_xrGetDisplayRefreshRateFB get_refresh_rate_=nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_refresh_rate_=nullptr;
    PFN_xrSetPerformanceMetricsStateMETA set_metrics_state_=nullptr;
    PFN_xrQueryPerformanceMetricsCounterMETA query_metrics_counter_=nullptr;
    XrPath render_latency_path_=XR_NULL_PATH;
    Clock::time_point next_metrics_query_{},last_metrics_success_{};
    GazeHudState gaze_hud_{};
    bool gaze_debug_enabled_=false;
    std::array<UiPose,2> gaze_debug_eye_poses_{};
    std::array<std::array<float,4>,2> gaze_debug_fov_{};
    Clock::time_point next_battery_query_{};
    float last_refresh_preference_=0,last_reported_refresh_=0;
    std::uint64_t last_refresh_reply_=0;

    void create_xr();

    void create_vulkan();

    void create_session();

    void create_swapchain(Surface& s,bool request_raw_views=false);

    void create_transfer(Surface& s,std::size_t requested_bytes=0);

    void create_commands();

    void record_copy(Surface& s,std::uint32_t index);
    static bool needs_upload(const Acquired& a);
    void convert_desktop_nv12(const std::vector<Acquired>& acquired,const std::vector<unsigned>& projected);
    void complete_desktop_nv12(const std::vector<Acquired>& acquired,bool covered);
    void stage_desktop_nv12(Surface&,const VideoFrame&,const XrPanelSource&,unsigned,std::shared_ptr<const VideoFrame>);
    void complete_copy_batch(const std::vector<Acquired>& acquired,bool covered_by_render);
    void copy_batch(const std::vector<Acquired>& acquired,bool defer_completion);

    void haptic_request(unsigned hand,float amplitude,std::uint64_t duration_ns,float frequency=0) noexcept;
    void stop_haptics() noexcept;
    void update_haptics(bool focused);
    void events();


    XrPath path(const char* value);
    XrAction action(const char* name,const char* label,XrActionType type);
    void suggest(const char* profile,bool simple,bool frame,bool index);
    void create_actions();
    static UiPose ui_pose(const XrPosef& p);
    static XrPosef xr_pose(UiPose p);
    void dispatch(const UiEvent& e);
    void block_pc_input(bool blocked);
    void allocate_curve_segments();
    void refresh_stage_bounds();
    TrackingPose locate_tracking(XrSpace tracked,XrTime time,bool& target_valid);
    void create_gaze_action();
    void sample_gaze(TrackingFrame& tracking,XrTime local_display_time);
    void mini_hud_layer(std::vector<Layer>& layers,std::vector<Acquired>& acquired,
                        const std::atomic_bool& stop,bool has_vr,unsigned reserved_layers);
    unsigned gaze_debug_layers(std::vector<Layer>& layers,std::vector<Acquired>& acquired,const std::atomic_bool& stop);
    static TrackingProfile tracking_profile(const std::string& name);
    void discover_performance_metrics() noexcept;
    void start_performance_metrics() noexcept;
    void update_performance_metrics() noexcept;
    void publish_surfxr_performance() noexcept;
    void observe_refresh_rate(float actual);
    void update_refresh_rate(const RemoteSettingsSnapshot* settings);
    void input(XrTime local_display_time);
    void clear_surface(Surface& s) noexcept;
    void stage(Surface& s,const VideoFrame& f,const XrPanelSource* source=nullptr,unsigned panel=0,
               std::shared_ptr<const VideoFrame> owner={});
    std::uint64_t predicted_monotonic_ns(XrTime target);
    std::uint64_t wait_vr_presentation(std::uint64_t wait_return,std::uint64_t display,
                                     std::int64_t period,const VrStereoFrame& initial,
                                     const std::atomic_bool& stop,bool next_interval=false);
    bool stage_performance();
    void stage_ui();
    void acquire(Surface& s,std::vector<Acquired>& acquired,const std::atomic_bool& stop);
    XrCompositionLayerQuad quad(Surface& s,UiPose pose,float width,float height);
    void panel_layers(unsigned id,std::vector<Layer>& layers);
    XrCompositionLayerQuad controls_layer(unsigned id,const UiPanelControls& controls);
    static UiVec3 add3(UiVec3 a,UiVec3 b);
    static UiVec3 mul3(UiVec3 p,float v);
    static UiVec3 cross(UiVec3 a,UiVec3 b);
    static UiVec3 norm(UiVec3 p);
    static UiQuat axes(UiVec3 x,UiVec3 y,UiVec3 z);
    void pointer_layers(unsigned hand,std::vector<Layer>& layers,bool include_laser);
    void create_projection();
    bool project(const std::vector<unsigned>& ids,XrTime time,std::vector<Acquired>& acquired,
                 const std::atomic_bool& stop,std::array<XrCompositionLayerProjectionView,2>& out);
    bool valid_vr_pair(const VrStereoFrame& pair) const;
    void invalidate_vr_reuse();
    bool reusable_vr_pair(const std::shared_ptr<const VrStereoFrame>& pair) const;
    void vr_projection_views(const VrStereoFrame& pair,std::array<XrCompositionLayerProjectionView,2>& views);
    void stage_transition(bool vr_requested);
    void reset_vr_surfaces();
    void stage_vr(const VrStereoFrame& pair,std::vector<Acquired>& acquired,const std::atomic_bool& stop,
                  std::array<XrCompositionLayerProjectionView,2>& views);
    void record_foveation_completion(bool count_wait);
    void observe_motion_feedback(const MotionFeedback& frame);
    void record_enhancement_completion();
    void complete_vr_upload();
    void complete_vr_render();
    bool reconstruct_vr(const std::vector<Acquired>& acquired,bool allow_deferred);
    void frame(const std::atomic_bool& stop);
    ~Impl();
    void initialize();
    XrDisplayStats run(const std::vector<XrPanelSource>& panels,std::atomic_bool& stop,unsigned seconds,const XrUiCallbacks& callbacks,const UiLayout* initial_layout);
};
} // namespace surfxr
