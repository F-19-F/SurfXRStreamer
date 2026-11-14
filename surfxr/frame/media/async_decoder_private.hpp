#pragma once
#include "surfxr/async_video_decoder.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#include "decoder_wake.hpp"

namespace surfxr {
namespace async_decoder_detail {
constexpr std::size_t kMaxPackets=4,kMaxPackedPackets=8,kMaxBytes=16u*1024u*1024u;
inline std::uint64_t steady_ns(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
// The private adapter permits deterministic concurrency tests without opening
// a hardware device; production always constructs the real VideoDecoder here.
struct DecoderOwner {
    virtual ~DecoderOwner()=default;
    virtual void configure(std::uint32_t,std::uint32_t,std::uint32_t,bool)=0;
    virtual void deactivate()=0;
    virtual void submit(const std::uint8_t*,std::size_t,std::uint64_t,std::shared_ptr<const VrVideoMetadata>,std::uint64_t,std::optional<std::uint64_t>)=0;
    virtual std::size_t poll()=0;
    virtual std::array<int,2> readiness_fds()const=0;
    virtual bool idle_poll_needed()const=0;
    virtual DecoderStats statistics() const=0;
    virtual bool needs_keyframe() const=0;
    virtual void keyframe_request_queued()=0;
};
}
using namespace async_decoder_detail;
struct AsyncVideoDecoder::Impl {
    enum class CommandKind {Configure,Deactivate};
    struct Command {CommandKind kind;std::uint64_t id;std::uint32_t codec=0,width=0,height=0;};
    struct Packet {
        std::vector<std::uint8_t> bytes;
        std::size_t offset=0;
        std::uint64_t timestamp=0,submitted_ns=0;
        std::shared_ptr<const VrVideoMetadata> metadata;
        std::optional<std::uint64_t> output_epoch;
    };
    mutable std::mutex mutex;
    std::mutex join_mutex;
    std::condition_variable changed;
    decoder_detail::DecoderWake wake;
    std::thread worker;
    std::deque<Packet> packets;
    std::optional<Command> command;
    std::exception_ptr failure;
    DecoderStats stats;
    FrameDemand demand;
    const std::size_t max_packets;
    bool initialized=false,stopping=false,configured=false,keyframe=false;
    bool decoder_keyframe=false,keyframe_ack_pending=false;
    std::uint64_t keyframe_version=0,keyframe_ack_version=0;
    mutable std::uint64_t observed_keyframe_version=0;
    std::size_t outstanding_packets=0,outstanding_bytes=0;
    std::uint64_t next_command=0,completed_command=0;
    std::uint64_t peak_packets=0,peak_bytes=0,backpressure_ns=0,discarded_packets=0;

