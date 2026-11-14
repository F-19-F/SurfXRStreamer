#include "surfxr/surfxr_stream.hpp"
#include <algorithm>

namespace surfxr::transport {
std::uint32_t big(const std::uint8_t* p,unsigned n){std::uint32_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|p[i];return v;}
void big(Bytes& b,std::uint32_t v,unsigned n){for(unsigned i=0;i<n;++i)b.push_back(static_cast<std::uint8_t>(v>>(8*(n-1-i))));}
std::uint32_t little(const std::uint8_t* p,unsigned n){std::uint32_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint32_t{p[i]}<<(8*i);return v;}
void Framer::feed(const std::uint8_t* p,std::size_t n,const std::function<void(Bytes)>& sink){
    while(n){
        const auto target=expected_?expected_+4:4;
        const auto count=std::min(n,target-pending_.size());pending_.insert(pending_.end(),p,p+count);p+=count;n-=count;
        if(pending_.size()==4&&!expected_){expected_=little_endian_?little(pending_.data(),4):big(pending_.data(),4);require(expected_>0&&expected_<=limit_,"surfxr_invalid_frame_length");}
        if(expected_&&pending_.size()==expected_+4){
            Bytes body(pending_.begin()+4,pending_.end());pending_.clear();expected_=0;sink(std::move(body));
        }
    }
}
namespace {
bool later(std::uint32_t a,std::uint32_t b){return a!=b&&(a-b)<0x80000000u;}
constexpr std::size_t max_bytes=32*1024*1024,max_packet=8*1024*1024;
}
void Reassembler::erase(std::size_t i){retained_-=pending_[i].bytes;allocated_-=pending_[i].payload.size();pending_.erase(pending_.begin()+static_cast<std::ptrdiff_t>(i));}
std::optional<Packet> Reassembler::accept(const Bytes& shard,std::uint64_t now){
    return accept(shard.data(),shard.size(),now);
}
std::optional<Packet> Reassembler::accept(const std::uint8_t* shard,std::size_t size,std::uint64_t now){
    const unsigned prefix=modern_?14:18,offset=modern_?0:4;
    require(shard&&size>=prefix&&size<=packet_size_&&(modern_||big(shard,4)==size-4),"surfxr_invalid_shard_length");
    const auto read=[&](const std::uint8_t* p,unsigned n){return modern_?little(p,n):big(p,n);};
    const auto stream=read(shard+offset,2),index=read(shard+offset+2,4),count=read(shard+offset+6,4),part=read(shard+offset+10,4);
    const bool fec=video_fec_&&stream==3;
    const auto capacity=packet_size_-prefix-(fec?4:0),groups=fec?(count+15)/16:0;
    require(stream<5&&(streams_&(1u<<stream))&&count>0&&count<=(limit_+capacity-1)/capacity&&part<count+groups,"surfxr_invalid_shard_header");
    const bool parity=part>=count;
    std::uint32_t total=0;
    if(parity){
        require(size==packet_size_,"surfxr_invalid_parity_length");total=little(shard+prefix,4);
        require(total<=limit_&&total>std::size_t{count-1}*capacity&&total<=std::size_t{count}*capacity,"surfxr_invalid_parity_length");
    }else{
        require(size<=prefix+capacity&&std::size_t{part}*capacity+size-prefix<=limit_,"surfxr_packet_memory_limit");
        require(part+1==count||size==prefix+capacity,"surfxr_short_nonfinal_shard");
    }
    for(std::size_t i=0;i<pending_.size();)if(now>=pending_[i].started&&now-pending_[i].started>100'000'000){erase(i);++dropped_;}else ++i;
    if(last_[stream]&&!later(index,*last_[stream])){
        // Count older-index shards discarded after delivery advanced. These
        // include stale duplicates; equal-index duplicates are excluded.
        if(index!=*last_[stream])++late_shards_;
        return {};
    }
    auto it=std::find_if(pending_.begin(),pending_.end(),[&](const auto& p){return p.stream==stream&&p.index==index;});
    if(it==pending_.end()){
        const auto allocation=std::min(limit_,std::size_t{count}*capacity)+groups*capacity;
        while(pending_.size()>=8||allocated_+allocation>std::min(max_bytes,8*limit_)){erase(0);++dropped_;}
        pending_.push_back({stream,index,count,now,Bytes(allocation),std::vector<std::uint16_t>(count+groups),0,0});
        allocated_+=allocation;it=std::prev(pending_.end());
    }
    require(it->count==count,"surfxr_inconsistent_shard_count");auto& saved=it->sizes[part];
    if(parity){
        require(!it->total_length||it->total_length==total,"surfxr_conflicting_parity_length");it->total_length=total;
        require(!it->sizes[count-1]||it->sizes[count-1]==total-std::size_t{count-1}*capacity,"surfxr_conflicting_parity_length");
    }else if(it->total_length&&part+1==count)
        require(size-prefix==it->total_length-std::size_t{count-1}*capacity,"surfxr_conflicting_parity_length");
    const auto* data=shard+prefix+(parity?4:0);
    const auto length=size-prefix-(parity?4:0),position=parity?
        std::min(limit_,std::size_t{count}*capacity)+std::size_t{part-count}*capacity:std::size_t{part}*capacity;
    if(saved){
        require(saved==length&&std::equal(data,shard+size,it->payload.data()+position),"surfxr_conflicting_duplicate_shard");return pop_ready(now);
    }
    require(length&&it->bytes+length<=it->payload.size()&&retained_+length<=std::min(max_bytes,8*limit_),"surfxr_packet_memory_limit");
    std::copy(data,shard+size,it->payload.data()+position);
    saved=static_cast<std::uint16_t>(length);if(!parity)++it->received;it->bytes+=length;retained_+=length;
    if(fec)repair(*it,parity?part-count:part/16,capacity);
    return pop_ready(now);
}
std::optional<Packet> Reassembler::pop_ready(std::uint64_t now){
    for(unsigned stream=0;stream<last_.size();++stream){
        const auto expected=last_[stream]?std::uint32_t(*last_[stream]+1):0u;
        auto chosen=pending_.end();
        for(auto it=pending_.begin();it!=pending_.end();++it)
            if(it->stream==stream&&it->received==it->count&&
               (chosen==pending_.end()||std::uint32_t(it->index-expected)<std::uint32_t(chosen->index-expected)))chosen=it;
        if(chosen==pending_.end())continue;
        const bool gap=chosen->index!=expected;
        if(gap&&reorder_ns_){
            if(!gap_started_[stream])gap_started_[stream]=now;
            if(now<*gap_started_[stream]||now-*gap_started_[stream]<reorder_ns_)continue;
        }
        if(!gap&&gap_started_[stream])++reordered_;
        gap_started_[stream].reset();const auto index=chosen->index;
        Packet packet{stream,index,gap,{}};
        const auto bytes=chosen->bytes;
        // Account the full allocation before moving it; the final datagram
        // defines the real payload length, with no missing/uninitialized gaps.
        allocated_-=chosen->payload.size();retained_-=bytes;
        std::size_t payload_size=0;for(unsigned i=0;i<chosen->count;++i)payload_size+=chosen->sizes[i];
        if(chosen->repaired)++recovered_packets_;
        chosen->payload.resize(payload_size);packet.bytes=std::move(chosen->payload);
        pending_.erase(chosen);last_[stream]=index;
        for(std::size_t i=0;i<pending_.size();)if(pending_[i].stream==stream&&!later(pending_[i].index,index)){++dropped_;erase(i);}else ++i;
        return packet;
    }
    return {};
}
std::optional<std::uint64_t> Reassembler::next_ready_ns(std::uint64_t now)const{
    std::optional<std::uint64_t> deadline;
    for(unsigned stream=0;stream<last_.size();++stream){
        const auto expected=last_[stream]?std::uint32_t(*last_[stream]+1):0u;
        for(const auto& packet:pending_)if(packet.stream==stream&&packet.received==packet.count){
            const auto ready=packet.index==expected||!reorder_ns_?now:
                gap_started_[stream]?*gap_started_[stream]+reorder_ns_:now;
            if(!deadline||ready<*deadline)deadline=ready;
        }
    }
    return deadline;
}
std::vector<Bytes> shards(unsigned stream,std::uint32_t index,const Bytes& payload,unsigned size,bool modern){
    require(stream<5&&size>=64&&size<=65503&&!payload.empty()&&payload.size()<=max_packet,"surfxr_invalid_outbound_packet");
    const auto capacity=size+4-18;const auto count=(payload.size()+capacity-1)/capacity;std::vector<Bytes> result;
    for(std::size_t part=0;part<count;++part){const auto begin=part*capacity,n=std::min(payload.size()-begin,std::size_t{capacity});Bytes b;
        b.reserve(n+18);
        if(modern){Writer prefix;prefix.integer(stream,2);prefix.integer(index,4);prefix.integer(count,4);prefix.integer(part,4);b=std::move(prefix.bytes);}
        else{big(b,static_cast<std::uint32_t>(n+14),4);big(b,stream,2);big(b,index,4);big(b,static_cast<std::uint32_t>(count),4);big(b,static_cast<std::uint32_t>(part),4);}
        b.insert(b.end(),payload.begin()+static_cast<std::ptrdiff_t>(begin),payload.begin()+static_cast<std::ptrdiff_t>(begin+n));result.push_back(std::move(b));
    }
    return result;
}
void TcpPacketFramer::feed(const std::uint8_t* p,std::size_t n,const std::function<void(Packet)>& sink){
    while(n){const auto target=expected_?expected_+10:10,count=std::min(n,target-pending_.size());
        pending_.insert(pending_.end(),p,p+count);p+=count;n-=count;
        if(pending_.size()==10&&!expected_){expected_=little(pending_.data()+6,4);const auto stream=little(pending_.data(),2);
            require(expected_&&expected_<=limit_&&stream<5&&(streams_&(1u<<stream)),"surfxr_invalid_frame_length");}
        if(expected_&&pending_.size()==expected_+10){const auto stream=little(pending_.data(),2),index=little(pending_.data()+2,4);
            Packet packet{stream,index,last_[stream]?index!=std::uint32_t(*last_[stream]+1):index!=0,Bytes(pending_.begin()+10,pending_.end())};
            pending_.clear();expected_=0;last_[stream]=index;sink(std::move(packet));
        }
    }
}
}
