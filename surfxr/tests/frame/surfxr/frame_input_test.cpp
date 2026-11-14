#include "wire.hpp"
#include "controller_motion.hpp"
#include "../../../frame/xr/controller_status.hpp"
#include "../../../frame/xr/controller_tracking.hpp"
#include <limits>
#include <map>
#include <set>

using namespace surfxr;
using namespace surfxr::transport;
namespace {
void controller_prediction_checks(){
    TrackingFrame frame;frame.prediction_time_valid=true;
    frame.query_xr_time=1'000'000'000;frame.predicted_xr_time=1'060'000'000;
    require(xr_detail::controller_tracking_time(frame,0,0)==frame.query_xr_time,
        "SURFXR controllers must not inherit the 60ms head prediction before PC extrapolation");
    require(!xr_detail::controller_tracking_time(frame,1'020'000'000,0),
        "current controller query cannot use a Stage epoch already selected for future display");
    require(!xr_detail::controller_tracking_time(frame,0,frame.query_xr_time),
        "controller query at the next Stage boundary is not in the previous epoch");
    require(xr_detail::controller_tracking_time(frame,frame.query_xr_time,frame.predicted_xr_time)==frame.query_xr_time,
        "current query on this epoch's first instant is valid");
    frame.query_xr_time=0;
    require(!xr_detail::controller_tracking_time(frame,0,0),"missing XR now must not fall back to a predicted controller");
    frame.query_xr_time=1'000'000'000;frame.prediction_time_valid=false;
    require(!xr_detail::controller_tracking_time(frame,0,0),
        "invalid timing cannot publish either controller convention");
    frame.prediction_time_valid=true;frame.head.flags=OrientationValid|PositionValid;
    frame.head.linear_velocity_valid=frame.head.angular_velocity_valid=true;
    frame.head.linear_velocity={1,2,3};frame.head.angular_velocity={4,5,6};
    for(unsigned i=0;i<2;++i){auto& hand=frame.hands[i];hand.grip_active=true;hand.grip=frame.head;
        hand.grip.pose.position={float(i),2,3};hand.grip.pose.orientation={.5f,.5f,.5f,.5f};
        // Opposite signs exercise both halves of a side-to-side swing.
        const float sign=i?-1.f:1.f;
        hand.grip.linear_velocity={sign*2,sign*3,sign*4};hand.grip.angular_velocity={sign*5,sign*6,sign*7};}
    for(bool modern:{false,true}){
        const auto bytes=tracking(frame,1'005'000'000,true,modern);Reader packet(bytes,modern);
        require(packet.duration()==1'005'000'000&&packet.integer(8)==3,"upload identity and device count unchanged");
        require(packet.integer(8)==path_id("/user/head"),"head first");
        for(unsigned j=0;j<7;++j)packet.real();
        for(unsigned j=0;j<6;++j)require(packet.real()==0,"head remains client predicted without PC extrapolation");
        for(unsigned i=0;i<2;++i){
            require(packet.integer(8)==path_id(i?"/user/hand/right":"/user/hand/left"),"physical controller order");
            for(unsigned j=0;j<4;++j)require(packet.real()==.5f,"grip orientation preserved");
            require(packet.real()==float(i)&&packet.real()==2&&packet.real()==3,"grip position preserved without extra prediction");
            const float sign=i?-1.f:1.f;
            for(unsigned j=2;j<=7;++j)require(packet.real()==sign*float(j),"controller linear/angular velocities retained for prediction and throwing");
        }
        for(unsigned j=0;j<7;++j)require(!packet.boolean(),"unchanged optional wire suffix");
        packet.end();
    }
}
void connection_checks(){
    constexpr std::uint64_t now=5'000'000'000;
    xr_detail::ControllerStatus status;status.known_mask=3;status.connected_mask=3;
    status.battery_mask=3;status.levels={0.f,.73f};status.charging_known_mask=3;status.charging_mask=2;status.sampled_ns=now;
    TrackingFrame frame;frame.origin_epoch=1;frame.focused=frame.visible=true;
    for(auto& hand:frame.hands){hand.profile=TrackingProfile::SteamFrame;hand.grip_active=true;
        hand.grip.flags=OrientationValid|PositionValid;hand.grip.linear_velocity_valid=true;}
    std::array<DeviceBattery,2> batteries{};
    for(unsigned i=0;i<2;++i)status.apply(i,frame.hands[i],batteries[i],now);
    require(batteries[0].percent==0&&batteries[1].percent==73&&batteries[1].charging,"real per-role batteries, including empty, reach existing battery path");
    batteries[1].percent=81;status.apply(1,frame.hands[1],batteries[1],now);
    require(batteries[1].percent==81,"valid XR battery takes priority");
    require(!status.fresh(now-1)&&!status.fresh(now+2'000'000'001),"future or stalled bridge telemetry expires");
    auto invalid=status;invalid.levels[0]=std::numeric_limits<float>::quiet_NaN();require(!invalid.valid(),"malformed battery IPC rejected");
    invalid=status;invalid.connected_mask=4;require(!invalid.valid(),"malformed connection mask rejected");
    TrackingController legacy;legacy.profile=TrackingProfile::ValveIndex;DeviceBattery legacy_battery;
    status.apply(0,legacy,legacy_battery,now);require(!legacy.connection_known&&legacy_battery.percent<0,"native Frame status cannot disconnect another controller type");
    ControllerMotionCache cache;cache.apply(frame,{true,true},true);
    frame.focused=false;cache.apply(frame,{false,false},true);
    require(frame.hands[0].grip_active&&!frame.hands[0].grip.linear_velocity_valid,"focus loss freezes pose without powering controller off");
    status.connected_mask=status.battery_mask=status.charging_known_mask=status.charging_mask=2;
    status.apply(0,frame.hands[0],batteries[0],now);
    require(frame.hands[0].connection_known&&!frame.hands[0].connected&&batteries[0].percent<0,"physical disconnect invalidates old battery");
    cache.apply(frame,{false,false},true);
    require(!frame.hands[0].grip_active&&frame.hands[1].grip_active,"power-off clears only that hand's cached pose");
    for(bool modern:{false,true}){
        const auto bytes=tracking(frame,now,true,modern);Reader packet(bytes,modern);
        require(packet.duration()==now&&packet.integer(8)==2,"powered-off hand omitted from unchanged tracking protocol");
        require(packet.integer(8)==path_id("/user/head"),"head remains first");
        for(unsigned i=0;i<13;++i)packet.real();
        require(packet.integer(8)==path_id("/user/hand/right"),"other physical hand retained");
    }
    frame.hands[0].connected=true;cache.apply(frame,{false,false},true);
    require(!frame.hands[0].grip_active,"wake-up cannot resurrect pre-shutdown pose");
    frame.hands[0].grip.flags=OrientationValid|PositionValid;frame.hands[0].grip_active=true;
    cache.apply(frame,{true,false},true);require(frame.hands[0].grip_active,"new valid pose reconnects automatically");
    const auto local_frame=frame;
    cache.apply(frame,{false,false},false);
    require(!frame.hands[0].grip_active&&!frame.hands[1].grip_active,"hot disable clears both cached controller poses");
    for(bool modern:{false,true}){
        const auto bytes=tracking(frame,now,true,modern);Reader packet(bytes,modern);
        require(packet.duration()==now&&packet.integer(8)==1,"hot disable emits only the head just like both controllers powered off");
    }
    frame=local_frame;cache.apply(frame,{false,false},true);
    require(!frame.hands[0].grip_active&&!frame.hands[1].grip_active,"hot enable during focus loss cannot revive old poses");
    frame=local_frame;cache.apply(frame,{true,true},true);
    require(frame.hands[0].grip_active&&frame.hands[1].grip_active,"new valid poses reconnect hot-enabled controllers");
    ++frame.origin_epoch;cache.apply(frame,{false,false},true);require(!frame.hands[0].grip_active,"old-stage cache still expires");
}
void skeleton_checks(){
    TrackingFrame frame;frame.focused=frame.visible=true;frame.hands[1].profile=TrackingProfile::SteamFrame;
    auto& source=frame.controller_skeletons[1];source.sampled_monotonic_ns=1'000'000'000;
    for(unsigned i=0;i<31;++i)source.bones[i].position.x=i*.001f;
    const auto check_packet=[&](bool enabled,std::uint64_t now,bool active){
        const auto packet=frame_skeleton(frame,1,enabled,now);Reader outer(packet,true);
        require(outer.integer(4)==11,"bones use existing v21 ReservedBuffer");const auto payload=outer.blob();outer.end();Reader reader(payload);
        require(payload.size()==(active?892u:24u)&&reader.integer(4)==0x5644534b&&reader.integer(4)==1&&
            reader.integer(4)==1&&reader.integer(4)==unsigned(active),"bounded parent-relative skeleton header");
        require(reader.integer(8)==(active?source.sampled_monotonic_ns:0),"source age remains observable");
        if(active)for(unsigned i=0;i<31;++i){require(reader.real()==i*.001f,"all 31 native joints preserved");
            for(unsigned j=0;j<5;++j)require(reader.real()==0,"parent-relative position and quaternion");
            require(reader.real()==1,"quaternion identity");}
        reader.end();
    };
    check_packet(true,source.sampled_monotonic_ns,true); // capture is OFF; bones remain active.
    check_packet(true,source.sampled_monotonic_ns+100'000'001,false);
    check_packet(false,source.sampled_monotonic_ns,false);
    frame.hands[1].connection_known=true;frame.hands[1].connected=false;
    check_packet(true,source.sampled_monotonic_ns,false);frame.hands[1].connected=true;
    frame.focused=false;check_packet(true,source.sampled_monotonic_ns,false);frame.focused=true;
    source.bones[30].orientation.w=std::numeric_limits<float>::quiet_NaN();check_packet(true,source.sampled_monotonic_ns,false);
}
std::map<std::uint64_t,float> decode(const TrackingFrame& frame,bool enabled=true){
    const auto bytes=buttons(frame,enabled,true,true);Reader reader(bytes,true);
    require(reader.integer(4)==6&&reader.integer(8)==48,"frame native button shape");
    std::map<std::uint64_t,float> values;
    for(unsigned i=0;i<48;++i){const auto id=reader.integer(8),type=reader.integer(4);
        require(type<=1,"frame component type");values[id]=type?reader.real():float(reader.boolean());}
    reader.end();require(values.size()==48,"unique native component identities");return values;
}
}
void frame_input_checks(){
    controller_prediction_checks();
    connection_checks();
    skeleton_checks();
    std::set<std::uint64_t> advertised;
    for(unsigned hand=0;hand<2;++hand){
        const auto bytes=profile(hand,true,true);Reader reader(bytes,true);
        require(reader.integer(4)==7&&reader.integer(8)==path_id(hand?"/user/hand/right":"/user/hand/left")&&
            reader.integer(8)==path_id("/interaction_profiles/valve/frame_controller_valve"),"native Frame profile identity");
        require(reader.integer(8)==48,"both hands declared for global mapping");
        std::set<std::uint64_t> ids;for(unsigned i=0;i<48;++i)ids.insert(reader.integer(8));reader.end();
        if(hand)require(ids==advertised,"native global mapping survives second profile");else advertised=std::move(ids);
    }
    TrackingFrame frame;frame.focused=frame.visible=frame.system_capture_active=true;
    for(auto& hand:frame.hands){hand.profile=TrackingProfile::SteamFrame;hand.buttons_active=hand.touches_active=0xffff;hand.analog_active=7;}
    const std::array<std::pair<const char*,std::uint32_t>,16> physical_buttons{{
        {"a",ButtonA},{"b",ButtonB},{"x",ButtonX},{"y",ButtonY},{"thumbstick",ButtonThumbstick},
        {"trigger",ButtonTrigger},{"squeeze",ButtonSqueeze},{"menu",ButtonMenu},{"view",ButtonView},
        {"bumper",ButtonBumper},{"dpad_up",ButtonDpadUp},{"dpad_down",ButtonDpadDown},
        {"dpad_left",ButtonDpadLeft},{"dpad_right",ButtonDpadRight},{"thumbrest",ButtonThumbrest},{"system",ButtonSystem}}};
    for(unsigned hand=0;hand<2;++hand)for(auto button:physical_buttons)for(bool touch:{false,true}){
        frame.hands[hand].buttons=touch?0:button.second;frame.hands[hand].touches=touch?button.second:0;
        const auto values=decode(frame);const auto path=std::string(hand?"/user/hand/right/input/":"/user/hand/left/input/")+button.first+(touch?"/touch":"/click");
        unsigned pressed=0;for(const auto& entry:values)if(entry.second){++pressed;require(entry.first==path_id(path),"physical input never crosses hands or aliases system");}
        require(pressed==unsigned(advertised.count(path_id(path))),"each declared native input forwarded");
        frame.hands[hand].buttons=frame.hands[hand].touches=0;
    }
    frame.hands[0].trigger=.25f;frame.hands[0].squeeze=.75f;frame.hands[0].stick_x=-.5f;frame.hands[0].stick_y=.125f;
    auto values=decode(frame);require(values[path_id("/user/hand/left/input/trigger/value")]==.25f&&
        values[path_id("/user/hand/left/input/squeeze/value")]==.75f&&values[path_id("/user/hand/left/input/thumbstick/x")]==-.5f,"raw analog values preserved");
    frame.hands[0].trigger=std::numeric_limits<float>::quiet_NaN();require(decode(frame)[path_id("/user/hand/left/input/trigger/value")]==0,"nonfinite analog is neutral");
    frame.hands[0].buttons=frame.hands[0].touches=ButtonSystem|ButtonBumper;frame.ui_captured[0]=true;
    values=decode(frame);require(values[path_id("/user/hand/left/input/system/click")]==1&&values[path_id("/user/hand/left/input/system/touch")]==1&&
        values[path_id("/user/hand/left/input/bumper/click")]==0,"system routed during local UI capture only");
    frame.system_capture_active=false;require(decode(frame)[path_id("/user/hand/left/input/system/click")]==0,"normal mode reserves system to headset");
    frame.focused=false;for(auto value:decode(frame))require(value.second==0,"focus loss sends all releases");
    frame.focused=true;for(auto value:decode(frame,false))require(value.second==0,"disabled forwarding sends all releases");
    frame.hands[0].connection_known=true;frame.hands[0].connected=false;
    for(auto value:decode(frame))require(value.second==0,"physical disconnect releases buttons even if XR reports stale active input");
    require(buttons(surfxr::TrackingFrame{},true,false,true)==buttons(surfxr::TrackingFrame{},true,false),"v20 bytes unchanged by native option");
}
