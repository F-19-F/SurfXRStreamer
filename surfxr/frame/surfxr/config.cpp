#include "surfxr/ports.hpp"
#include "wire.hpp"
#include <json-c/json.h>
#include <arpa/inet.h>
#include <cmath>
#include <memory>

namespace surfxr::transport {
namespace {
using Json=std::unique_ptr<json_object,decltype(&json_object_put)>;
Json parse(const std::string& s){
    std::unique_ptr<json_tokener,decltype(&json_tokener_free)> parser(json_tokener_new_ex(32),json_tokener_free);
    require(bool(parser),"surfxr_json_allocation");json_tokener_set_flags(parser.get(),JSON_TOKENER_STRICT);
    Json result(json_tokener_parse_ex(parser.get(),s.data(),static_cast<int>(s.size())),json_object_put);
    const auto parsed=static_cast<std::size_t>(parser->char_offset);
    require(result&&json_tokener_get_error(parser.get())==json_tokener_success&&
        parsed==s.size()&&json_object_is_type(result.get(),json_type_object),"surfxr_invalid_config_json");
    return result;
}
json_object* field(json_object* o,const char* key){json_object* v=nullptr;if(o)json_object_object_get_ex(o,key,&v);return v;}
unsigned number(json_object* o,const char* key,unsigned fallback,unsigned minimum,unsigned maximum){
    auto* v=field(o,key);if(!v)return fallback;
    require(json_object_is_type(v,json_type_int),"surfxr_invalid_config_number");const auto n=json_object_get_int64(v);
    require(n>=minimum&&n<=maximum,"surfxr_invalid_config_number");return static_cast<unsigned>(n);
}
bool boolean(json_object* o,const char* key,bool fallback){auto* v=field(o,key);if(!v)return fallback;require(json_object_is_type(v,json_type_boolean),"surfxr_invalid_config_bool");return json_object_get_boolean(v)!=0;}
std::string string(json_object* o,const char* key,const char* fallback){auto* v=field(o,key);if(!v)return fallback;require(json_object_is_type(v,json_type_string),"surfxr_invalid_config_string");return json_object_get_string(v);}
}
Bytes capabilities(const std::string& host,unsigned width,unsigned height,const std::vector<float>& rates,const std::string& version,bool split_streams,bool udp_only,bool gaze,bool frame_inputs,const std::string& client_hostname,bool desktop,bool desktop_tracking,bool pose_time){
    require(width>=32&&width<=4096&&height>=32&&height<=8192&&!rates.empty()&&rates.size()<=16,"surfxr_invalid_capabilities");
    in_addr ip{};require(inet_pton(AF_INET,host.c_str(),&ip)==1,"surfxr_ipv4_required");
    const std::string extra="{\"supports_foveated_encoding\":false,\"encoder_high_profile\":true,\"encoder_10_bits\":false,\"encoder_av1\":false,\"multimodal_protocol\":false,\"prefer_10bit\":false,\"prefer_full_range\":true,\"preferred_encoding_gamma\":1.0,\"prefer_hdr\":false}";
    const bool v21=is_v21(version);Writer w(v21);w.integer(0,4);w.integer(path_id(v21?"21-dev14":"20"),8);w.string("Steam Frame");
    w.integer(0,4); // serde IpAddr::V4 variant + four octets
    const auto* octets=reinterpret_cast<const std::uint8_t*>(&ip.s_addr);w.bytes.insert(w.bytes.end(),octets,octets+4);
    w.boolean(true);w.integer(width,4);w.integer(height,4);
    if(v21){w.integer(4096,4);w.integer(4096,4);}
    w.integer(rates.size()+(v21?0:extra.size()),8);
    for(float rate:rates){require(rate>=30&&rate<=240&&std::isfinite(rate),"surfxr_invalid_refresh_rate");w.real(rate);}
    if(!v21)for(unsigned char c:extra)w.real(-float(c));
    w.integer(48000,4);
    if(v21){for(bool value:{false,true,false,false,false})w.boolean(value);w.real(1);w.boolean(false);
        std::string ext=!split_streams?"{}":(udp_only?"{\"frame_udp_only\":1,\"frame_split_streams_udp\":1,\"frame_video_haptics\":1}":"{\"frame_split_streams\":1,\"frame_split_streams_udp\":1,\"frame_video_haptics\":1}");
        if(gaze)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_gaze_streaming\":1,\"frame_video_timestamps\":1");
        if(pose_time&&!desktop&&!desktop_tracking)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_pose_time\":1,\"frame_clock_calibration\":2");
        if(split_streams&&udp_only)ext.insert(ext.size()-1,",\"frame_video_fec\":1");
        if(frame_inputs)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_controller_inputs\":1,\"frame_controller_skeletons\":1,\"frame_controller_state\":1");
        if(frame_inputs||desktop_tracking)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_boundary_sync\":1");
        if(desktop)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_desktop\":1,\"frame_desktop_input\":1,\"frame_desktop_multimonitor\":1,\"frame_keyboard\":1,\"frame_candidate_click\":1,\"frame_desktop_keyboard\":1");
        if(desktop_tracking)ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_desktop_tracking\":1,\"frame_desktop_optics\":1");
        if(!client_hostname.empty()){
            require(client_hostname.size()<=63&&client_hostname.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string::npos,"surfxr_invalid_client_hostname");
            ext.insert(ext.size()-1,std::string(ext.size()>2?",":"")+"\"frame_client_connect\":1,\"frame_client_hostname\":\""+client_hostname+"\",\"frame_session_lifecycle\":1");
        }
        w.string(ext);}
    return std::move(w.bytes);
}
StreamConfig stream_config(const Bytes& bytes,const std::string& expected_version,bool allow_deprecated_tcp){
    const bool v21=is_v21(expected_version);Reader r(bytes,v21);auto session=parse(r.string());Json negotiated(nullptr,json_object_put),extensions(nullptr,json_object_put);
    if(v21){
        negotiated.reset(json_object_new_object());require(bool(negotiated),"surfxr_json_allocation");
        auto* resolution=json_object_new_array();json_object_object_add(negotiated.get(),"view_resolution",resolution);
        json_object_array_add(resolution,json_object_new_int64(r.integer(4)));json_object_array_add(resolution,json_object_new_int64(r.integer(4)));
        json_object_object_add(negotiated.get(),"refresh_rate_hint",json_object_new_double(r.real()));
        json_object_object_add(negotiated.get(),"game_audio_sample_rate",json_object_new_int64(r.integer(4)));
        require(!r.boolean(),"surfxr_foveation_not_supported");
        json_object_object_add(negotiated.get(),"encoding_gamma",json_object_new_double(r.real()));
        json_object_object_add(negotiated.get(),"enable_hdr",json_object_new_boolean(r.boolean()));
        json_object_object_add(negotiated.get(),"wired",json_object_new_boolean(r.boolean()));
        const auto ext=r.string(4096);if(!ext.empty())extensions=parse(ext);
    }else negotiated=parse(r.string());
    r.end();
    const auto version=string(session.get(),"server_version","");
    require(v21?version==expected_version:(version.rfind("20.",0)==0&&version.find('-')==std::string::npos),"surfxr_incompatible_server_version");
    auto* settings=field(session.get(),"session_settings");require(settings,"surfxr_missing_session_settings");
    auto* connection=field(settings,"connection");require(connection,"surfxr_missing_connection_config");
    StreamConfig result;
    result.port=number(connection,"stream_port",ports::vr_media,1024,65535);
    result.packet_size=number(connection,"packet_size",1400,64,65503);
    const auto protocol=string(field(connection,"stream_protocol"),"variant","Udp");
    require(protocol=="Udp"||protocol=="Tcp","surfxr_unsupported_transport");
    result.tcp=protocol=="Tcp"||boolean(negotiated.get(),"wired",false);
    require(!result.tcp||allow_deprecated_tcp,"surfxr_tcp_media_deprecated_select_udp");
    result.frame_inputs=number(extensions.get(),"frame_controller_inputs",0,1,1)==1;
    result.pose_time=number(extensions.get(),"frame_pose_time",0,1,1)==1;
    result.clock_calibration=number(extensions.get(),"frame_clock_calibration",0,1,2);
    require(!result.clock_calibration||result.pose_time,"surfxr_clock_calibration_requires_pose_time");
    result.controller_state=result.frame_inputs&&number(extensions.get(),"frame_controller_state",0,1,1)==1;
    result.boundary_sync=number(extensions.get(),"frame_boundary_sync",0,1,1)==1;
    result.session_lifecycle=number(extensions.get(),"frame_session_lifecycle",0,1,1)==1;
    result.frame_skeletons=result.frame_inputs&&number(extensions.get(),"frame_controller_skeletons",0,1,1)==1;
    result.video_haptics=number(extensions.get(),"frame_video_haptics",0,1,1)==1;
    result.image_timestamps=number(extensions.get(),"frame_video_timestamps",0,1,1)==1;
    auto* split_tcp=field(extensions.get(),"frame_split_streams");
    auto* split_udp=field(extensions.get(),"frame_split_streams_udp");
    require(!(split_tcp&&split_udp)&&!(result.tcp?split_udp:split_tcp),"surfxr_invalid_split_transport");
    if(auto* split=result.tcp?split_tcp:split_udp){
        require(json_object_is_type(split,json_type_object)&&number(split,"version",0,1,1)==1,"surfxr_invalid_split_transport");
        result.priority_port=number(split,"priority_port",0,1024,65535);
        require(result.priority_port&&result.priority_port!=result.port,"surfxr_port_collision");
    }
    if(auto* fec=field(extensions.get(),"frame_video_fec")){
        require(v21&&!result.tcp&&result.priority_port&&json_object_is_type(fec,json_type_object)&&
            number(fec,"version",0,1,1)==1&&number(fec,"data_shards",0,16,16)==16,"surfxr_invalid_video_fec");
        result.video_fec=true;
    }
    auto* resolution=field(negotiated.get(),"view_resolution");
    require(resolution&&json_object_is_type(resolution,json_type_array)&&json_object_array_length(resolution)==2,"surfxr_invalid_resolution");
    for(unsigned i=0;i<2;++i){auto* v=json_object_array_get_idx(resolution,i);
        require(json_object_is_type(v,json_type_int),"surfxr_invalid_resolution");const auto n=json_object_get_int64(v);
        require(n>=32&&n<=(i?8192:4096),"surfxr_invalid_resolution");if(i)result.height=static_cast<unsigned>(n);else result.width=static_cast<unsigned>(n);
    }
    require(std::uint64_t{result.width}*result.height<=16ULL*1024*1024,"surfxr_decoder_pixel_limit");
    if(auto* desktop=field(extensions.get(),"frame_desktop")){
        require(v21&&!result.tcp&&json_object_is_type(desktop,json_type_object)&&number(desktop,"version",0,1,1)==1,"surfxr_invalid_desktop_config");
        result.desktop=true;
        result.desktop_multimonitor=number(desktop,"multi_monitor",0,1,1)==1;
        result.desktop_capture_control=result.desktop_multimonitor&&number(desktop,"capture_control",0,1,1)==1;
        for(const auto* name:{"x","y"}){auto* value=field(desktop,name);require(value&&json_object_is_type(value,json_type_int)&&json_object_get_int64(value)>=-65536&&json_object_get_int64(value)<=65536,"surfxr_invalid_desktop_config");
            (name[0]=='x'?result.desktop_x:result.desktop_y)=json_object_get_int(value);}
        result.desktop_width=static_cast<int>(number(desktop,"width",0,32,16384));result.desktop_height=static_cast<int>(number(desktop,"height",0,32,16384));
        require(result.desktop_width>=32&&result.desktop_height>=32,"surfxr_invalid_desktop_config");
    }
    if(auto* gaze=field(extensions.get(),"frame_gaze_streaming")){
        require(!result.tcp&&json_object_is_type(gaze,json_type_object)&&number(gaze,"version",0,1,1)==1,"surfxr_invalid_gaze_config");
        auto& a=result.gaze_atlas;
        a.base_width=number(gaze,"base_width",0,32,result.width);a.base_height=number(gaze,"base_height",0,32,result.height);
        a.focus_width=number(gaze,"focus_width",0,32,a.base_width);a.focus_height=number(gaze,"focus_height",0,32,result.height);
        require(a.base_width&&a.base_height&&a.focus_width&&a.focus_height&&a.focus_height<=result.height&&
            a.base_width%32==0&&a.base_height%32==0&&a.focus_width%32==0&&a.focus_height%32==0&&
            a.base_width*2<=8192&&a.base_height+a.focus_height<=8192,"surfxr_invalid_gaze_config");
    }
    auto* rate=field(negotiated.get(),"refresh_rate_hint");require(rate&&(json_object_is_type(rate,json_type_double)||json_object_is_type(rate,json_type_int)),"surfxr_invalid_refresh_rate");
    result.refresh_rate=static_cast<float>(json_object_get_double(rate));require(std::isfinite(result.refresh_rate)&&result.refresh_rate>=30&&result.refresh_rate<=240,"surfxr_invalid_refresh_rate");
    require(!boolean(negotiated.get(),"enable_foveated_encoding",false),"surfxr_foveation_not_supported");
    require(!boolean(negotiated.get(),"enable_hdr",false),"surfxr_hdr_not_supported");
    auto* encoder=field(field(settings,"video"),"encoder_config");
    auto* ten_bit=field(encoder,"use_10bit");
    const bool enabled_10bit=v21&&ten_bit?boolean(ten_bit,"set",false)&&boolean(ten_bit,"content",false):boolean(encoder,"use_10bit",false);
    require(!enabled_10bit,"surfxr_10bit_not_supported");
    auto* gamma=field(negotiated.get(),"encoding_gamma");
    require(!gamma||std::abs(json_object_get_double(gamma)-1.0)<.0001,"surfxr_encoding_gamma_not_supported");
    result.full_range=boolean(negotiated.get(),"use_full_range",true);
    if(result.desktop)result.full_range=false;
    result.audio_rate=number(negotiated.get(),"game_audio_sample_rate",48000,0,192000);
    auto* audio=field(settings,"audio");result.game_audio=boolean(field(audio,"game_audio"),"enabled",true);
    result.microphone=boolean(field(audio,"microphone"),"enabled",false);
    require(!result.game_audio||result.audio_rate==48000,"surfxr_audio_requires_48000hz");
    return result;
}
}
