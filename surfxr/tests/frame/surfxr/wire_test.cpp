#include "wire.hpp"
#include "stream.hpp"
#include "view_updates.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
#include <cmath>
#include <chrono>

using namespace surfxr::transport;
void media_checks();
void master_checks();
void pose_time_checks();
void frame_input_checks();
void boundary_checks();
void fec_checks();
void fec_rust_fixture(const char*);
namespace {
template<class F> void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"test_expected_rejection");}
void hashes(){
    // Rust std::hash::DefaultHasher oracle, the exact surfxr_common::hash_string
    // implementation in upstream SURFXR v20.14.1/common/src/version.rs.
    require(path_id("20")==1686174404061257063ULL,"test_protocol_hash");
    require(path_id("/user/head")==6597919954511603000ULL,"test_head_hash");
    require(path_id("/user/hand/left")==16510716142922041250ULL,"test_left_hash");
    require(path_id("/user/hand/right")==17777980629871082968ULL,"test_right_hash");
    require(path_id("/interaction_profiles/oculus/touch_controller")==581806052916561062ULL,"test_profile_hash");
    require(path_id("/user/hand/left/input/trigger/value")==821550569367651487ULL,"test_button_hash");
}
void input_benchmark(){
    // Explicit CPU-only mode, using the real serializer and bounded synthetic
    // state. The digest consumes every output byte and must match both builds.
    surfxr::TrackingFrame frame;frame.focused=frame.visible=true;
    for(auto& hand:frame.hands){hand.profile=surfxr::TrackingProfile::SteamFrame;
        hand.buttons_active=hand.touches_active=hand.analog_active=UINT32_MAX;
        hand.trigger=.4f;hand.squeeze=.7f;hand.stick_y=-.5f;}
    constexpr unsigned iterations=100000;
    for(bool modern:{false,true}){
        for(unsigned i=0;i<1000;++i)buttons(frame,true,modern);
        std::uint64_t digest=14695981039346656037ULL;
        const auto started=std::chrono::steady_clock::now();
        for(unsigned i=0;i<iterations;++i){
            frame.hands[0].buttons=i;frame.hands[1].touches=i;
            frame.hands[0].stick_x=(static_cast<int>(i%17)-8)/8.f;
            const auto packet=buttons(frame,true,modern);
            for(auto byte:packet){digest^=byte;digest*=1099511628211ULL;}
        }
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        std::cout<<"{\"benchmark\":\"surfxr_buttons\",\"modern\":"<<(modern?"true":"false")
            <<",\"iterations\":"<<iterations<<",\"ns_per_packet\":"<<double(ns)/iterations<<",\"digest\":"<<digest<<"}\n";
    }
}
void framing(){
    Bytes body{0,1,2,3,4};Bytes frame;big(frame,static_cast<unsigned>(body.size()),4);frame.insert(frame.end(),body.begin(),body.end());
    Bytes both=frame;both.insert(both.end(),frame.begin(),frame.end());
    for(std::size_t split=0;split<=both.size();++split){Framer f(32);unsigned count=0;const auto sink=[&](Bytes b){require(b==body,"test_fragmented_body");++count;};
        f.feed(both.data(),split,sink);f.feed(both.data()+split,both.size()-split,sink);require(count==2,"test_multiple_records");}
    rejects([]{Framer f(1024);const Bytes bad{0xff,0xff,0xff,0xff};f.feed(bad.data(),bad.size(),[](Bytes){});});
    Bytes payload(8000);for(std::size_t i=0;i<payload.size();++i)payload[i]=static_cast<std::uint8_t>(i);
    const auto parts=shards(3,0,payload,1400);Reassembler r(1400);
    for(std::size_t i=parts.size();i-->0;){auto p=r.accept(parts[i],1);if(i==0)require(p&&p->bytes==payload&&!p->loss,"test_reverse_shards");else require(!p,"test_incomplete_shards");}
    require(r.retained_bytes()==0&&!r.accept(parts[0],2),"test_late_duplicate");
    auto newer=shards(3,2,Bytes{5},1400);const auto packet=r.accept(newer[0],3);require(packet&&packet->loss,"test_packet_gap");
    Reassembler duplicates(1400);require(!duplicates.accept(parts[0],1)&&!duplicates.accept(parts[0],2),"test_duplicate_shard");
    auto bad=parts[0];bad[18]^=1;rejects([&]{duplicates.accept(bad,3);});
    bad=parts[0];bad[14]=255;rejects([&]{duplicates.accept(bad,3);});
    Reassembler pressure(1400);
    for(unsigned i=0;i<100;++i)pressure.accept(shards(3,i,payload,1400)[0],i+1);
    require(pressure.retained_bytes()<=8*1404&&pressure.dropped()>0,"test_bounded_reassembly");
    Reassembler wrapped(1400);auto p=wrapped.accept(shards(3,0xffffffff,Bytes{1},1400)[0],1);
    require(p&&wrapped.accept(shards(3,0,Bytes{2},1400)[0],2).has_value(),"test_wrapping_sequence");
}
void packets(){
    const auto caps=capabilities("127.0.0.1",32,32,{90});Reader c(caps);
    require(c.integer(4)==0&&c.integer(8)==path_id("20")&&c.string()=="Steam Frame","test_capability_identity");
    require(c.integer(4)==0&&c.integer(4)==0x0100007f&&c.boolean()&&c.integer(4)==32&&c.integer(4)==32,"test_capability_ipv4");
    const auto count=c.integer(8);require(c.real()==90,"test_refresh_rate");std::string extra;
    for(unsigned i=1;i<count;++i)extra.push_back(static_cast<char>(-c.real()));
    require(extra.find("\"encoder_av1\":false")!=std::string::npos&&c.integer(4)==48000,"test_actual_decoder_capabilities");c.end();
    Writer packet;packet.string("{\"server_version\":\"20.14.1\",\"session_settings\":{\"connection\":{\"stream_protocol\":{\"variant\":\"Tcp\"},\"packet_size\":1400,\"stream_port\":42123},\"audio\":{\"game_audio\":{\"enabled\":true}}}} ");
    packet.string("{\"view_resolution\":[1728,1728],\"refresh_rate_hint\":120,\"game_audio_sample_rate\":48000,\"enable_foveated_encoding\":false,\"encoding_gamma\":1}");
    // Strict parser accepts JSON's trailing whitespace, never trailing data.
    const auto config=stream_config(packet.bytes);require(config.tcp&&config.port==42123&&config.width==1728,"test_negotiated_config");
    Writer godlike;godlike.string("{\"server_version\":\"20.14.1\",\"session_settings\":{\"connection\":{}}}");
    godlike.string("{\"view_resolution\":[3072,3264],\"refresh_rate_hint\":144,\"game_audio_sample_rate\":48000}");
    const auto large=stream_config(godlike.bytes);require(large.width==3072&&large.height==3264&&large.refresh_rate==144,"test_godlike_144_config");
    Writer too_large;too_large.string("{\"server_version\":\"20.14.1\",\"session_settings\":{\"connection\":{}}}");
    too_large.string("{\"view_resolution\":[4096,8192],\"refresh_rate_hint\":144}");rejects([&]{stream_config(too_large.bytes);});
    Writer unsupported;unsupported.string("{\"server_version\":\"21.0.0\",\"session_settings\":{\"connection\":{}}}");unsupported.string("{}");rejects([&]{stream_config(unsupported.bytes);});
    rejects([]{const Bytes bytes{2};Reader r(bytes);r.boolean();});
    rejects([]{Writer w;w.integer(1,8);w.integer(1'000'000'000,4);Reader r(w.bytes);r.duration();});
    Writer decoder;decoder.integer(1,4);decoder.integer(1,4);decoder.blob({0,0,0,1,0x40});
    require(server_control(decoder.bytes).codec==1,"test_hevc_decoder_config");
    rejects([]{Writer w;w.integer(1,4);w.integer(2,4);w.blob({1});server_control(w.bytes);});
    Writer pulse;pulse.integer(path_id("/user/hand/right"),8);pulse.duration(75'000'000);pulse.real(150);pulse.real(.5f);
    const auto h=haptic(pulse.bytes);require(h.hand==1&&h.duration_ns==75'000'000&&h.frequency==150&&h.amplitude==.5f,"test_haptics");
}
void optical_updates(){
    surfxr::TrackingFrame initial,current;current.optics_valid=true;current.origin_epoch=1;
    current.ipd_meters=.064f;current.eye_fov={{{-.7f,.7f,.7f,-.7f},{-.7f,.7f,.7f,-.7f}}};
    require(views_changed(current,initial),"test_initial_optics_sent");initial=current;
    current.ipd_meters=std::nextafter(current.ipd_meters,1.f);
    current.head.pose.position={1,2,3};current.sequence=2;
    require(views(current)!=views(initial)&&!views_changed(current,initial),"test_ipd_float_jitter_not_optical_update");
    current.ipd_meters=initial.ipd_meters+.0009f;
    require(!views_changed(current,initial),"test_fine_ipd_requires_direction_filter");
    current.ipd_meters=initial.ipd_meters+.0011f;
    require(views_changed(current,initial),"test_real_ipd_change_sent");
    current=initial;current.eye_fov[1][3]+=.0002f;
    require(!views_changed(current,initial),"test_frame_handoff_fov_jitter_held");
    current=initial;current.eye_fov[1][3]+=.002f;
    require(views_changed(current,initial),"test_fov_change_sent");
    current=initial;++current.origin_epoch;
    require(views_changed(current,initial),"test_epoch_optics_resent");
    ViewUpdates filter;filter.sent(initial,0);
    for(std::uint64_t tick=1;tick<=10'000;++tick){current=initial;
        current.ipd_meters+=(tick%2?.00015f:-.00015f);
        require(!filter.should_send(current,tick*50'000'000),"test_large_back_and_forth_drift_is_not_manual_adjustment");}
    for(int direction:{-1,1}){
        filter={};filter.sent(initial,0);auto sent=initial;unsigned updates=0;std::uint64_t sent_at=0;
        for(std::uint64_t tick=1;tick<=40;++tick){current=initial;
            current.ipd_meters+=direction*float(tick)*.00002f;const auto now=tick*50'000'000;
            if(filter.should_send(current,now)){
                require(now-sent_at>=200'000'000,"test_fine_adjustment_notifications_rate_limited");
                filter.sent(current,now);sent=current;sent_at=now;++updates;}
        }
        if(filter.should_send(current,2'300'000'000)){filter.sent(current,2'300'000'000);sent=current;}
        require(updates>=4&&updates<=10&&std::abs(sent.ipd_meters-current.ipd_meters)<=.00005f,
                "test_continuous_fine_adjustment_tracks_both_directions_and_final_value");
        for(std::uint64_t tick=0;tick<10'000;++tick){current=sent;current.ipd_meters+=(tick%2?.000015f:-.000015f);
            require(!filter.should_send(current,3'000'000'000+tick*5'000'000),"test_stationary_noise_never_repeats");}
        current=sent;current.ipd_meters+=direction*.00006f;
        require(!filter.should_send(current,54'000'000'000)&&!filter.should_send(current,54'500'000'000),
                "test_idle_follow_mode_does_not_reuse_old_direction_for_new_drift");
    }
    filter={};filter.sent(initial,0);current=initial;current.ipd_meters+=.0002f;
    require(!filter.should_send(current,50'000'000)&&!filter.should_send(current,349'999'999),"test_one_sided_step_requires_quiet_hold");
    require(filter.should_send(current,350'000'000),"test_single_fine_wheel_step_is_not_lost");
    filter.sent(current,350'000'000);
    require(!filter.should_send(current,900'000'000),"test_held_final_value_does_not_repeat");
}
void menu_mapping(){
    std::vector<std::uint64_t> advertised;
    for(unsigned hand=0;hand<2;++hand){
        const auto bytes=profile(hand,true);Reader r(bytes,true);
        require(r.integer(4)==7&&r.integer(8)==path_id(hand?"/user/hand/right":"/user/hand/left")&&
                r.integer(8)==path_id("/interaction_profiles/oculus/touch_controller"),"test_active_profile_identity");
        std::vector<std::uint64_t> ids;const auto count=r.integer(8);
        for(std::uint64_t i=0;i<count;++i)ids.push_back(r.integer(8));
        r.end();
        require(count==29&&std::find(ids.begin(),ids.end(),path_id("/user/hand/left/input/menu/click"))!=ids.end()&&
                std::find(ids.begin(),ids.end(),path_id("/user/hand/right/input/trigger/value"))!=ids.end(),"test_profile_keeps_both_hands_active");
        if(hand)require(ids==advertised,"test_global_mapping_not_replaced_by_single_hand");else advertised=std::move(ids);
    }
    const auto pressed=[](const surfxr::TrackingFrame& frame,bool enabled,bool modern,const char* path){
        const auto packet=buttons(frame,enabled,modern);Reader r(packet,modern);
        require(r.integer(4)==(modern?6u:7u),"test_button_control_type");
        const auto count=r.integer(8);bool value=false,found=false;
        for(std::uint64_t i=0;i<count;++i){const auto id=r.integer(8),type=r.integer(4);
            if(type==0){const bool down=r.boolean();if(id==path_id(path)){value=down;found=true;}}
            else{require(type==1,"test_button_scalar_type");r.real();}
        }
        r.end();require(found,"test_menu_path_advertised");return value;
    };
    for(bool modern:{false,true})for(auto key:{surfxr::ButtonView,surfxr::ButtonSystem}){
        surfxr::TrackingFrame frame;frame.focused=frame.visible=true;frame.ui_captured[0]=true;
        frame.hands[0].profile=surfxr::TrackingProfile::SteamFrame;
        frame.hands[0].buttons=frame.hands[0].buttons_active=key|surfxr::ButtonTrigger;
        const char* menu="/user/hand/left/input/menu/click";
        require(pressed(frame,true,modern,menu),"test_frame_menu_bypasses_local_ui_capture");
        require(!pressed(frame,true,modern,"/user/hand/left/input/trigger/click"),"test_captured_trigger_not_forwarded");
        frame.hands[0].buttons=0;
        require(!pressed(frame,true,modern,menu),"test_frame_menu_release_sent");
        frame.hands[0].buttons=key;frame.focused=false;
        require(!pressed(frame,true,modern,menu),"test_unfocused_menu_released");frame.focused=true;
        require(!pressed(frame,false,modern,menu),"test_disabled_menu_released");
        frame.hands[0].buttons_active=surfxr::ButtonTrigger;
        require(!pressed(frame,true,modern,menu),"test_inactive_menu_not_forwarded");
        frame.hands[0].buttons=frame.hands[0].buttons_active=0;
        frame.hands[1].profile=surfxr::TrackingProfile::SteamFrame;
        frame.hands[1].buttons=frame.hands[1].buttons_active=surfxr::ButtonMenu;
        require(!pressed(frame,true,modern,menu),"test_right_local_menu_not_remote_system");
    }
    for(bool modern:{false,true}){
        const auto bytes=buttons(surfxr::TrackingFrame{},true,modern);Reader r(bytes,modern);
        require(r.integer(4)==(modern?6u:7u)&&r.integer(8)==29,"test_complete_button_count");
        for(unsigned hand=0;hand<2;++hand){
            const auto check=[&](const char* suffix,unsigned type){
                const auto path=std::string(hand?"/user/hand/right/input/":"/user/hand/left/input/")+suffix;
                require(r.integer(8)==path_id(path)&&r.integer(4)==type,"test_cached_input_identity_and_order");
                if(type)require(r.real()==0,"test_inactive_scalar_released");
                else require(!r.boolean(),"test_inactive_button_released");
            };
            for(const auto* suffix:hand?std::vector<const char*>{"a/click","b/click","a/touch","b/touch"}:
                std::vector<const char*>{"x/click","y/click","x/touch","y/touch","menu/click"})check(suffix,0);
            for(const auto* suffix:{"trigger/click","trigger/touch","squeeze/click","thumbstick/click","thumbstick/touch","thumbrest/touch"})check(suffix,0);
            for(const auto* suffix:{"trigger/value","squeeze/value","thumbstick/x","thumbstick/y"})check(suffix,1);
        }
        r.end();
    }
}
}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--benchmark"){input_benchmark();return 0;}
    if(argc==3&&std::string(argv[1])=="--fec-fixture"){fec_checks();fec_rust_fixture(argv[2]);return 0;}
    hashes();framing();packets();optical_updates();menu_mapping();frame_input_checks();boundary_checks();media_checks();master_checks();pose_time_checks();fec_checks();
    if(argc==2){std::ifstream file(argv[1],std::ios::binary);require(bool(file),"test_upstream_fixture_missing");
        const Bytes bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        const auto config=stream_config(bytes);require(config.width==32&&config.height==32&&config.refresh_rate==90,"test_upstream_stream_config");
    }else require(argc==1,"test_unexpected_arguments");
    std::cout<<"SURFXR wire, framing, media and ownership checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
