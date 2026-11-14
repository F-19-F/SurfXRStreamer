#include "wire.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr::transport {
namespace {
struct Component {std::uint64_t id;std::uint32_t bit;bool touch;unsigned scalar;};
const std::array<Component,48>& components(){
    static const auto table=[] {
        std::array<Component,48> result{};unsigned index=0;
        const std::array<std::pair<const char*,std::uint32_t>,5> left{{
            {"dpad_up",ButtonDpadUp},{"dpad_down",ButtonDpadDown},{"dpad_left",ButtonDpadLeft},
            {"dpad_right",ButtonDpadRight},{"view",ButtonView}}};
        const std::array<std::pair<const char*,std::uint32_t>,5> right{{
            {"a",ButtonA},{"b",ButtonB},{"x",ButtonX},{"y",ButtonY},{"menu",ButtonMenu}}};
        for(unsigned hand=0;hand<2;++hand){
            const auto prefix=std::string(hand?"/user/hand/right/input/":"/user/hand/left/input/");
            const auto binary=[&](const char* name,std::uint32_t bit){
                for(bool touch:{false,true})result[index++]={path_id(prefix+name+(touch?"/touch":"/click")),bit,touch,0};
            };
            for(auto button:hand?right:left)binary(button.first,button.second);
            for(auto button:std::array<std::pair<const char*,std::uint32_t>,5>{{
                {"bumper",ButtonBumper},{"trigger",ButtonTrigger},{"squeeze",ButtonSqueeze},
                {"thumbstick",ButtonThumbstick},{"system",ButtonSystem}}})binary(button.first,button.second);
            unsigned scalar=0;
            for(const auto* name:{"trigger/value","squeeze/value","thumbstick/x","thumbstick/y"})
                result[index++]={path_id(prefix+name),scalar<2?(scalar?SqueezeActive:TriggerActive):ThumbstickActive,false,++scalar};
        }
        return result;
    }();
    return table;
}
}
Bytes frame_profile(unsigned hand){
    Writer w(true);w.integer(7,4);w.integer(path_id(hand?"/user/hand/right":"/user/hand/left"),8);
    w.integer(path_id("/interaction_profiles/valve/frame_controller_valve"),8);
    w.integer(components().size(),8);for(const auto& c:components())w.integer(c.id,8);
    return std::move(w.bytes);
}
Bytes frame_buttons(const TrackingFrame& f,bool enabled){
    Writer w(true);w.bytes.reserve(768);w.integer(6,4);w.integer(components().size(),8);
    for(unsigned index=0;index<components().size();++index){
        const auto& c=components()[index];const auto hand=index/24;const auto& raw=f.hands[hand];
        const bool system=c.bit==ButtonSystem&&!c.scalar;
        const bool active=enabled&&f.focused&&f.visible&&tracking_controller_connected(raw)&&raw.profile==TrackingProfile::SteamFrame&&
            (!f.ui_captured[hand]||system||c.bit==ButtonView)&&(!system||f.system_capture_active);
        w.integer(c.id,8);w.integer(c.scalar?1:0,4);
        if(!c.scalar)w.boolean(active&&((c.touch?raw.touches_active:raw.buttons_active)&c.bit)&&
            ((c.touch?raw.touches:raw.buttons)&c.bit));
        else{
            const std::array<float,4> values{raw.trigger,raw.squeeze,raw.stick_x,raw.stick_y};
            const auto value=values[c.scalar-1];
            w.real(active&&(raw.analog_active&c.bit)&&std::isfinite(value)?std::clamp(value,c.scalar<=2?0.f:-1.f,1.f):0.f);
        }
    }
    return std::move(w.bytes);
}
}
