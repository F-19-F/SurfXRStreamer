#pragma once
#include "surfxr/ports.hpp"
#include <optional>
#include "surfxr/surfxr_codec.hpp"
#include <array>

namespace surfxr::transport::desktop {
inline constexpr unsigned control_port=ports::desktop_control,media_port=ports::desktop_media,packet_size=1200;
inline constexpr std::array<std::uint8_t,8> magic{'A','L','V','R','D','S','0','1'};
// Inner messages remain SURFXR v21 bincode2. Desktop is explicitly negotiated;
// upstream VR servers cannot accidentally receive desktop input.
enum class EventKind:std::uint8_t {Release=0,Move=1,Button=2,Wheel=3,Key=4,Unicode=5};
struct Event {
    EventKind kind=EventKind::Release;
    unsigned stream=0;float u=0,v=0;int a=0,b=0;
    std::uint64_t candidate=0; // negotiated image identity, zero for ordinary input
};
inline Bytes input(const Event& e){
    Writer body(true);body.raw_integer(1,1);body.raw_integer(static_cast<unsigned>(e.kind),1);
    body.raw_integer(e.stream,1);body.real(e.u);body.real(e.v);
    body.raw_integer(static_cast<std::uint32_t>(e.a),4);body.raw_integer(static_cast<std::uint32_t>(e.b),4);
    if(e.candidate){body.bytes[0]=2;body.raw_integer(e.candidate,8);}
    Writer w(true);w.integer(11,4);w.blob(body.bytes);return w.bytes;
}
inline Event event(const Bytes& b){
    Reader r(b);require(r.integer(1)==1,"surfxr_desktop_input_version");Event e;
    e.kind=static_cast<EventKind>(r.integer(1));e.stream=static_cast<unsigned>(r.integer(1));e.u=r.real();e.v=r.real();
    e.a=static_cast<std::int32_t>(r.integer(4));e.b=static_cast<std::int32_t>(r.integer(4));r.end();
    require(e.stream<4&&e.u>=0&&e.u<=1&&e.v>=0&&e.v<=1,"surfxr_desktop_input_coordinate");
    switch(e.kind){
    case EventKind::Release:case EventKind::Move:require(e.a==0&&e.b==0,"surfxr_desktop_input_value");break;
    case EventKind::Button:require(e.a>=0&&e.a<3&&(e.b==0||e.b==1),"surfxr_desktop_input_button");break;
    case EventKind::Wheel:require(e.a>=-8192&&e.a<=8192&&e.b>=-8192&&e.b<=8192,"surfxr_desktop_input_wheel");break;
    case EventKind::Key:require((e.a>0&&e.a<=0x7f)||((e.a&0xff00)==0xe000&&(e.a&255)>0&&(e.a&255)<=0x7f),"surfxr_desktop_input_key");require(e.b==0||e.b==1,"surfxr_desktop_input_value");break;
    case EventKind::Unicode:require(e.a>0&&e.a<=65535&&(e.b==0||e.b==1),"surfxr_desktop_input_unicode");break;
    default:fail("surfxr_desktop_input_kind");
    }return e;
}
inline Bytes reserved(unsigned type,const std::string& value){Writer w(true);w.integer(type,4);w.string(value);return w.bytes;}
// ReservedBuffer also carries Frame skeletons. Match the existing desktop
// event envelope exactly before consuming it in a negotiated VR session.
inline std::optional<Event> keyboard_input_event(const Bytes& bytes,bool candidates_only=false){
    Reader r(bytes,true);if(r.integer(4)!=11)return std::nullopt;
    const auto size=r.integer(8);
    if(r.remaining()!=size||!((size==19&&r.data()[0]==1)||(size==27&&r.data()[0]==2)))return std::nullopt;
    if(candidates_only&&size!=27)return std::nullopt;
    Bytes body(r.data(),r.data()+19);body[0]=1;auto e=event(body);
    if(size==27){r.skip(19);e.candidate=r.integer(8);require(e.candidate&&e.kind==EventKind::Button&&e.stream==0&&e.a==0&&e.b==1,"surfxr_candidate_click");}
    else require(e.kind==EventKind::Release||e.kind==EventKind::Key||e.kind==EventKind::Unicode,"surfxr_keyboard_event_kind");
    return e;
}
inline Bytes control(unsigned type){Writer w(true);w.integer(type,4);return w.bytes;}
// Mono desktop occupies stream 3; the standard v21 video header's view data is
// neutral and never enters the VR pose ledger or stereo renderer.
inline Bytes video(std::uint64_t timestamp,bool idr,const std::uint8_t* data,std::size_t size){
    require(size>0&&size<=8*1024*1024,"surfxr_desktop_video_size");Writer w(true);w.duration(timestamp);
    for(unsigned eye=0;eye<2;++eye){for(float f:{0.f,0.f,0.f,1.f,0.f,0.f,0.f,-.7f,.7f,.7f,-.7f})w.real(f);}
    w.boolean(false);w.boolean(idr);w.bytes.insert(w.bytes.end(),data,data+size);return w.bytes;
}
struct Display {int x=0,y=0,width=0,height=0;unsigned encoded_width=0,encoded_height=0;};
inline std::string config(unsigned port,bool audio){
    return "{\"server_version\":\"21.0.0-dev14\",\"session_settings\":{\"connection\":{\"stream_port\":"+std::to_string(port)+",\"packet_size\":1200,\"stream_protocol\":{\"variant\":\"Udp\"}},\"video\":{\"encoder_config\":{\"use_10bit\":{\"set\":true,\"content\":false}}},\"audio\":{\"game_audio\":{\"enabled\":"+(audio?"true":"false")+"},\"microphone\":{\"enabled\":false}}}}";
}
inline Bytes configuration(const Display& d,unsigned fps,unsigned port,bool audio=false,bool multi=false){
    Writer w(true);w.string(config(port,audio));w.integer(d.encoded_width,4);w.integer(d.encoded_height,4);w.real(float(fps));
    w.integer(48000,4);w.boolean(false);w.real(1);w.boolean(false);w.boolean(false);
    w.string("{\"frame_desktop\":{\"version\":1,\"x\":"+std::to_string(d.x)+",\"y\":"+std::to_string(d.y)+",\"width\":"+std::to_string(d.width)+",\"height\":"+std::to_string(d.height)+(multi?",\"multi_monitor\":1,\"capture_control\":1":"")+"}"+(audio?",\"frame_split_streams_udp\":{\"version\":1,\"priority_port\":"+std::to_string(port+1)+"}":"")+"}");return w.bytes;
}
}
