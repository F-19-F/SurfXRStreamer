#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::create_gaze_action(){
    if(!stats_.eye_gaze_supported)return;
    // Gaze has no hand subactions; never use the controller action helper.
    auto create=xr_info<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
    std::strcpy(create.actionName,"eye_gaze");std::strcpy(create.localizedActionName,"Eye gaze for streaming");
    create.actionType=XR_ACTION_TYPE_POSE_INPUT;
    xr_check(xrCreateAction(action_set_,&create,&gaze_action_),"xrCreateAction(gaze)");
    const XrActionSuggestedBinding binding{gaze_action_,path("/user/eyes_ext/input/gaze_ext/pose")};
    auto suggest=xr_info<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    suggest.interactionProfile=path("/interaction_profiles/ext/eye_gaze_interaction");
    suggest.countSuggestedBindings=1;suggest.suggestedBindings=&binding;
    const auto result=xrSuggestInteractionProfileBindings(instance_,&suggest);
    if(XR_FAILED(result)){stats_.eye_gaze_supported=false;return;}
    auto space=xr_info<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
    space.action=gaze_action_;space.poseInActionSpace.orientation.w=1;
    xr_check(xrCreateActionSpace(session_,&space,&gaze_space_),"xrCreateActionSpace(gaze)");
}
void XrDisplay::Impl::sample_gaze(TrackingFrame& tracking,XrTime local_display_time){
    mini_hud_gaze_ns_=mini_hud_gaze_query_ns_=0;
    if(!gaze_space_||!tracking.focused)return;
    auto get=xr_info<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);get.action=gaze_action_;
    auto state=xr_info<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
    if(xrGetActionStatePose(session_,&get,&state)!=XR_SUCCESS||!state.isActive){++stats_.eye_gaze_invalid;return;}
    auto location=xr_info<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    constexpr auto valid=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if(mini_hud_preferences_.mini_hud_enabled&&mini_hud_preferences_.mini_hud_gaze_only){
        // Local HUD hit testing uses the full VIEW-space ray at the local
        // display target, independent of clipped foveation wire coordinates.
        auto sampled=xr_info<XrEyeGazeSampleTimeEXT>(XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT);
        auto local=xr_info<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        local.next=&sampled;
        constexpr auto tracked=valid|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        if(xrLocateSpace(gaze_space_,head_space_,local_display_time,&local)==XR_SUCCESS&&
           (local.locationFlags&tracked)==tracked){
            TrackingPose ray;ray.pose=ui_pose(local.pose);ray.flags=OrientationValid|PositionValid;
            const auto sample=sampled.time?predicted_monotonic_ns(sampled.time):0;
            if(tracking_pose_valid(ray)&&(!sampled.time||sample)){
                mini_hud_gaze_=ray.pose;mini_hud_gaze_ns_=sample;mini_hud_gaze_query_ns_=clock_ns(Clock::now());
            }
        }
    }
    if(!tracking.optics_valid||!tracking.prediction_time_valid)return;
    if(xrLocateSpace(gaze_space_,stage_space_,tracking.predicted_xr_time,&location)!=XR_SUCCESS||
       (location.locationFlags&valid)!=valid){++stats_.eye_gaze_invalid;return;}
    TrackingPose gaze;gaze.pose=ui_pose(location.pose);gaze.flags=OrientationValid|PositionValid;
    for(unsigned eye=0;eye<2;++eye)
        tracking.gaze.valid[eye]=project_gaze(gaze,tracking.eye_poses[eye],tracking.eye_fov[eye],tracking.gaze.positions[eye]);
    tracking.gaze.sampled_monotonic_ns=tracking.sampled_monotonic_ns;
    tracking.gaze.sequence=tracking.sequence;tracking.gaze.origin_epoch=tracking.origin_epoch;
    if(tracking.gaze.valid[0]||tracking.gaze.valid[1])++stats_.eye_gaze_samples;else ++stats_.eye_gaze_invalid;
}
unsigned XrDisplay::Impl::gaze_debug_layers(std::vector<Layer>& layers,std::vector<Acquired>& acquired,const std::atomic_bool& stop){
    std::array<UiPose,2> poses{};std::array<bool,2> valid{};const auto now=clock_ns(Clock::now());
    for(unsigned eye=0;eye<2;++eye)
        valid[eye]=GazeHud::marker_pose(gaze_hud_,eye,now,gaze_debug_eye_poses_[eye],gaze_debug_fov_[eye],poses[eye]);
    if(!valid[0]&&!valid[1])return 0;
    // The image is immutable: acquire/upload once, then change only layer poses.
    if(!gaze_debug_surface_.have_frame){stage(gaze_debug_surface_,GazeHud::marker_texture());acquire(gaze_debug_surface_,acquired,stop);}
    for(unsigned eye=0;eye<2;++eye)if(valid[eye]){
        auto marker=quad(gaze_debug_surface_,poses[eye],.022f,.022f);marker.space=head_space_;
        marker.eyeVisibility=eye?XR_EYE_VISIBILITY_RIGHT:XR_EYE_VISIBILITY_LEFT;
        marker.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        layers.emplace_back(marker);
    }
    return unsigned(valid[0])+unsigned(valid[1]);
}
} // namespace surfxr
