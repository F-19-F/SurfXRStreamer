#include "surfxr/audio_output.hpp"
#include <algorithm>
#include <cstring>

namespace surfxr {
namespace {constexpr unsigned reserve_frames=576;}
// The cursor can be advanced by the consumer, overflow, or connection reset.
// Per-slot stamps and epoch tags prevent torn stereo samples or old-session
// playback even if the producer overwrites a claimed slot while preempted.
std::uint64_t AudioPcmRing::discard_buffered(std::uint64_t floor) noexcept {
    auto read=buffered_read_.load(std::memory_order_acquire);
    while(read<floor){
        if(buffered_read_.compare_exchange_weak(read,floor,std::memory_order_acq_rel))return floor-read;
    }
    return 0;
}
bool AudioPcmRing::submit_buffered(const std::uint8_t* bytes,const std::array<std::uint8_t,4>& partial,
        std::size_t partial_size,std::size_t complete,std::uint64_t epoch) noexcept {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"Buffered audio needs lock-free 64-bit atomics");
    const auto begin=buffered_written_.load(std::memory_order_relaxed),end=begin+complete/4;
    const auto capacity=capacity_/4,floor=end>capacity?end-capacity:0;
    dropped_old_.fetch_add(discard_buffered(floor),std::memory_order_relaxed);
    for(auto index=std::max(begin,floor);index<end;++index){
        const auto offset=static_cast<std::size_t>(index-begin)*4;
        std::uint32_t pcm=0;
        for(unsigned c=0;c<4;++c){const auto pos=offset+c;
            pcm|=std::uint32_t(pos<partial_size?partial[pos]:bytes[pos-partial_size])<<(c*8);}
        auto& slot=buffered_data_[index%capacity];
        // Sequential consistency orders both tags around the sample. Readers
        // validate twice; no non-atomic PCM storage is shared between threads.
        slot.stamp.store(index*2+1,std::memory_order_seq_cst);
        slot.pcm_epoch.store((epoch<<32)|pcm,std::memory_order_seq_cst);
        slot.stamp.store(index*2+2,std::memory_order_seq_cst);
    }
    buffered_written_.store(end,std::memory_order_release);
    const auto read=buffered_read_.load(std::memory_order_acquire);
    const auto queued=static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity,end>read?end-read:0));
    maximum_.store(std::max(maximum_.load(std::memory_order_relaxed),queued),std::memory_order_relaxed);
    return true;
}
std::size_t AudioPcmRing::read_buffered(std::uint8_t* output,std::size_t bytes) noexcept {
    const auto epoch=reset_epoch_.load(std::memory_order_acquire);
    if(consumer_epoch_!=epoch){
        consumer_epoch_=epoch;primed_=false;last_sample_={};fade_remaining_=0;
        consumer_dropped_=dropped_old_.load(std::memory_order_relaxed);
    }
    bool discontinuity=false;
    std::size_t actual=0;
    auto read=buffered_read_.load(std::memory_order_acquire);
    const auto written=buffered_written_.load(std::memory_order_acquire);
    const auto available=written>read?written-read:0;
    if(!primed_&&available>=std::min<std::size_t>(capacity_/4,bytes/4+reserve_frames)){
        primed_=true;discontinuity=true;
    }
    const bool buffering=!primed_;
    if(buffering)++rebuffer_callbacks_;
    else{
        const auto count=std::min<std::uint64_t>(available,bytes/4);
        // One bounded claim; overflow/reset may win the race. In that case
        // preserve the new cursor and retry at the next playback callback.
        if(buffered_read_.compare_exchange_strong(read,read+count,std::memory_order_acq_rel)){
            for(std::uint64_t i=0;i<count;++i){
                auto& slot=buffered_data_[(read+i)%(capacity_/4)];
                const auto expected=(read+i)*2+2;
                const auto before=slot.stamp.load(std::memory_order_seq_cst);
                const auto value=slot.pcm_epoch.load(std::memory_order_seq_cst);
                const auto after=slot.stamp.load(std::memory_order_seq_cst);
                if(before!=expected||after!=expected||std::uint32_t(value>>32)!=std::uint32_t(epoch)){
                    dropped_old_.fetch_add(count-i,std::memory_order_relaxed);break;
                }
                for(unsigned c=0;c<4;++c)output[actual+c]=static_cast<std::uint8_t>(value>>(c*8));
                actual+=4;
            }
        }
        if(reset_epoch_.load(std::memory_order_acquire)!=epoch){
            dropped_old_.fetch_add(actual/4,std::memory_order_relaxed);
            actual=0;std::memset(output,0,bytes);primed_=false;
        }
    }
    const auto dropped=dropped_old_.load(std::memory_order_relaxed);
    discontinuity|=dropped!=consumer_dropped_;consumer_dropped_=dropped;
    if(actual<bytes&&primed_){primed_=false;++rebuffer_events_;}
    smooth(output,bytes,actual,discontinuity);
    consumed_.fetch_add(actual/4,std::memory_order_relaxed);
    if(actual<bytes){silence_.fetch_add((bytes-actual)/4,std::memory_order_relaxed);if(!buffering)++underruns_;}
    return actual;
}
} // namespace surfxr