    explicit Impl(std::function<std::unique_ptr<DecoderOwner>()> factory,FrameDemand frame_demand={},std::size_t packet_limit=kMaxPackets)
        :demand(std::move(frame_demand)),max_packets(packet_limit){
        worker=std::thread([this,factory=std::move(factory)]()mutable{run(std::move(factory));});
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock,[&]{return initialized||failure;});
        if(failure){const auto error=failure;lock.unlock();stop();std::rethrow_exception(error);}
    }
    ~Impl(){stop();}
    void notify()noexcept{changed.notify_all();wake.notify();}
    void throw_locked()const{
        if(failure)std::rethrow_exception(failure);
        if(stopping)throw VideoDecodeError("async_decoder_stopped");
    }
    void discard_locked(){
        discarded_packets+=packets.size();
        for(const auto& packet:packets){--outstanding_packets;outstanding_bytes-=packet.bytes.size();}
        packets.clear();
    }
    void stop()noexcept{
        // Serialize joins as well as making sequential stop/destruction cheap.
        std::lock_guard<std::mutex> joining(join_mutex);
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;discard_locked();command.reset();}
        notify();
        if(worker.joinable())worker.join();
    }
    void barrier(CommandKind kind,std::uint32_t codec=0,std::uint32_t width=0,std::uint32_t height=0){
        std::unique_lock<std::mutex> lock(mutex);throw_locked();
        // Commands are owned by the receiver. Waiting also permits a stop to
        // unblock this barrier while a driver call is being retired.
        changed.wait(lock,[&]{return !command||stopping||failure;});throw_locked();
        discard_locked();keyframe_ack_pending=false;
        const auto id=++next_command;command=Command{kind,id,codec,width,height};notify();
        changed.wait(lock,[&]{return completed_command>=id||stopping||failure;});throw_locked();
    }
    bool submit(const std::uint8_t* data,std::size_t size,std::uint64_t timestamp,std::shared_ptr<const VrVideoMetadata> metadata,bool wait=true,
                std::vector<std::uint8_t>* owner=nullptr,std::size_t offset=0){
        const auto original=steady_ns();
        const auto retained=owner?owner->size():size;
        if(!data||!size||retained>kMaxBytes)throw VideoDecodeError("invalid_encoded_packet");
        // Capture before queue/backpressure waits, not when the worker later
        // enters the codec. Old input must not gain a fresh epoch on resume.
        const auto epoch=demand?std::optional<std::uint64_t>{demand()}:std::nullopt;
        std::unique_lock<std::mutex> lock(mutex);throw_locked();
        if(!configured)throw VideoDecodeError("decoder_not_configured_or_finished");
        const auto room=[&]{return outstanding_packets<max_packets&&retained<=kMaxBytes-outstanding_bytes;};
        if(!room()){
            if(!wait){++discarded_packets;return false;}
            const auto before=steady_ns();changed.wait(lock,[&]{return room()||stopping||failure;});
            backpressure_ns+=steady_ns()-before;throw_locked();
            if(!configured)throw VideoDecodeError("decoder_not_configured_or_finished");
        }
        Packet packet;if(owner){packet.bytes=std::move(*owner);packet.offset=offset;}else packet.bytes.assign(data,data+size);packet.timestamp=timestamp;
        packet.submitted_ns=original;packet.metadata=std::move(metadata);packet.output_epoch=epoch;packets.push_back(std::move(packet));
        ++outstanding_packets;outstanding_bytes+=retained;
        peak_packets=std::max(peak_packets,static_cast<std::uint64_t>(outstanding_packets));
        peak_bytes=std::max(peak_bytes,static_cast<std::uint64_t>(outstanding_bytes));
        lock.unlock();notify();return true;
    }
    void snapshot(DecoderOwner& decoder){
        const auto next=decoder.statistics();const auto needs=decoder.needs_keyframe();
        std::lock_guard<std::mutex> lock(mutex);stats=next;
        if(needs&&!decoder_keyframe)++keyframe_version;
        decoder_keyframe=needs;
        keyframe=needs&&!(keyframe_ack_pending&&keyframe_ack_version==keyframe_version);
    }
    void run(std::function<std::unique_ptr<DecoderOwner>()> factory)noexcept{
        std::unique_ptr<DecoderOwner> decoder;
        try{
            decoder=factory();snapshot(*decoder);
            {std::lock_guard<std::mutex> lock(mutex);initialized=true;}notify();
            for(;;){
                std::optional<Command> next_command_value;std::optional<Packet> packet;bool poll=false,ack=false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    const auto available=[&]{return stopping||command||keyframe_ack_pending||!packets.empty();};
                    bool ready=false;
                    if(!available()){
                        const auto descriptors=configured?decoder->readiness_fds():std::array<int,2>{-1,-1};
                        if(wake.descriptor()>=0&&(descriptors[0]>=0||descriptors[1]>=0)){
                            lock.unlock();ready=wake.wait(descriptors);lock.lock();
                        }else if(configured&&decoder->idle_poll_needed()){
                            ready=!changed.wait_for(lock,std::chrono::milliseconds(1),available);
                        }else changed.wait(lock,available);
                    }
                    if(stopping)break;
                    if(command){next_command_value=*command;command.reset();}
                    else if(keyframe_ack_pending){
                        ack=keyframe_ack_version==keyframe_version;
                        keyframe_ack_pending=false;
                    }
                    else if(!packets.empty()){packet=std::move(packets.front());packets.pop_front();}
                    else poll=configured&&ready;
                }
                if(next_command_value){
                    const auto value=*next_command_value;
                    if(value.kind==CommandKind::Configure)decoder->configure(value.codec,value.width,value.height,true);
                    else decoder->deactivate();
                    snapshot(*decoder);
                    {std::lock_guard<std::mutex> lock(mutex);
                        configured=value.kind==CommandKind::Configure;
                        completed_command=value.id;}
                    notify();
                }else if(ack){decoder->keyframe_request_queued();snapshot(*decoder);}
                else if(packet){
                    decoder->submit(packet->bytes.data()+packet->offset,packet->bytes.size()-packet->offset,packet->timestamp,std::move(packet->metadata),packet->submitted_ns,packet->output_epoch);
                    snapshot(*decoder);
                    {std::lock_guard<std::mutex> lock(mutex);--outstanding_packets;outstanding_bytes-=packet->bytes.size();}
                    notify();
                }else if(poll){decoder->poll();snapshot(*decoder);}
            }
            decoder->deactivate();snapshot(*decoder);
        }catch(...){
            const auto error=std::current_exception();
            // Preserve decoder diagnostics before publishing the original
            // exception. No consumer ever reads the live decoder itself.
            if(decoder){try{snapshot(*decoder);}catch(...){}}
            {std::lock_guard<std::mutex> lock(mutex);failure=error;stats.failed=true;discard_locked();}
            notify();
        }
        // Hardware/MMAP and allocator references die on their owning worker
        // before stop() joins and the caller can destroy its captured objects.
        decoder.reset();
    }
    DecoderStats statistics()const{
        std::lock_guard<std::mutex> lock(mutex);auto result=stats;
        result.async_queue_packets_max=peak_packets;result.async_queue_bytes_max=peak_bytes;
        result.async_backpressure_wait_ns=backpressure_ns;result.async_discarded_packets=discarded_packets;return result;
    }
};
} // namespace surfxr
