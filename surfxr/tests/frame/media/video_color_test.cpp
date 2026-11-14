#include "surfxr/video_color.hpp"
#include "surfxr/h264_sequence.hpp"
#include "surfxr/hevc_sequence.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Bytes=std::vector<std::uint8_t>;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Writer {
    Bytes data;unsigned position=0;
    void bit(unsigned value){if(position%8==0)data.push_back(0);data.back()|=static_cast<std::uint8_t>((value&1)<<(7-position%8));++position;}
    void bits(unsigned value,unsigned count){for(unsigned i=count;i>0;--i)bit(value>>(i-1));}
    void zero(unsigned count){for(unsigned i=0;i<count;++i)bit(0);}
    void ue(unsigned value){unsigned count=0;for(unsigned x=value+1;x>1;x>>=1)++count;zero(count);bits(value+1,count+1);}
    void se(int value){ue(value>0?2*static_cast<unsigned>(value)-1:2*static_cast<unsigned>(-value));}
    Bytes nal(unsigned type,bool hevc=false,unsigned layer=0){
        bit(1);while(position%8)bit(0);
        Bytes output=hevc?Bytes{static_cast<std::uint8_t>((type<<1)|(layer>>5)),static_cast<std::uint8_t>((layer<<3)|1)}:
                          Bytes{static_cast<std::uint8_t>(0x60|type)};
        unsigned zeros=0;
        for(auto byte:data){if(zeros>=2&&byte<=3){output.push_back(3);zeros=0;}output.push_back(byte);zeros=byte==0?zeros+1:0;}
        return output;
    }
};
void color_vui(Writer& w,unsigned matrix,bool full,bool advanced=false){
    w.bit(advanced?1:0);if(advanced){w.bits(255,8);w.bits(1,16);w.bits(1,16);}
    w.bit(advanced?1:0);if(advanced)w.bit(0);
    w.bit(1);w.bits(5,3);w.bit(full?1:0);w.bit(1);w.bits(1,8);w.bits(1,8);w.bits(matrix,8);
}
Bytes avc_sps(unsigned id,unsigned matrix,bool full,bool vui=true,bool advanced=false){
    Writer w;w.bits(advanced?100:66,8);w.bits(0,8);w.bits(31,8);w.ue(id);
    if(advanced){w.ue(1);w.ue(0);w.ue(0);w.bit(0);w.bit(1);
        for(unsigned i=0;i<8;++i){w.bit(i==0?1:0);if(i==0)for(unsigned j=0;j<16;++j)w.se(0);}}
    w.ue(0);w.ue(advanced?1:0);
    if(advanced){w.bit(0);w.se(-1);w.se(1);w.ue(2);w.se(-1);w.se(1);}else w.ue(0);
    w.ue(1);w.bit(0);w.ue(15);w.ue(15);w.bit(1);w.bit(1);
    w.bit(advanced?1:0);if(advanced){w.ue(0);w.ue(1);w.ue(0);w.ue(1);}
    w.bit(vui?1:0);if(vui){color_vui(w,matrix,full,advanced);w.zero(6);}
    return w.nal(7);
}
Bytes avc_pps(unsigned id,unsigned sps){
    Writer w;w.ue(id);w.ue(sps);w.bit(0);w.bit(0);w.ue(0);w.ue(0);w.ue(0);w.bit(0);w.bits(0,2);
    w.se(0);w.se(0);w.se(0);w.bit(1);w.bit(0);w.bit(0);return w.nal(8);
}
Bytes avc_slice(unsigned pps,bool idr=false){Writer w;w.ue(0);w.ue(idr?2:0);w.ue(pps);w.bits(0x55,8);return w.nal(idr?5:1);}
Bytes hevc_sps(unsigned id,unsigned matrix,bool full,bool vui=true,bool advanced=false,unsigned layer=0){
    Writer w;const unsigned layers=advanced?2:0;
    w.bits(0,4);w.bits(layers,3);w.bit(1);w.zero(88);w.bits(120,8);
    for(unsigned i=0;i<layers;++i){w.bit(1);w.bit(1);}
    if(layers)w.zero((8-layers)*2);
    for(unsigned i=0;i<layers;++i){w.zero(88);w.bits(90,8);}
    w.ue(id);w.ue(1);w.ue(256);w.ue(256);w.bit(advanced?1:0);
    if(advanced){w.ue(0);w.ue(1);w.ue(0);w.ue(1);}
    w.ue(0);w.ue(0);w.ue(0);w.bit(advanced?1:0);
    for(unsigned i=advanced?0:layers;i<=layers;++i){w.ue(4);w.ue(0);w.ue(0);}
    w.ue(0);w.ue(3);w.ue(0);w.ue(3);w.ue(0);w.ue(0);
    w.bit(advanced?1:0);
    if(advanced){w.bit(1);for(unsigned size=0;size<4;++size)for(unsigned matrix_id=0;matrix_id<6;matrix_id+=size==3?3:1){
        w.bit(matrix_id==0?1:0);
        if(matrix_id==0){if(size>1)w.se(0);for(unsigned j=0;j<(size==0?16:64);++j)w.se(0);}else w.ue(0);
    }}
    w.bit(1);w.bit(1);w.bit(advanced?1:0);
    if(advanced){w.bits(7,4);w.bits(7,4);w.ue(0);w.ue(2);w.bit(1);}
    w.ue(advanced?2:0);
    if(advanced){
        w.ue(1);w.ue(1);w.ue(0);w.bit(1);w.ue(0);w.bit(1);
        w.bit(1);w.bit(0);w.ue(0);w.bit(1);w.bit(0);w.bit(1);w.bit(0);w.bit(0);
    }
    w.bit(advanced?1:0);if(advanced){w.ue(1);w.bits(3,4);w.bit(1);}
    w.bit(1);w.bit(1);w.bit(vui?1:0);
    if(vui){color_vui(w,matrix,full,advanced);w.zero(7);}
    w.bit(0);return w.nal(33,true,layer);
}
Bytes hevc_pps(unsigned id,unsigned sps,unsigned layer=0){Writer w;w.ue(id);w.ue(sps);w.bits(0x55,8);return w.nal(34,true,layer);}
Bytes hevc_slice(unsigned pps,bool irap=false,unsigned layer=0,bool dependent=false){
    Writer w;w.bit(dependent?0:1);if(irap)w.bit(0);w.ue(pps);w.bits(0x55,8);return w.nal(irap?19:1,true,layer);
}
Bytes annex(const Bytes& nal){Bytes out{0,0,0,1};out.insert(out.end(),nal.begin(),nal.end());return out;}
void append(Bytes& out,const Bytes& in){out.insert(out.end(),in.begin(),in.end());}
Bytes hex(const char* text){Bytes out;for(std::size_t i=0;text[i];i+=2){unsigned value=0;for(unsigned j=0;j<2;++j){const char c=text[i+j];value=value*16+static_cast<unsigned>(c>='a'?c-'a'+10:c-'0');}out.push_back(static_cast<std::uint8_t>(value));}return out;}
template<class Tracker> auto inspect(Tracker& tracker,const Bytes& packet){return tracker.inspect_packet(packet.data(),packet.size());}
template<class Inspection> void color(const Inspection& value,int matrix,int range,const char* label){
    require(value.valid&&value.has_active_color&&value.active_color.color_space==matrix&&value.active_color.color_range==range,label);
}
unsigned tests(){unsigned groups=0;
    // Independently encoded libx264/libx265 256x256 SPS, checked with ffprobe.
    // Includes Annex B emulation prevention, BT709, and both signaled ranges.
    const char* avc_real[]={"6742c00dda04021a6a02020280000003008000001e078a1550","6742c00dda04021a6e02020280000003008000001e078a1550"};
    const char* hevc_real[]={"4201010160000003009000000300000300ffa00808040596e92930b9a808080820000003002000000303c1",
                             "4201010160000003009000000300000300ffa00808040596e92930b9b808080820000003002000000303c1"};
    for(unsigned full=0;full<2;++full){
        for(bool hevc:{false,true}){const auto bytes=hex(hevc?hevc_real[full]:avc_real[full]);unsigned id=99;surfxr::VideoColorInfo info;
            require((hevc?surfxr::hevc_sps_color(bytes.data(),bytes.size(),id,info):surfxr::h264_sps_color(bytes.data(),bytes.size(),id,info))
                &&id==0&&info.color_space==1&&info.color_range==static_cast<int>(full+1),"independent_encoded_vui");}
    }++groups;
    for(bool hevc:{false,true})for(bool advanced:{false,true})for(unsigned matrix:{1u,5u,6u,9u,10u,3u,255u})for(bool full:{false,true}){
        const auto bytes=hevc?hevc_sps(3,matrix,full,true,advanced):avc_sps(3,matrix,full,true,advanced);unsigned id=99;surfxr::VideoColorInfo info;
        require((hevc?surfxr::hevc_sps_color(bytes.data(),bytes.size(),id,info):surfxr::h264_sps_color(bytes.data(),bytes.size(),id,info))
            &&id==3&&info.color_space==static_cast<int>(matrix==3||matrix>14?2:matrix)&&info.color_range==(full?2:1),"optional_sps_syntax_and_matrices");
    }++groups;
    for(bool hevc:{false,true}){
        const auto bytes=hevc?hevc_sps(0,1,true,false):avc_sps(0,1,true,false);unsigned id=99;surfxr::VideoColorInfo info;
        require((hevc?surfxr::hevc_sps_color(bytes.data(),bytes.size(),id,info):surfxr::h264_sps_color(bytes.data(),bytes.size(),id,info))
            &&id==0&&info.color_space==2&&info.color_range==1,"absent_vui_uses_standard_limited_default");
        const auto complete=hevc?hevc_sps(0,9,true,true,true):avc_sps(0,9,true,true,true);unsigned rejected=0;
        for(std::size_t n=0;n<complete.size();++n){unsigned saved_id=13;surfxr::VideoColorInfo saved{7,2};
            if(!(hevc?surfxr::hevc_sps_color(complete.data(),n,saved_id,saved):surfxr::h264_sps_color(complete.data(),n,saved_id,saved))){
                require(saved_id==13&&saved.color_space==7&&saved.color_range==2,"truncated_metadata_outputs_are_atomic");++rejected;}}
        require(rejected>10,"truncation_coverage");
        Bytes oversized(4097,0x55);require(!(hevc?surfxr::hevc_sps_color(oversized.data(),oversized.size(),id,info):surfxr::h264_sps_color(oversized.data(),oversized.size(),id,info)),"sps_byte_bound");
        const auto malformed=hevc?Bytes{66,1,0,0,3,4}:Bytes{0x67,0,0,3,4};
        require(!(hevc?surfxr::hevc_sps_color(malformed.data(),malformed.size(),id,info):surfxr::h264_sps_color(malformed.data(),malformed.size(),id,info)),"invalid_emulation_prevention");
    }++groups;
    {surfxr::H264SequenceTracker tracker;auto packet=annex(avc_sps(0,1,true));append(packet,annex(avc_pps(0,0)));
        append(packet,annex(avc_sps(1,6,false)));append(packet,annex(avc_pps(1,1)));append(packet,annex(avc_slice(0,true)));
        color(inspect(tracker,packet),1,2,"avc_idr_uses_referenced_sps_not_latest");
        color(inspect(tracker,annex(avc_slice(1))),6,1,"avc_predicted_picture_uses_own_pps");
        color(inspect(tracker,annex(avc_slice(0))),1,2,"avc_original_sps_remains_available");
        require(!inspect(tracker,annex(avc_slice(2))).has_active_color,"avc_unknown_pps_no_stale_color");
        auto mixed=annex(avc_slice(0));append(mixed,annex(avc_slice(1)));require(!inspect(tracker,mixed).has_active_color,"avc_mixed_pps_has_no_color");
        auto broken=annex(avc_sps(0,9,false));append(broken,{0,0,1,0x67});require(!inspect(tracker,broken).valid,"avc_bad_tail_rejected");
        color(inspect(tracker,annex(avc_slice(0))),1,2,"avc_bad_tail_preserves_color_cache");
        require(inspect(tracker,annex(avc_sps(0,1,true,false))).valid,"avc_no_vui_replacement");
        color(inspect(tracker,annex(avc_slice(0))),2,1,"avc_vui_removal_clears_previous_full_range");
        auto short_vui=avc_sps(0,1,true);short_vui.resize(short_vui.size()-3);
        require(inspect(tracker,annex(short_vui)).valid&&!inspect(tracker,annex(avc_slice(0))).has_active_color,"avc_truncated_vui_replacement_does_not_retain_color");
        tracker.reset();require(!inspect(tracker,annex(avc_slice(0))).has_active_color,"avc_reset_clears_color");
    }++groups;
    {surfxr::HevcSequenceTracker tracker;auto packet=annex(hevc_sps(0,1,true));append(packet,annex(hevc_pps(0,0)));
        append(packet,annex(hevc_sps(1,6,false)));append(packet,annex(hevc_pps(1,1)));append(packet,annex(hevc_slice(0,true)));
        color(inspect(tracker,packet),1,2,"hevc_irap_uses_referenced_sps_not_latest");
        color(inspect(tracker,annex(hevc_slice(1))),6,1,"hevc_predicted_picture_uses_own_pps");
        color(inspect(tracker,annex(hevc_slice(0,false,0,true))),1,2,"hevc_dependent_slice_has_pps");
        require(!inspect(tracker,annex(hevc_slice(2))).has_active_color,"hevc_unknown_pps_no_stale_color");
        auto mixed=annex(hevc_slice(0));append(mixed,annex(hevc_slice(1)));require(!inspect(tracker,mixed).has_active_color,"hevc_mixed_pps_has_no_color");
        auto other=annex(hevc_sps(0,9,false,true,false,1));append(other,annex(hevc_pps(0,0,1)));append(other,annex(hevc_slice(0,false,1)));
        color(inspect(tracker,other),9,1,"hevc_layer_independent_color");
        color(inspect(tracker,annex(hevc_slice(0))),1,2,"hevc_other_layer_did_not_replace_base");
        mixed=annex(hevc_slice(0));append(mixed,annex(hevc_slice(0,false,1)));require(!inspect(tracker,mixed).has_active_color,"hevc_mixed_layers_has_no_color");
        auto broken=annex(hevc_sps(0,9,false));append(broken,{0,0,1,66});require(!inspect(tracker,broken).valid,"hevc_bad_tail_rejected");
        color(inspect(tracker,annex(hevc_slice(0))),1,2,"hevc_bad_tail_preserves_color_cache");
        auto late=annex(hevc_slice(0));append(late,annex(hevc_sps(0,6,false)));
        require(!inspect(tracker,late).has_active_color,"hevc_late_sps_not_applied_to_earlier_picture");
        color(inspect(tracker,annex(hevc_slice(0))),6,1,"hevc_late_sps_available_next_packet");
        require(inspect(tracker,annex(hevc_sps(0,1,true,false))).valid,"hevc_no_vui_replacement");
        color(inspect(tracker,annex(hevc_slice(0))),2,1,"hevc_vui_removal_clears_previous_color");
        auto short_vui=hevc_sps(0,1,true);short_vui.resize(short_vui.size()-3);
        require(inspect(tracker,annex(short_vui)).valid&&!inspect(tracker,annex(hevc_slice(0))).has_active_color,"hevc_truncated_vui_replacement_does_not_retain_color");
        tracker.reset();require(!inspect(tracker,annex(hevc_slice(0))).has_active_color,"hevc_reset_clears_color");
    }++groups;
    return groups;
}
} // namespace
int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--self-test")){std::cout<<"surfxr-video-color-test --self-test (CPU only)\n";return argc==1?0:1;}
    try{const auto groups=tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<groups<<",\"hardware_devices_used\":false,\"network_used\":false}\n";return 0;}
    catch(const std::exception& error){std::cerr<<"video_color_test: "<<error.what()<<'\n';return 1;}
}
