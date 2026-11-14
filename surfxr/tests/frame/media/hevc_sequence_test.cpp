#include "surfxr/hevc_sequence.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Bytes=std::vector<std::uint8_t>;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// Independent generated identifiers and opaque parameter tails. Full VUI syntax
// is deliberately outside the tracker: changing any VUI/timing byte must count.
struct Writer {
    Bytes bytes;unsigned position=0;
    void bit(unsigned value){if(position%8==0)bytes.push_back(0);bytes.back()|=static_cast<std::uint8_t>((value&1)<<(7-position%8));++position;}
    void bits(unsigned value,unsigned n){for(unsigned i=n;i>0;--i)bit(value>>(i-1));}
    void zero(unsigned n){for(unsigned i=0;i<n;++i)bit(0);}
    void ue(unsigned value){unsigned n=0;for(unsigned x=value+1;x>1;x>>=1)++n;zero(n);bits(value+1,n+1);}
    void finish(){bit(1);while(position%8)bit(0);}
};
Bytes parameter(unsigned type,unsigned id,std::uint8_t vui=90,unsigned sublayers=0,unsigned layer=0){
    Writer w;
    if(type==32){w.bits(id,4);w.bits(15,4);}
    else if(type==34){w.ue(id);w.ue(0);}
    else{
        w.bits(0,4);w.bits(sublayers,3);w.bit(1);w.zero(88);w.bits(120,8);
        for(unsigned i=0;i<sublayers;++i){w.bit(1);w.bit(1);}
        if(sublayers)w.zero((8-sublayers)*2);
        for(unsigned i=0;i<sublayers;++i){w.zero(88);w.bits(90+i,8);}
        w.ue(id);
    }
    w.bits(vui,8);w.finish();Bytes result{static_cast<std::uint8_t>((type<<1)|(layer>>5)),static_cast<std::uint8_t>((layer<<3)|1)};
    unsigned zeros=0;for(const auto byte:w.bytes){if(zeros>=2&&byte<=3){result.push_back(3);zeros=0;}result.push_back(byte);zeros=byte==0?zeros+1:0;}return result;
}
Bytes annex(const Bytes& nal,unsigned start=4,unsigned trailing=0){Bytes result(start-1,0);result.push_back(1);result.insert(result.end(),nal.begin(),nal.end());result.insert(result.end(),trailing,0);return result;}
void append(Bytes& into,const Bytes& value){into.insert(into.end(),value.begin(),value.end());}
surfxr::HevcSequenceInspection inspect(surfxr::HevcSequenceTracker& state,const Bytes& packet){return state.inspect_packet(packet.data(),packet.size());}
unsigned tests(){unsigned groups=0;
    const auto vps=parameter(32,0),sps=parameter(33,0),pps=parameter(34,0);
    {surfxr::HevcSequenceTracker state;Bytes packet=annex(vps);append(packet,annex(sps,3));append(packet,annex(pps));append(packet,annex({38,1,0xa0}));
        const auto first=inspect(state,packet);require(first.valid&&first.has_parameter_sets&&first.has_vps&&first.has_sps&&first.has_pps&&first.has_vcl&&first.random_access_ready&&!first.parameter_sets_changed,"initial_sets_and_irap");
        require(!inspect(state,packet).parameter_sets_changed,"unchanged_repeated_parameter_sets");
        Bytes canonical=annex(vps);append(canonical,annex(sps));append(canonical,annex(pps));require(state.parameter_sets_annexb()==canonical,"ordered_canonical_reinjection_sets");
        const auto saved=state.parameter_sets_annexb();Bytes padded{0,0};append(padded,annex(sps,3,5));const auto repeat=inspect(state,padded);
        require(repeat.valid&&!repeat.parameter_sets_changed&&state.parameter_sets_annexb()==saved,"start_codes_and_trailing_zero_canonicalization");}++groups;
    {surfxr::HevcSequenceTracker state;require(inspect(state,annex(vps)).valid&&!state.has_sps(),"vps_before_sps");
        require(!inspect(state,annex(parameter(32,0,72))).parameter_sets_changed,"changes_before_first_sps_are_not_reconfiguration");
        require(!inspect(state,annex(sps)).parameter_sets_changed&&state.has_sps(),"separate_sps_establishes_baseline");
        require(!inspect(state,annex(pps)).parameter_sets_changed,"first_pps_after_sps_is_initialization");
        const auto change=inspect(state,annex(parameter(33,0,120)));require(change.valid&&change.parameter_sets_changed&&!change.has_vcl&&!change.random_access,"vui_timing_bytes_changed_without_irap");
        require(!inspect(state,annex(parameter(33,0,120))).parameter_sets_changed,"new_sps_repeat_stable");
        require(inspect(state,annex(parameter(34,0,100))).parameter_sets_changed,"pps_change_detected");
        require(inspect(state,annex(parameter(32,0,80))).parameter_sets_changed,"vps_change_detected");
        const auto old=state.parameter_sets_annexb();state.reset();require(!state.has_sps()&&state.parameter_sets_annexb().empty()&&!inspect(state,old).parameter_sets_changed,"explicit_reset_discards_baseline");}++groups;
    {surfxr::HevcSequenceTracker state;const auto irap=annex({38,1,0xa0});
        require(!inspect(state,irap).random_access_ready,"irap_without_headers_not_ready");
        inspect(state,annex(vps));inspect(state,annex(sps));
        require(!inspect(state,irap).random_access_ready,"irap_without_pps_not_ready");
        inspect(state,annex(pps));require(inspect(state,irap).random_access_ready,"cached_headers_make_irap_ready");
        require(!inspect(state,annex({2,1,0xc0})).random_access_ready,"predicted_frame_never_starts_dpb");
        state.reset();require(!inspect(state,irap).random_access_ready,"reset_requires_fresh_headers");}++groups;
    {surfxr::HevcSequenceTracker state;Bytes initial=annex(parameter(33,0,90,2));append(initial,annex(parameter(33,1,72,0)));append(initial,annex(parameter(34,63)));append(initial,annex(parameter(34,0)));
        require(inspect(state,initial).valid,"sublayer_profile_and_max_pps_id");
        require(!inspect(state,annex(parameter(33,0,90,2))).parameter_sets_changed&&!inspect(state,annex(parameter(33,1,72,0))).parameter_sets_changed,"multiple_ids_do_not_alternate_false_changes");
        require(inspect(state,annex(parameter(33,2))).parameter_sets_changed,"new_id_in_known_family_changes_sequence");
        require(!inspect(state,annex(parameter(33,0,90,0,1))).parameter_sets_changed,"layer_ids_have_independent_baseline");
        require(!inspect(state,annex(parameter(33,0,90,2))).parameter_sets_changed,"different_layer_does_not_replace_base_layer");}++groups;
    {surfxr::HevcSequenceTracker state;for(unsigned type=0;type<64;++type){if(type>=32&&type<=34)continue;const auto result=inspect(state,annex({static_cast<std::uint8_t>(type<<1),1},type%2?3:4));
        require(result.valid&&result.has_vcl==(type<=31)&&result.random_access==(type>=16&&type<=23)&&!result.has_parameter_sets,"all_nal_types_vcl_irap_classification");}}++groups;
    {surfxr::HevcSequenceTracker state;require(inspect(state,annex(sps)).valid,"malformed_test_baseline");const auto saved=state.parameter_sets_annexb();
        const std::vector<Bytes> invalid{{},{0,0},{0,0,1},{0,0,1,0x42},{0,0,1,0x80,1},{0,0,1,0x42,0},{9,0,0,1,2,1},annex({66,1,0x80}),annex(parameter(33,16)),annex(parameter(34,64)),annex(parameter(33,0,90,7)),annex({64,1,0,0,3,4}),annex({64,1,0,0,3}),annex({64,1,0,0,2})};
        for(const auto& bad:invalid){const auto result=inspect(state,bad);require(!result.valid&&!result.parameter_sets_changed&&state.parameter_sets_annexb()==saved,"malformed_packet_is_atomic");}
        Bytes changed=annex(parameter(33,0,120));append(changed,{0,0,1,66});require(!inspect(state,changed).valid&&state.parameter_sets_annexb()==saved,"bad_tail_does_not_commit_changed_sps");
        require(!state.inspect_packet(nullptr,10).valid&&!state.inspect_packet(saved.data(),surfxr::HevcSequenceTracker::max_packet_bytes+1).valid,"null_and_packet_size_bounds");
        Bytes split=annex(sps);const auto half=split.size()/2;require(!state.inspect_packet(split.data(),half).valid&&!state.inspect_packet(split.data()+half,split.size()-half).valid,"byte_fragments_are_not_complete_access_units");}++groups;
    {surfxr::HevcSequenceTracker state;auto huge=parameter(34,0);huge.resize(surfxr::HevcSequenceTracker::max_parameter_nal+1,0x55);
        require(!inspect(state,annex(huge)).valid&&state.parameter_sets_annexb().empty(),"oversize_parameter_nal_rejected");
        huge.resize(surfxr::HevcSequenceTracker::max_parameter_nal,0x55);require(inspect(state,annex(huge)).valid,"exact_parameter_nal_limit");
        state.reset();for(unsigned id=0;id<15;++id){auto nal=parameter(34,id);nal.resize(4096,0x55);require(inspect(state,annex(nal)).valid,"bounded_parameter_bank_population");}
        const auto saved=state.parameter_sets_annexb();auto next=parameter(34,15);next.resize(4096,0x55);require(!inspect(state,annex(next)).valid&&state.parameter_sets_annexb()==saved&&saved.size()<=65536,"cumulative_budget_rejects_atomically");
        state.reset();Bytes repeated;for(unsigned i=0;i<17;++i)append(repeated,annex(huge));require(!inspect(state,repeated).valid&&state.parameter_sets_annexb().empty(),"per_packet_parameter_budget");}++groups;
    {for(unsigned offset=0;offset<64;++offset)for(unsigned start:{3u,4u}){
        surfxr::HevcSequenceTracker state;require(inspect(state,annex(sps)).valid,"boundary_baseline");
        Bytes nal{2,1,0xc0};nal.resize(3+offset,0x55);
        // False candidates, escaped 00 00 01 and a real delimiter at every
        // byte alignment. Late SPS detection must still be exact and atomic.
        append(nal,{0,0,3,1,0,1,0,0,3,0,2,1});Bytes packet(offset,0);
        append(packet,annex(nal,start));append(packet,annex(parameter(33,0,120),start,offset%9));
        const auto result=inspect(state,packet);
        require(result.valid&&result.has_vcl&&result.parameter_sets_changed&&!result.has_active_color,"word_boundary_late_sps");
        require(state.parameter_sets_annexb()==annex(parameter(33,0,120)),"word_boundary_canonical_sets");
        const auto saved=state.parameter_sets_annexb();append(packet,{0,0,1});
        require(!inspect(state,packet).valid&&state.parameter_sets_annexb()==saved,"word_boundary_truncated_tail_is_atomic");
    }}++groups;
    return groups;
}
void benchmark(){
    constexpr unsigned iterations=2000,payload_bytes=256*1024;
    std::cout<<"{\"cpu_sequence_benchmark\":true,\"synthetic\":true,\"decoder_used\":false,\"network_used\":false,\"cases\":[";
    for(unsigned pattern=0;pattern<3;++pattern){
        Bytes nal{2,1,0xc0};std::uint32_t random=0x76381839;unsigned zeros=0;
        for(unsigned i=0;i<payload_bytes;++i){random^=random<<13;random^=random>>17;random^=random<<5;
            const auto value=std::uint8_t(pattern==0?random:pattern==1?(i%4==3?1:i%4==2?3:0):1);
            if(pattern==0&&zeros>=2&&value<=3){nal.push_back(3);zeros=0;}
            nal.push_back(value);zeros=value==0?zeros+1:0;
        }
        const auto packet=annex(nal);surfxr::HevcSequenceTracker state;
        require(inspect(state,packet).valid,"benchmark_fixture_invalid");
        const auto before=std::chrono::steady_clock::now();unsigned valid=0;
        for(unsigned i=0;i<iterations;++i){const auto result=inspect(state,packet);valid+=result.valid&&result.has_vcl&&!result.has_parameter_sets;}
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
        require(valid==iterations,"benchmark_sequence_changed");
        if(pattern)std::cout<<',';
        std::cout<<"{\"pattern\":\""<<(pattern==0?"escaped_entropy":pattern==1?"dense_false_prefixes":"dense_01")
            <<"\",\"packet_bytes\":"<<packet.size()<<",\"iterations\":"<<iterations<<",\"ms_per_packet\":"<<elapsed/iterations<<'}';
    }
    std::cout<<"]}\n";
}
}
int main(int argc,char** argv){
    const bool bench=argc==2&&std::strcmp(argv[1],"--benchmark")==0;
    if(!bench&&(argc!=2||std::strcmp(argv[1],"--self-test")!=0)){std::cout<<"surfxr-hevc-sequence-test --self-test | --benchmark (CPU only)\n";return argc==1?0:1;}
    try{if(bench){benchmark();return 0;}const auto count=tests();std::cout<<"{\"status\":\"self_test_passed\",\"scenario_groups\":"<<count<<",\"decoder_used\":false,\"network_used\":false}\n";return 0;}
    catch(const std::exception& e){std::cerr<<"hevc_sequence_test: "<<e.what()<<'\n';return 1;}
}
