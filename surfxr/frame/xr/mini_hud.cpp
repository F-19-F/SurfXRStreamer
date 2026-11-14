#include "display_private.hpp"

namespace surfxr {
void XrDisplay::Impl::mini_hud_layer(std::vector<Layer>& layers,std::vector<Acquired>& acquired,
                                    const std::atomic_bool& stop,bool has_vr,unsigned reserved_layers){
    const auto& preferences=mini_hud_preferences_;
    if(!has_vr||!preferences.mini_hud_enabled){mini_hud_visibility_.reset();return;}
    const auto now=Clock::now();const auto ns=clock_ns(now);
    const auto layout=MiniHud::layout(preferences);
    const bool fresh=MiniHudVisibility::fresh_sample(mini_hud_gaze_ns_,mini_hud_gaze_query_ns_,ns);
    const auto visibility=mini_hud_visibility_.update(preferences,layout,mini_hud_gaze_,fresh,ns);
    if((!visibility.values&&!visibility.marker)||layers.size()+reserved_layers+1>max_layers_)return;
    const bool changed=mini_hud_texture_values_!=visibility.values;
    if(changed||!mini_hud_surface_.have_frame||now>=next_mini_hud_update_){
        // Geometry is cropped from a fixed allocation. The last released image
        // remains valid between 4 Hz updates, including an unchanged marker.
        if(changed||!mini_hud_surface_.have_frame||visibility.values||next_mini_hud_update_==Clock::time_point{}){
            const auto generation=++mini_hud_generation_;
            stage(mini_hud_surface_,visibility.values?
                MiniHud::raster(performance_,ns,generation,preferences,surfxr_performance_.snapshot(ns)):
                MiniHud::marker(preferences,generation));
            acquire(mini_hud_surface_,acquired,stop);
            mini_hud_texture_values_=visibility.values;
        }
        next_mini_hud_update_=now+std::chrono::milliseconds(250);
    }
    auto hud=quad(mini_hud_surface_,layout.pose,layout.width_m,layout.height_m);
    hud.subImage.imageRect.extent.height=static_cast<std::int32_t>(layout.active_height);
    hud.space=head_space_;
    hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT|XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    layers.emplace_back(hud);
}
} // namespace surfxr
