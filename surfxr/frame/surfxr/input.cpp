#include "wire.hpp"
#include <algorithm>
#include <cmath>

namespace surfxr::transport {
namespace {
const char* hand_path(unsigned hand){return hand?"/user/hand/right":"/user/hand/left";}
enum Input {Face1Click,Face2Click,Face1Touch,Face2Touch,MenuClick,TriggerClick,TriggerTouch,
    SqueezeClick,StickClick,StickTouch,RestTouch,TriggerValue,SqueezeValue,StickX,StickY,InputCount};
const std::array<std::array<std::uint64_t,InputCount>,2>& input_ids(){
    // These protocol paths never change. Hash once instead of constructing and
    // hashing 29 strings at each 360 Hz upload; retain packet order below.
    static const auto ids=[] {
        std::array<std::array<std::uint64_t,InputCount>,2> result{};
        const std::array<const char*,InputCount> suffixes{"x/click","y/click","x/touch","y/touch","menu/click",
            "trigger/click","trigger/touch","squeeze/click","thumbstick/click","thumbstick/touch","thumbrest/touch",
            "trigger/value","squeeze/value","thumbstick/x","thumbstick/y"};
        const std::array<const char*,4> right_faces{"a/click","b/click","a/touch","b/touch"};
        for(unsigned hand=0;hand<2;++hand)for(unsigned input=0;input<InputCount;++input){
            const auto* suffix=hand&&input<=Face2Touch?right_faces[input]:suffixes[input];
            const auto path=std::string(hand_path(hand))+"/input/"+suffix;
            result[hand][input]=path_id(path);
        }
        return result;
    }();
    return ids;
}
void vec(Writer& w,const UiVec3& p){w.real(p.x);w.real(p.y);w.real(p.z);}
void motion(Writer& w,std::uint64_t id,const TrackingPose& p,bool head=false){
    w.integer(id,8);const auto q=p.pose.orientation;w.real(q.x);w.real(q.y);w.real(q.z);w.real(q.w);vec(w,p.pose.position);
    // Legacy SURFXR expects an already predicted head with zero velocities.
    // Negotiated present-time tracking preserves both head and hand velocities.
    // Controllers are sampled at XR now: retain velocities for PC prediction
    // and game physics (e.g. throwing), rather than masking double prediction.
    vec(w,!head&&p.linear_velocity_valid?p.linear_velocity:UiVec3{});
    vec(w,!head&&p.angular_velocity_valid?p.angular_velocity:UiVec3{});
}
}
Bytes tracking(const TrackingFrame& f,std::uint64_t timestamp,bool controllers,bool v21,bool pose_time){
    Writer w(v21);w.duration(timestamp);
    std::array<const TrackingPose*,2> hands{};unsigned count=1;
    if(controllers)for(unsigned i=0;i<2;++i){const auto& c=f.hands[i];
        if(tracking_controller_connected(c)&&c.grip_active&&tracking_pose_valid(c.grip)){hands[i]=&c.grip;++count;}
    }
    static const auto head_id=path_id("/user/head");
    static const std::array<std::uint64_t,2> hand_ids{path_id(hand_path(0)),path_id(hand_path(1))};
    w.integer(count,8);motion(w,head_id,f.head,!pose_time);
    for(unsigned i=0;i<2;++i)if(hands[i])motion(w,hand_ids[i],*hands[i]);
    // No hand skeleton or face extensions are advertised. SURFXR v20 wire suffix.
    for(unsigned i=0;i<7;++i)w.boolean(false);
    return std::move(w.bytes);
}
bool views_changed(const TrackingFrame& current,const TrackingFrame& last) noexcept {
    // xrLocateViews reports world-space eye positions. Their subtraction can
    // jitter in the last float bits during head motion; those bits are not an
    // optical change. This predicate covers immediate changes; ViewUpdates
    // owns directional fine-adjustment detection and rate limiting. A retained Frame XR
    // session also alternates its FOV by about 0.2 mrad after desktop/VR handoff;
    // do not turn that sub-pixel noise into SteamVR lens changes every frame.
    if(!last.optics_valid||current.origin_epoch!=last.origin_epoch||
       std::abs(current.ipd_meters-last.ipd_meters)>.001f)return true;
    for(unsigned eye=0;eye<2;++eye)for(unsigned angle=0;angle<4;++angle)
        if(std::abs(current.eye_fov[eye][angle]-last.eye_fov[eye][angle])>.001f)return true;
    return false;
}
Bytes views(const TrackingFrame& f,bool v21){
    Writer w(v21);w.integer(4,4);if(!v21)w.real(f.ipd_meters);
    for(unsigned i=0;i<2;++i){
        if(v21){w.real(0);w.real(0);w.real(0);w.real(1);w.real(f.ipd_meters*(i?.5f:-.5f));w.real(0);w.real(0);}
        for(float v:f.eye_fov[i])w.real(v);
    }
    return std::move(w.bytes);
}
Bytes playspace(const TrackingFrame& f,bool v21){
    Writer w(v21);w.integer(0,4);w.boolean(f.stage_bounds_valid);
    if(f.stage_bounds_valid){w.real(f.stage_width);w.real(f.stage_depth);}return std::move(w.bytes);
}
Bytes profile(unsigned hand,bool v21,bool native){
    if(v21&&native)return frame_profile(hand);
    Writer w(v21);w.integer(v21?7:8,4);w.integer(path_id(hand_path(hand)),8);w.integer(path_id("/interaction_profiles/oculus/touch_controller"),8);
    if(v21){
        // master replaces one global mapping manager for each profile packet.
        // Every declaration must therefore include both hands' active inputs.
        std::vector<std::string> paths;
        for(unsigned i=0;i<2;++i){
            const auto add=[&](const char* path){paths.emplace_back(std::string(hand_path(i))+"/input/"+path);};
            for(const auto* path:{"trigger/click","trigger/touch","squeeze/click","thumbstick/click","thumbstick/touch","thumbrest/touch","trigger/value","squeeze/value","thumbstick/x","thumbstick/y"})add(path);
            for(const char* path:i?std::vector<const char*>{"a/click","b/click","a/touch","b/touch"}:std::vector<const char*>{"x/click","y/click","x/touch","y/touch","menu/click"})add(path);
        }
        w.integer(paths.size(),8);for(const auto& path:paths)w.integer(path_id(path),8);
    }
    return std::move(w.bytes);
}
Bytes battery(std::uint64_t device,float level,bool charging,bool v21){Writer w(v21);w.integer(5,4);w.integer(device,8);w.real(level);w.boolean(charging);return std::move(w.bytes);}
Bytes buttons(const TrackingFrame& f,bool enabled,bool v21,bool native){
    if(v21&&native)return frame_buttons(f,enabled);
    auto raw=f;
    for(unsigned i=0;i<2;++i)if(raw.ui_captured[i]){
        // The physical Frame left View/system key opens the remote SteamVR
        // dashboard even while the local UI owns trigger/stick input.
        const auto keep=i==0&&raw.hands[i].profile==TrackingProfile::SteamFrame?ButtonView|ButtonSystem:0u;
        raw.hands[i].buttons&=keep;raw.hands[i].buttons_active&=keep;
        raw.hands[i].touches=raw.hands[i].touches_active=raw.hands[i].analog_active=0;
    }
    // Both hands always send their complete state, including inactive releases.
    // Write the fixed count first so no intermediate entries buffer is needed.
    Writer entries(v21);entries.bytes.reserve(512);entries.integer(v21?6:7,4);entries.integer(29,8);
    const auto hands=touch_controller_mapping(raw);
    const auto& ids=input_ids();
    for(unsigned i=0;i<2;++i){const auto& c=hands[i];
        const bool active=enabled&&f.focused&&f.visible&&tracking_controller_connected(f.hands[i]);
        const auto binary=[&](Input input,std::uint32_t bit,bool touch=false){
            entries.integer(ids[i][input],8);entries.integer(0,4);
            entries.boolean(active&&((touch?c.touches_active:c.buttons_active)&bit)&&((touch?c.touches:c.buttons)&bit));
        };
        const auto scalar=[&](Input input,float value,std::uint32_t bit){
            entries.integer(ids[i][input],8);entries.integer(1,4);
            entries.real(active&&(c.analog_active&bit)?std::clamp(value,-1.f,1.f):0.f);
        };
        if(i){binary(Face1Click,ButtonA);binary(Face2Click,ButtonB);binary(Face1Touch,ButtonA,true);binary(Face2Touch,ButtonB,true);}
        else{binary(Face1Click,ButtonX);binary(Face2Click,ButtonY);binary(Face1Touch,ButtonX,true);binary(Face2Touch,ButtonY,true);binary(MenuClick,ButtonMenu);}
        binary(TriggerClick,ButtonTrigger);binary(TriggerTouch,ButtonTrigger,true);
        binary(SqueezeClick,ButtonSqueeze);binary(StickClick,ButtonThumbstick);binary(StickTouch,ButtonThumbstick,true);binary(RestTouch,ButtonThumbrest,true);
        scalar(TriggerValue,std::clamp(c.trigger,0.f,1.f),TriggerActive);scalar(SqueezeValue,std::clamp(c.squeeze,0.f,1.f),SqueezeActive);
        scalar(StickX,c.stick_x,ThumbstickActive);scalar(StickY,c.stick_y,ThumbstickActive);
    }
    return std::move(entries.bytes);
}
}
