#include "surfxr/h264_sequence.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using Bytes=std::vector<std::uint8_t>;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Writer {
    Bytes data;unsigned at=0;
    void bit(unsigned value){if(at%8==0)data.push_back(0);data.back()|=static_cast<std::uint8_t>((value&1)<<(7-at%8));++at;}
    void bits(unsigned value,unsigned n){for(unsigned i=n;i>0;--i)bit(value>>(i-1));}
    void ue(unsigned value){unsigned n=0;for(unsigned x=value+1;x>1;x>>=1)++n;for(unsigned i=0;i<n;++i)bit(0);bits(value+1,n+1);}
    void se(int value){ue(value>0?2*static_cast<unsigned>(value)-1:2*static_cast<unsigned>(-value));}
    Bytes nal(unsigned type){bit(1);while(at%8)bit(0);Bytes out{static_cast<std::uint8_t>(0x60|type)};unsigned zeros=0;
        for(auto byte:data){if(zeros>=2&&byte<=3){out.push_back(3);zeros=0;}out.push_back(byte);zeros=byte==0?zeros+1:0;}return out;}
};
Bytes sps(unsigned id=0,unsigned width=256,unsigned height=144,unsigned profile=100,unsigned depth=0,unsigned chroma=1,unsigned vui=0,unsigned right_crop=0,bool progressive=true){
    Writer w;w.bits(profile,8);w.bits(0,8);w.bits(51,8);w.ue(id);
    if(profile==100){w.ue(chroma);w.ue(depth);w.ue(depth);w.bit(0);w.bit(0);}
    w.ue(0);w.ue(0);w.ue(0);w.ue(1);w.bit(0);w.ue(width/16-1);w.ue(height/16-1);w.bit(progressive?1:0);w.bit(1);
    w.bit(right_crop?1:0);if(right_crop){w.ue(0);w.ue(right_crop);w.ue(0);w.ue(0);}
    w.bit(vui?1:0);if(vui)w.bits(vui,8);return w.nal(7);
}
Bytes pps(unsigned id=0,unsigned sps_id=0,int qp=0){
    Writer w;w.ue(id);w.ue(sps_id);w.bit(1);w.bit(0);w.ue(0);w.ue(0);w.ue(0);w.bit(0);w.bits(0,2);w.se(qp);w.se(0);w.se(0);w.bit(1);w.bit(0);w.bit(0);return w.nal(8);
}
Bytes slice(unsigned pps_id=0,unsigned type=5){Writer w;w.ue(0);w.ue(type==5?2:0);w.ue(pps_id);w.bits(0x5a,8);return w.nal(type);}
Bytes annex(const Bytes& nal,unsigned code=4,unsigned trailing=0){Bytes out(code-1,0);out.push_back(1);out.insert(out.end(),nal.begin(),nal.end());out.insert(out.end(),trailing,0);return out;}
void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
surfxr::H264SequenceInspection inspect(surfxr::H264SequenceTracker& t,const Bytes& bytes){return t.inspect_packet(bytes.data(),bytes.size());}
Bytes hex(const char* text){Bytes bytes;for(std::size_t i=0;text[i];i+=2){unsigned value=0;for(unsigned j=0;j<2;++j){const char c=text[i+j];value=value*16+static_cast<unsigned>(c>='a'?c-'a'+10:c-'0');}bytes.push_back(static_cast<std::uint8_t>(value));}return bytes;}
unsigned tests(){unsigned groups=0;
    {surfxr::H264SequenceTracker t;Bytes au=annex(sps());append(au,annex(pps(),3));append(au,annex(slice()));
        auto r=inspect(t,au);require(r.valid&&r.has_vcl&&r.has_parameter_sets&&r.random_access&&r.random_access_ready&&r.width==256&&r.height==144&&!r.parameter_sets_changed,"initial_high_idr");
        require(!inspect(t,au).parameter_sets_changed,"unchanged_repeated_sets");Bytes expected=annex(sps());append(expected,annex(pps()));require(t.parameter_sets_annexb()==expected,"canonical_sps_then_pps");
        Bytes padded{0,0};append(padded,annex(sps(),3,5));require(inspect(t,padded).valid&&t.parameter_sets_annexb()==expected,"canonical_startcodes_and_padding");
        require(inspect(t,annex(slice(0,1))).valid&&!inspect(t,annex(slice(0,1))).random_access,"predicted_frame_classification");
        t.reset();require(t.parameter_sets_annexb().empty()&&!inspect(t,annex(slice())).random_access_ready,"deactivate_discards_headers");}++groups;
    {surfxr::H264SequenceTracker t;require(inspect(t,annex(slice())).valid&&!inspect(t,annex(slice())).random_access_ready,"idr_before_parameters_waits");
        require(inspect(t,annex(pps())).valid&&!inspect(t,annex(slice())).random_access_ready,"pps_before_referenced_sps");
        require(inspect(t,annex(sps())).valid&&inspect(t,annex(slice())).random_access_ready,"separate_headers_enable_idr");
        require(!inspect(t,annex(slice(1))).random_access_ready,"unknown_pps_never_uses_latest_sps");
        require(inspect(t,annex(pps(1,1))).parameter_sets_changed&&!inspect(t,annex(slice(1))).random_access_ready,"known_pps_missing_sps");
        require(inspect(t,annex(sps(1,320,192))).parameter_sets_changed,"new_sps_id_reconfigures");
        auto r=inspect(t,annex(slice(1)));require(r.random_access_ready&&r.width==320&&r.height==192,"idr_uses_its_own_sps_dimensions");
        r=inspect(t,annex(slice()));require(r.random_access_ready&&r.width==256,"original_id_preserved");}++groups;
    {surfxr::H264SequenceTracker t;require(inspect(t,annex(sps())).valid&&inspect(t,annex(pps())).valid,"change_baseline");
        const auto r=inspect(t,annex(sps(0,256,144,100,0,1,90)));require(r.valid&&r.parameter_sets_changed&&!r.random_access,"vui_only_change_requires_idr");
        require(!inspect(t,annex(sps(0,256,144,100,0,1,90))).parameter_sets_changed,"updated_vui_is_new_baseline");
        require(inspect(t,annex(pps(0,0,4))).parameter_sets_changed,"pps_qp_change");
        auto a=annex(slice());append(a,annex(slice()));require(inspect(t,a).random_access_ready,"multiple_idr_slices");
        append(a,annex(slice(0,1)));require(!inspect(t,a).valid,"mixed_idr_predicted_access_unit_rejected");}++groups;
    {for(unsigned profile:{66u,77u,88u,100u}){surfxr::H264SequenceTracker t;Bytes a=annex(sps(31,272,144,profile,0,1,0,8));append(a,annex(pps(255,31)));append(a,annex(slice(255)));
        const auto r=inspect(t,a);require(r.valid&&r.random_access_ready&&r.width==256&&r.height==144,"profile_crop_and_max_ids");}}++groups;
    {// Independent libx264 baseline/main/high SPS/PPS, from a local 256x144
     // testsrc2 frame. These exercise real VUI escaping and high-profile PPS.
        const char* sets[][2]={{"6742c00bd901013b011000000300100000030020f142a480","68cb83cb20"},
            {"674d400beca0809d80880000030008000003001078a14cb0","68ebe3cb20"},
            {"6764000bacd941013b011000000300100000030020f1429960","68ebe3cb22c0"}};
        for(const auto& pair:sets){surfxr::H264SequenceTracker t;auto a=annex(hex(pair[0]));append(a,annex(hex(pair[1])));append(a,annex(slice()));
            const auto r=inspect(t,a);require(r.valid&&r.random_access_ready&&r.width==256&&r.height==144,"independent_x264_parameters");}}++groups;
    {surfxr::H264SequenceTracker t;require(inspect(t,annex(sps())).valid,"invalid_baseline");const auto saved=t.parameter_sets_annexb();
        const std::vector<Bytes> bad{{},{0,0,1},{0,0,1,0x67},{9,0,0,1,0x67,0x80},annex({0xe7,0x80}),annex({0x67,0,0,3,4}),annex({0x67,0,0,3}),annex({0x67,0,0,2}),
            annex(sps(32)),annex(pps(256)),annex(pps(0,32)),annex(sps(0,256,144,100,2)),annex(sps(0,256,144,110)),annex(sps(0,256,144,100,0,3)),annex(sps(0,256,144,100,0,1,0,0,false)),
            annex(sps(0,256,144,100,0,1,0,128)),annex(sps(0,112,144)),annex(sps(0,8192,8192)),annex(pps(0,0,26)),annex({0x68,0x80}),annex(slice(256)),annex({0x62,0x80})};
        for(const auto& packet:bad)require(!inspect(t,packet).valid&&t.parameter_sets_annexb()==saved,"malformed_is_atomic");
        auto changed=annex(sps(0,320,192));append(changed,{0,0,1,0x67});require(!inspect(t,changed).valid&&t.parameter_sets_annexb()==saved,"bad_tail_no_partial_commit");
        require(!t.inspect_packet(nullptr,10).valid&&!t.inspect_packet(saved.data(),surfxr::H264SequenceTracker::max_packet_bytes+1).valid,"null_and_packet_limits");
        auto late=annex(slice());append(late,annex(sps()));require(!inspect(t,late).valid,"headers_after_vcl_rejected");}++groups;
    {surfxr::H264SequenceTracker t;auto huge=sps(0,256,144,100,0,1,90);huge.resize(surfxr::H264SequenceTracker::max_parameter_nal+1,0x55);
        require(!inspect(t,annex(huge)).valid,"oversize_parameter_rejected");huge.resize(surfxr::H264SequenceTracker::max_parameter_nal);require(inspect(t,annex(huge)).valid,"exact_parameter_limit");
        t.reset();for(unsigned i=0;i<15;++i){auto nal=sps(i,256,144,100,0,1,90);nal.resize(4096,0x55);require(inspect(t,annex(nal)).valid,"populate_parameter_budget");}
        const auto saved=t.parameter_sets_annexb();auto over=sps(15,256,144,100,0,1,90);over.resize(4096,0x55);require(!inspect(t,annex(over)).valid&&t.parameter_sets_annexb()==saved,"cumulative_parameter_limit_atomic");
        t.reset();Bytes repeated;for(unsigned i=0;i<17;++i)append(repeated,annex(huge));require(!inspect(t,repeated).valid&&t.parameter_sets_annexb().empty(),"per_packet_parameter_limit");}++groups;
    return groups;
}
}
int main(int argc,char** argv){
    if(argc!=2||std::strcmp(argv[1],"--self-test")){std::cout<<"surfxr-h264-sequence-test --self-test (CPU only)\n";return argc==1?0:1;}
    try{const auto n=tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<n<<",\"decoder_used\":false,\"network_used\":false}\n";return 0;}
    catch(const std::exception& e){std::cerr<<"h264_sequence_test: "<<e.what()<<'\n';return 1;}
}
