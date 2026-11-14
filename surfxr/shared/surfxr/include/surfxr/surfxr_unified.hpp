#pragma once
#include "surfxr/surfxr_codec.hpp"
#include <chrono>

namespace surfxr::transport::unified {
// Authenticated peers may be briefly descheduled during PC game startup.
// This is a silence budget, not permission to ignore EOF or revoked tickets.
inline constexpr auto stall_timeout=std::chrono::seconds(15);
enum class Kind:unsigned {Hello=0,Begin=1,End=2,Control=3,Ended=4,KeepAlive=5,Tracking=6,TrackingBegin=7,Settings=8,Keyboard=9};
struct Record {Kind kind=Kind::KeepAlive;std::uint32_t generation=0;Bytes body;};
inline Bytes record(Kind kind,std::uint32_t generation,const Bytes& body={}){
    require(body.size()<=2*1024*1024,"surfxr_unified_control_bound");
    Writer w;w.integer(static_cast<unsigned>(kind),1);w.integer(generation,4);w.bytes.insert(w.bytes.end(),body.begin(),body.end());return w.bytes;
}
inline Record parse(const Bytes& bytes){
    Reader r(bytes);Record value;const auto kind=r.integer(1);require(kind<=9,"surfxr_unified_control_kind");
    value.kind=static_cast<Kind>(kind);value.generation=static_cast<std::uint32_t>(r.integer(4));value.body.assign(r.data(),r.data()+r.remaining());return value;
}
inline Bytes datagram(std::uint32_t generation,const std::uint8_t* shard,std::size_t size){
    require(size>=14&&size<=2048,"surfxr_unified_datagram_bound");Writer w;w.integer(generation,4);w.bytes.insert(w.bytes.end(),shard,shard+size);return w.bytes;
}
}
