#pragma once
#include "surfxr/surfxr_codec.hpp"
#include <array>
#include <deque>
#include <functional>
#include <optional>

namespace surfxr::transport {
std::uint32_t big(const std::uint8_t* p,unsigned n);
void big(Bytes& b,std::uint32_t v,unsigned n);
std::uint32_t little(const std::uint8_t* p,unsigned n);
// All framing lengths are bounded before allocation. Packet size includes
class Framer {
public:
    explicit Framer(std::size_t limit,bool little_endian=false):limit_(limit),little_endian_(little_endian){}
    void feed(const std::uint8_t*,std::size_t,const std::function<void(Bytes)>&);
    std::size_t needed()const{return (expected_?expected_+4:4)-pending_.size();}
private:
    std::size_t limit_;Bytes pending_;std::size_t expected_=0;bool little_endian_;
};
struct Packet {unsigned stream=0;std::uint32_t index=0;bool loss=false;Bytes bytes;};
class Reassembler {
public:
    explicit Reassembler(unsigned packet_size,bool modern=false,std::size_t limit=8*1024*1024,unsigned streams=0x1f,std::uint64_t reorder_ns=0,bool video_fec=false):
        packet_size_(packet_size+(modern?0:4)),modern_(modern),limit_(limit),streams_(streams),reorder_ns_(reorder_ns),video_fec_(video_fec){
        require(!video_fec||modern,"surfxr_invalid_video_fec");
    }
    std::optional<Packet> accept(const Bytes& shard,std::uint64_t now_ns);
    std::optional<Packet> accept(const std::uint8_t*,std::size_t,std::uint64_t now_ns);
    std::optional<Packet> pop_ready(std::uint64_t now_ns);
    // Deadline for a complete packet waiting on a sequence gap. Incomplete
    // packets alone need no polling: arrival owns their expiry/repair checks.
    std::optional<std::uint64_t> next_ready_ns(std::uint64_t now_ns)const;
    std::size_t retained_bytes()const{return retained_;}
    std::uint64_t dropped()const{return dropped_;}
    std::uint64_t reordered()const{return reordered_;}
    std::uint64_t late_shards()const{return late_shards_;}
    std::uint64_t recovered_shards()const{return recovered_shards_;}
    std::uint64_t recovered_packets()const{return recovered_packets_;}
private:
    struct Pending {
        unsigned stream;std::uint32_t index,count;std::uint64_t started;
        Bytes payload;std::vector<std::uint16_t> sizes;std::size_t received=0,bytes=0;
        std::uint32_t total_length=0;bool repaired=false;
    };
    unsigned packet_size_;bool modern_;std::deque<Pending> pending_;std::size_t retained_=0;
    std::size_t limit_;unsigned streams_;std::size_t allocated_=0;
    std::uint64_t reorder_ns_=0,reordered_=0,late_shards_=0;
    bool video_fec_=false;std::uint64_t recovered_shards_=0,recovered_packets_=0;
    std::array<std::optional<std::uint64_t>,5> gap_started_{};
    std::array<std::optional<std::uint32_t>,5> last_{};std::uint64_t dropped_=0;
    void erase(std::size_t index);
    void repair(Pending& packet,unsigned group,std::size_t capacity);
};
std::vector<Bytes> shards(unsigned stream,std::uint32_t index,const Bytes& payload,unsigned packet_size,bool modern=false);
class TcpPacketFramer {
public:
    explicit TcpPacketFramer(std::size_t limit=8*1024*1024,unsigned streams=0x1f):limit_(limit),streams_(streams){}
    void feed(const std::uint8_t*,std::size_t,const std::function<void(Packet)>&);
private:
    std::size_t limit_;unsigned streams_;
    Bytes pending_;std::size_t expected_=0;
    std::array<std::optional<std::uint32_t>,5> last_{};
};
}
