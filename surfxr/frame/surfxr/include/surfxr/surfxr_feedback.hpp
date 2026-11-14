#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <atomic>
#include <ostream>
#include <algorithm>

namespace surfxr {struct VrStereoFrame;}
namespace surfxr::transport {
struct ReceiveStreamSnapshot {
    std::uint64_t datagrams=0,bytes=0,packets=0,gaps=0,reordered=0;
    std::uint64_t interval_ns_sum=0,interval_ns_max=0,intervals_over_20ms=0,intervals_over_50ms=0;
    std::uint64_t late_shards=0,reassembly_drops=0;
    std::uint64_t fec_recovered_shards=0,fec_recovered_packets=0;
};
struct ReceiveSnapshot {ReceiveStreamSnapshot video,audio;std::uint64_t idr_requests=0,reference_skips=0;bool video_fec=false;};
class ReceiveStreamCounters {
public:
    std::atomic<std::uint64_t> datagrams{0},bytes{0},packets{0},gaps{0},reordered{0};
    std::atomic<std::uint64_t> late_shards{0},reassembly_drops{0};
    std::atomic<std::uint64_t> fec_recovered_shards{0},fec_recovered_packets{0};
    void packet(std::uint64_t now,bool gap){
        ++packets;if(gap)++gaps;
        if(last_ns_&&now>=last_ns_){const auto dt=now-last_ns_;sum_.fetch_add(dt,std::memory_order_relaxed);
            max_.store(std::max(max_.load(std::memory_order_relaxed),dt),std::memory_order_relaxed);
            if(dt>20'000'000)++over20_;
            if(dt>50'000'000)++over50_;}
        last_ns_=now;
    }
    ReceiveStreamSnapshot snapshot()const{
        return {datagrams.load(),bytes.load(),packets.load(),gaps.load(),reordered.load(),sum_.load(),max_.load(),over20_.load(),over50_.load(),
            late_shards.load(),reassembly_drops.load(),fec_recovered_shards.load(),fec_recovered_packets.load()};
    }
private:
    // Exactly one producer per stream; readers access atomic numeric copies.
    std::uint64_t last_ns_=0;
    std::atomic<std::uint64_t> sum_{0},max_{0},over20_{0},over50_{0};
};
struct ReceiveCounters {
    ReceiveStreamCounters video,audio;
    std::atomic<std::uint64_t> idr_requests{0},reference_skips{0};
    std::atomic_bool video_fec{false};
    ReceiveSnapshot snapshot()const{return {video.snapshot(),audio.snapshot(),idr_requests.load(),reference_skips.load(),video_fec.load()};}
};
inline void write_receive_json(std::ostream& out,const ReceiveSnapshot& value){
    const auto stream=[&](const ReceiveStreamSnapshot& s){out<<"{\"datagrams\":"<<s.datagrams<<",\"bytes\":"<<s.bytes
        <<",\"packets\":"<<s.packets<<",\"gaps\":"<<s.gaps<<",\"reordered\":"<<s.reordered
        <<",\"interval_ns_sum\":"<<s.interval_ns_sum<<",\"interval_ns_max\":"<<s.interval_ns_max
        <<",\"intervals_over_20ms\":"<<s.intervals_over_20ms<<",\"intervals_over_50ms\":"<<s.intervals_over_50ms
        <<",\"late_shards\":"<<s.late_shards<<",\"reassembly_drops\":"<<s.reassembly_drops
        <<",\"fec_recovered_shards\":"<<s.fec_recovered_shards<<",\"fec_recovered_packets\":"<<s.fec_recovered_packets<<'}';};
    out<<"{\"video\":";stream(value.video);out<<",\"audio\":";stream(value.audio);
    out<<",\"idr_requests\":"<<value.idr_requests<<",\"reference_skips\":"<<value.reference_skips
        <<",\"video_fec\":"<<(value.video_fec?"true":"false")<<'}';
}
struct Feedback {
    std::uint64_t timestamp=0,interval=0,decode=0,rendering=0,total=0;
};
// Only successful xrEndFrame submissions enter the eight-entry mailbox.
// CPU elapsed times exclude unknowable panel scanout and remote GPU work.
class FeedbackQueue {
public:
    ReceiveCounters receive;
    void presented(const VrStereoFrame&,std::uint64_t now);
    bool pop(Feedback&);
private:
    std::mutex mutex_;std::deque<Feedback> queue_;
    std::uint64_t last_timestamp_=0,last_tracking_timestamp_=0,last_submit_=0;
};
}
