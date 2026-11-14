#pragma once
#include "surfxr/surfxr_codec.hpp"
#include <array>
#include <cstdint>

namespace surfxr::transport::pose_wire {
// v2 carries calibration evidence. Capability revision 2 makes quality advisory
// and permits continuous holdover. Upstream SURFXR and
// optional gaze bytes remain unchanged. All extension integers are fixed LE.
constexpr std::uint32_t stamp_magic=0x31505645,clock_magic=0x31435645,version=1;
constexpr std::size_t stamp_size=48,calibrated_stamp_size=80,request_size=16,response_size=40;
constexpr std::uint64_t max_age_ns=200'000'000;
struct Stamp {
    std::uint64_t generation=0,epoch=0,sequence=0,server_ns=0,client_ns=0;
    std::uint64_t calibration=0,error_ns=0,rate_ppb=0,calibrated_client_ns=0;
};
inline void put(Bytes& bytes,std::uint64_t value,unsigned size){
    for(unsigned i=0;i<size;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
inline std::uint64_t get(const std::uint8_t* bytes,unsigned size){
    std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t(bytes[i])<<(i*8);return value;
}
inline Bytes encode(Stamp value){
    Bytes bytes;bytes.reserve(value.calibration?calibrated_stamp_size:stamp_size);
    put(bytes,stamp_magic,4);put(bytes,value.calibration?2:version,4);
    for(auto field:{value.generation,value.epoch,value.sequence,value.server_ns,value.client_ns})put(bytes,field,8);
    if(value.calibration)for(auto field:{value.calibration,value.error_ns,value.rate_ppb,value.calibrated_client_ns})put(bytes,field,8);
    return bytes;
}
inline Bytes request(std::uint64_t client_ns){
    Bytes bytes;put(bytes,clock_magic,4);put(bytes,version,4);put(bytes,client_ns,8);return bytes;
}
inline bool response(const Bytes& bytes,std::array<std::uint64_t,4>& values){
    if(bytes.size()!=response_size||get(bytes.data(),4)!=clock_magic||get(bytes.data()+4,4)!=version)return false;
    for(unsigned i=0;i<4;++i)values[i]=get(bytes.data()+8+i*8,8);
    return true;
}
} // namespace surfxr::transport::pose_wire
