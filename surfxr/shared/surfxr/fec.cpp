#include "surfxr/surfxr_stream.hpp"
#include <algorithm>

namespace surfxr::transport {
void Reassembler::repair(Pending& p,unsigned group,std::size_t capacity){
    if(!p.total_length||!p.sizes[p.count+group])return;
    const auto begin=group*16,end=std::min(begin+16,p.count);
    unsigned missing=end;
    for(unsigned i=begin;i<end;++i)if(!p.sizes[i]){
        if(missing!=end)return;
        missing=i;
    }
    if(missing==end)return;
    const auto length=missing+1==p.count?p.total_length-std::size_t{missing}*capacity:capacity;
    require(retained_+length<=std::min(std::size_t{32*1024*1024},8*limit_),"surfxr_packet_memory_limit");
    auto* out=p.payload.data()+std::size_t{missing}*capacity;
    const auto* parity=p.payload.data()+std::min(limit_,std::size_t{p.count}*capacity)+group*capacity;
    std::copy(parity,parity+length,out);
    for(unsigned i=begin;i<end;++i)if(i!=missing){
        const auto* data=p.payload.data()+std::size_t{i}*capacity;
        for(std::size_t j=0;j<std::min(length,std::size_t{p.sizes[i]});++j)out[j]^=data[j];
    }
    p.sizes[missing]=static_cast<std::uint16_t>(length);++p.received;p.bytes+=length;retained_+=length;
    p.repaired=true;++recovered_shards_;
}
}
