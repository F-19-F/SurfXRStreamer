#pragma once
#include "socket.hpp"
#include <cerrno>
#include <deque>
#include <sys/eventfd.h>
#include <unistd.h>

namespace surfxr::transport {
// One producer and one consumer per lane. The event describes this queue,
// rather than the physical socket already drained by the transport owner.
class UnifiedQueue {
    struct Entry {Bytes bytes;std::size_t offset=0;};
    std::mutex mutex_;std::deque<Entry> entries_;
    std::size_t total_=0;unsigned generation_=0;
    const int event_=::eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    void drain_locked(){std::uint64_t value;while(::read(event_,&value,sizeof(value))<0&&errno==EINTR){}}
    void signal_locked(){
        const std::uint64_t value=1;ssize_t count;
        do{count=::write(event_,&value,sizeof(value));}while(count<0&&errno==EINTR);
        require(count==sizeof(value)||(count<0&&errno==EAGAIN),"surfxr_unified_queue_event_failed");
    }
public:
    UnifiedQueue(){require(event_>=0,"surfxr_unified_queue_event_failed");}
    ~UnifiedQueue(){::close(event_);}
    UnifiedQueue(const UnifiedQueue&)=delete;
    int fd()const{return event_;}
    void wake(){std::lock_guard<std::mutex> lock(mutex_);signal_locked();}
    void wake(unsigned generation){std::lock_guard<std::mutex> lock(mutex_);if(generation==generation_)signal_locked();}
    void clear(unsigned generation){
        std::lock_guard<std::mutex> lock(mutex_);entries_.clear();total_=0;generation_=generation;drain_locked();
    }
    void push(Bytes bytes,std::size_t budget,unsigned generation,const char* check,std::size_t offset=0){
        std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_)return;
        // Charge the complete retained allocation, including the generation
        // prefix, until the entry leaves the queue.
        require(offset<bytes.size()&&bytes.size()<=budget&&total_<=budget-bytes.size(),check);
        const bool empty=entries_.empty();total_+=bytes.size();entries_.push_back({std::move(bytes),offset});
        if(empty)signal_locked();
    }
    bool readable(unsigned generation){std::lock_guard<std::mutex> lock(mutex_);return generation==generation_&&!entries_.empty();}
    std::size_t read(std::uint8_t* data,std::size_t capacity,bool tcp,unsigned generation){
        std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_||entries_.empty())return 0;
        auto& entry=entries_.front();const auto size=entry.bytes.size()-entry.offset;
        require(tcp||size<=capacity,"surfxr_unified_datagram_bound");const auto count=std::min(size,capacity);
        std::memcpy(data,entry.bytes.data()+entry.offset,count);entry.offset+=count;
        if(entry.offset==entry.bytes.size()){total_-=entry.bytes.size();entries_.pop_front();if(entries_.empty())drain_locked();}
        return count;
    }
    unsigned read_datagrams(std::size_t capacity,unsigned count,unsigned generation,
                           const std::function<void(const std::uint8_t*,std::size_t)>& sink){
        unsigned used=0;
        for(;used<count;++used){Entry entry;
            {std::lock_guard<std::mutex> lock(mutex_);if(generation!=generation_||entries_.empty())break;
                entry=std::move(entries_.front());entries_.pop_front();total_-=entry.bytes.size();if(entries_.empty())drain_locked();}
            const auto size=entry.bytes.size()-entry.offset;require(size<=capacity,"surfxr_unified_datagram_bound");
            // Never hold the queue mutex over decoder/reassembly callbacks.
            // This owner keeps the plaintext immutable until the sink returns.
            sink(entry.bytes.data()+entry.offset,size);
        }
        return used;
    }
};
}
