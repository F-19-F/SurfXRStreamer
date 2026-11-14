#include "stream.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

using namespace surfxr::transport;
namespace {
constexpr unsigned mtu=1400,capacity=mtu-18;
Bytes payload(){
    Bytes bytes(50'123);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::uint8_t>(i*37+i/101);
    return bytes;
}
std::vector<Bytes> fixture(std::uint32_t packet){
    const auto bytes=payload();auto data=shards(3,packet,bytes,mtu-4,true);
    const auto count=static_cast<unsigned>(data.size());std::vector<Bytes> wire;
    for(unsigned begin=0;begin<count;begin+=16){
        Bytes parity(mtu,0);Writer w;w.raw_integer(3,2);w.raw_integer(packet,4);w.raw_integer(count,4);w.raw_integer(count+begin/16,4);
        w.raw_integer(bytes.size(),4);std::copy(w.bytes.begin(),w.bytes.end(),parity.begin());
        for(unsigned part=begin;part<std::min(begin+16,count);++part){
            for(std::size_t j=14;j<data[part].size();++j)parity[18+j-14]^=data[part][j];
            wire.push_back(data[part]);
        }
        wire.push_back(std::move(parity));
    }
    return wire;
}
template<class F> void rejects(F&& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"test_fec_expected_rejection");}
void replay(const std::vector<Bytes>& wire,const std::set<unsigned>& lost,bool reverse=false){
    Reassembler receiver(mtu,true,8*1024*1024,1u<<3,2'000'000,true);std::optional<Packet> result;
    const auto send=[&](const Bytes& shard){
        const auto part=little(shard.data()+10,4);if(lost.count(part))return;
        if(auto packet=receiver.accept(shard,100)){
            require(!result,"test_fec_duplicate_publication");result=std::move(packet);
        }
    };
    if(reverse)for(auto it=wire.rbegin();it!=wire.rend();++it)send(*it);
    else for(const auto& shard:wire)send(shard);
    require(result&&result->bytes==payload()&&!result->loss&&result->index==0,"test_fec_exact_recovered_bytes");
    // Parity-first reordering can repair a shard before its delayed duplicate.
    require(receiver.retained_bytes()==0&&(reverse?receiver.recovered_shards()>=lost.size():receiver.recovered_shards()==lost.size()),"test_fec_repair_accounting");
    require(receiver.recovered_packets()==(receiver.recovered_shards()?1:0),"test_fec_recovered_packet_count");
}
}
void fec_checks(){
    const auto wire=fixture(0);const auto count=little(wire.front().data()+6,4);
    replay(wire,{});
    // Every data position, including short final NAL bytes, must be repairable.
    for(unsigned part=0;part<count;++part){replay(wire,{part});replay(wire,{part},true);}
    replay(wire,{0,16,count-1});replay(wire,{0,16,count-1},true);
    // Parity is optional when no data is lost; a duplicate cannot count twice.
    auto no_parity=wire;no_parity.erase(std::remove_if(no_parity.begin(),no_parity.end(),[&](const auto& s){return little(s.data()+10,4)>=count;}),no_parity.end());
    replay(no_parity,{});auto duplicate=wire;duplicate.insert(duplicate.begin()+1,wire.front());replay(duplicate,{1});
    Reassembler burst(mtu,true,8*1024*1024,1u<<3,2'000'000,true);
    for(const auto& shard:wire)if(little(shard.data()+10,4)>1)require(!burst.accept(shard,1),"test_fec_must_not_invent_two_missing_shards");
    for(const auto& shard:fixture(1))require(!burst.accept(shard,2),"test_fec_gap_preserves_reorder_grace");
    require(!burst.pop_ready(2'000'001),"test_fec_gap_not_early");auto next=burst.pop_ready(2'000'002);
    require(next&&next->index==1&&next->loss&&next->bytes==payload()&&burst.retained_bytes()==0,"test_fec_unrepairable_uses_existing_loss_recovery");
    rejects([&]{Reassembler old(mtu,true);old.accept(wire.front(),1);});
    rejects([&]{Reassembler receiver(mtu,true,8*1024*1024,1u<<3,0,true);auto bad=wire[16];bad.resize(mtu-1);receiver.accept(bad,1);});
    rejects([&]{Reassembler receiver(mtu,true,8*1024*1024,1u<<3,0,true);auto bad=wire[16];std::fill(bad.begin()+14,bad.begin()+18,0);receiver.accept(bad,1);});
    rejects([&]{Reassembler receiver(mtu,true,8*1024*1024,1u<<3,0,true);auto bad=wire[16];receiver.accept(bad,1);bad[14]^=1;receiver.accept(bad,2);});
    rejects([&]{Reassembler receiver(mtu,true,8*1024*1024,1u<<3,0,true);auto bad=wire.front();receiver.accept(bad,1);bad.back()^=1;receiver.accept(bad,2);});
    // FEC storage (data plus parity) also obeys the existing aggregate budget.
    Reassembler bounded(mtu,true,8*1024*1024,1u<<3,0,true);
    for(unsigned i=0;i<12;++i){auto partial=wire.front();for(unsigned j=0;j<4;++j)partial[2+j]=static_cast<std::uint8_t>(i>>(8*j));bounded.accept(partial,1);}
    require(bounded.dropped()==4&&bounded.retained_bytes()==8*capacity,"test_fec_bounded_pending_packets");
}
void fec_rust_fixture(const char* path){
    std::ifstream file(path,std::ios::binary);require(bool(file),"test_fec_rust_fixture_missing");
    const Bytes bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};std::vector<Bytes> wire;
    for(std::size_t at=0;at<bytes.size();){
        require(bytes.size()-at>=2,"test_fec_fixture_length");const auto length=little(bytes.data()+at,2);at+=2;
        require(length&&length<=bytes.size()-at,"test_fec_fixture_length");wire.emplace_back(bytes.begin()+at,bytes.begin()+at+length);at+=length;
    }
    require(wire==fixture(0),"test_fec_cpp_rust_wire_equality");
    const auto count=little(wire.front().data()+6,4);replay(wire,{0,16,count-1});
}
