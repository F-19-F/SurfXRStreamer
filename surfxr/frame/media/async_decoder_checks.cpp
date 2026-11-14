#include "async_decoder_private.hpp"

namespace surfxr {
namespace {
void test_require(bool value,const char* stage){if(!value)throw std::runtime_error(stage);}
struct AsyncFixtureState {
    std::mutex mutex;std::condition_variable changed;
    bool hold=false,throw_sink=false,request_keyframe=false;
    unsigned entered=0,configured=0,deactivated=0,acknowledged=0,published=0;
    unsigned polls=0;
    std::shared_ptr<decoder_detail::DecoderWake> readiness;
    bool all_restarts=true;
    std::vector<std::uint64_t> inputs,original_times;
    std::vector<const std::uint8_t*> input_addresses;
    std::vector<std::optional<std::uint64_t>> input_epochs;
    std::shared_ptr<const VideoFrame> last;
    void release(){std::lock_guard<std::mutex> lock(mutex);hold=false;changed.notify_all();}
    template<class F>void wait(F predicate){
        std::unique_lock<std::mutex> lock(mutex);
        if(!changed.wait_for(lock,std::chrono::seconds(2),predicate)){
            hold=false;changed.notify_all();throw std::runtime_error("async_fixture_timed_out");
        }
    }
};
struct ReleaseFixture {
    std::shared_ptr<AsyncFixtureState> state;
    ~ReleaseFixture(){state->release();}
};
class FixtureDecoder final:public DecoderOwner {
public:
    explicit FixtureDecoder(std::shared_ptr<AsyncFixtureState> state):state_(std::move(state)),owner_(std::this_thread::get_id()){}
    void configure(std::uint32_t,std::uint32_t,std::uint32_t,bool restart)override{
        touch();pending_.clear();keyframe_=false;++stats_.configuration_changes;
        std::lock_guard<std::mutex> lock(state_->mutex);++state_->configured;state_->all_restarts&=restart;state_->changed.notify_all();
    }
    void deactivate()override{
        touch();pending_.clear();keyframe_=false;
        std::lock_guard<std::mutex> lock(state_->mutex);++state_->deactivated;state_->changed.notify_all();
    }
    void submit(const std::uint8_t* bytes,std::size_t size,std::uint64_t timestamp,
                std::shared_ptr<const VrVideoMetadata> metadata,std::uint64_t original,std::optional<std::uint64_t> epoch)override{
        touch();
        {std::unique_lock<std::mutex> lock(state_->mutex);++state_->entered;state_->changed.notify_all();
            state_->changed.wait(lock,[&]{return !state_->hold;});
            state_->inputs.push_back(timestamp);state_->original_times.push_back(original);state_->input_epochs.push_back(epoch);state_->input_addresses.push_back(bytes);keyframe_=state_->request_keyframe;}
        auto frame=std::make_shared<VideoFrame>();frame->source_timestamp=timestamp;frame->has_source_timestamp=true;
        frame->packet_submit_monotonic_ns=original;frame->vr_metadata=std::move(metadata);
        frame->rgba.assign(bytes,bytes+size);pending_.push_back(std::move(frame));++stats_.packets;
        if(state_->readiness)state_->readiness->notify();
        state_->changed.notify_all();
    }
    std::size_t poll()override{
        touch();if(state_->readiness)state_->readiness->consume();
        {std::lock_guard<std::mutex> lock(state_->mutex);++state_->polls;}
        if(pending_.empty())return 0;
        auto frame=std::move(pending_.front());pending_.pop_front();frame->generation=++stats_.frames;
        frame->conversion_begin_monotonic_ns=frame->decoded_monotonic_ns=steady_ns();
        frame->decoder_output_monotonic_ns=frame->decoded_monotonic_ns;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if(state_->throw_sink)throw std::runtime_error("async_fixture_sink_failure");
        if(state_->readiness&&!pending_.empty())state_->readiness->notify();
        state_->last=std::move(frame);++state_->published;state_->changed.notify_all();return 1;
    }
    std::array<int,2> readiness_fds()const override{touch();return {state_->readiness?state_->readiness->descriptor():-1,-1};}
    bool idle_poll_needed()const override{touch();return !pending_.empty();}
    DecoderStats statistics()const override{touch();return stats_;}
    bool needs_keyframe()const override{touch();return keyframe_;}
    void keyframe_request_queued()override{
        touch();keyframe_=false;
        std::lock_guard<std::mutex> lock(state_->mutex);++state_->acknowledged;state_->changed.notify_all();
    }
private:
    void touch()const{test_require(std::this_thread::get_id()==owner_,"async_decoder_accessed_from_non_owner");}
    std::shared_ptr<AsyncFixtureState> state_;std::thread::id owner_;
    DecoderStats stats_;bool keyframe_=false;
    std::deque<std::shared_ptr<VideoFrame>> pending_;
};
}
int async_video_decoder_self_test(){
    const auto create=[](const std::shared_ptr<AsyncFixtureState>& state,AsyncVideoDecoder::FrameDemand demand={},std::size_t packet_limit=kMaxPackets){
        return std::unique_ptr<AsyncVideoDecoder>(new AsyncVideoDecoder(std::make_unique<AsyncVideoDecoder::Impl>([state]{return std::make_unique<FixtureDecoder>(state);},std::move(demand),packet_limit)));
    };
    // Original input bytes and metadata outlive the receiver buffer. One input
    // progresses through idle polling, without another packet or a flush.
    for(const bool event_driven:{false,true}){
        auto state=std::make_shared<AsyncFixtureState>();state->hold=true;
        if(event_driven)state->readiness=std::make_shared<decoder_detail::DecoderWake>();
        auto decoder=create(state);ReleaseFixture release{state};decoder->configure(5,256,256);
        auto owner=std::make_shared<int>(7);
        std::shared_ptr<const VrVideoMetadata> metadata(owner,nullptr);
        std::uint8_t bytes[]{0x41,0x42};const auto before=steady_ns();
        decoder->submit(bytes,sizeof(bytes),101,metadata);const auto submitted=steady_ns();bytes[0]=0xff;
        state->wait([&]{return state->entered==1;});state->release();state->wait([&]{return state->published==1;});
        decoder->stop();decoder->stop();const auto stats=decoder->statistics();
        test_require(stats.packets==1&&stats.frames==1&&state->last->rgba==std::vector<std::uint8_t>({0x41,0x42}),"async_input_copy_or_idle_poll_failed");
        test_require(state->last->packet_submit_monotonic_ns>=before&&state->last->packet_submit_monotonic_ns<=submitted&&
                     !state->last->vr_metadata.owner_before(metadata)&&!metadata.owner_before(state->last->vr_metadata),"async_input_time_or_metadata_identity_changed");
        test_require(stats.async_queue_packets_max==1&&stats.async_queue_bytes_max==2,"async_single_packet_bound_failed");
    }
    // Owned input retains header/payload allocation and exact metadata without
    // another encoded copy. Idle waiting wakes for output and control barriers.
    {
        auto state=std::make_shared<AsyncFixtureState>();state->readiness=std::make_shared<decoder_detail::DecoderWake>();
        state->hold=true;std::atomic<std::uint64_t> epoch{7};auto decoder=create(state,[&]{return epoch.load();});
        ReleaseFixture release{state};decoder->configure(5,256,256);
        std::vector<std::uint8_t> bytes{0xaa,0xbb,0x41,0x42};const auto* address=bytes.data()+2;
        auto owner=std::make_shared<int>(9);std::shared_ptr<const VrVideoMetadata> metadata(owner,nullptr);
        decoder->submit(std::move(bytes),2,202,metadata);state->wait([&]{return state->entered==1;});
        epoch=9;state->release();state->wait([&]{return state->published==1;});
        unsigned idle_polls;{std::lock_guard<std::mutex> lock(state->mutex);idle_polls=state->polls;}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        {std::lock_guard<std::mutex> lock(state->mutex);test_require(state->polls==idle_polls,"async_empty_decoder_kept_polling");}
        decoder->deactivate();decoder->stop();
        test_require(state->input_addresses==std::vector<const std::uint8_t*>({address})&&
                     state->last->rgba==std::vector<std::uint8_t>({0x41,0x42})&&
                     state->input_epochs==std::vector<std::optional<std::uint64_t>>({7})&&
                     !state->last->vr_metadata.owner_before(metadata)&&!metadata.owner_before(state->last->vr_metadata)&&
                     decoder->statistics().async_queue_bytes_max==4,"async_owned_input_or_metadata_was_copied_or_relabelled");
    }
    // Demand belongs to the original receiver callback, even if pause/resume
    // occurs while this packet is queued behind an in-flight hardware call.
    {
        auto state=std::make_shared<AsyncFixtureState>();state->hold=true;
        std::atomic<std::uint64_t> epoch{1};auto decoder=create(state,[&]{return epoch.load();});
        ReleaseFixture release{state};decoder->configure(5,256,256);
        const std::uint8_t byte=1;decoder->submit(&byte,1,1);state->wait([&]{return state->entered==1;});
        epoch=0;decoder->submit(&byte,1,2);epoch=3;decoder->submit(&byte,1,3);
        epoch=4;state->release();state->wait([&]{return state->published==3;});decoder->stop();
        test_require(state->input_epochs==std::vector<std::optional<std::uint64_t>>({1,0,3})&&
                     decoder->statistics().packets==3&&decoder->statistics().async_discarded_packets==0,
                     "async_demand_epoch_was_relabelled_or_encoded_packet_dropped");
    }
    // Total packet and byte budgets include the item currently executing.
    // Backpressure releases the mutex, so the worker can make room; no encoded
    // dependency packet is dropped, duplicated or reordered.
    for(const auto packet_limit:{kMaxPackets,kMaxPackedPackets})for(const bool byte_budget:{false,true}){
        auto state=std::make_shared<AsyncFixtureState>();state->hold=true;
        auto decoder=create(state,{},packet_limit);ReleaseFixture release{state};decoder->configure(5,256,256);
        std::vector<std::uint8_t> bytes(byte_budget?kMaxBytes:1,0x5a);
        decoder->submit(bytes.data(),bytes.size(),1);state->wait([&]{return state->entered==1;});
        const unsigned accepted=byte_budget?1:static_cast<unsigned>(packet_limit);
        for(unsigned i=2;i<=accepted;++i)decoder->submit(bytes.data(),bytes.size(),i);
        test_require(!decoder->try_submit(bytes.data(),bytes.size(),999)&&decoder->statistics().async_backpressure_wait_ns==0,
                     "async_try_submit_waited_or_exceeded_bound");
        std::vector<std::uint8_t> rejected_owner{0xaa,0xbb};const auto* rejected_address=rejected_owner.data();
        test_require(!decoder->try_submit(std::move(rejected_owner),1,998)&&rejected_owner.size()==2&&rejected_owner.data()==rejected_address,
                     "async_full_queue_consumed_rejected_owner");
        std::promise<void> entered;auto started=entered.get_future();
        auto blocked=std::async(std::launch::async,[&]{entered.set_value();decoder->submit(bytes.data(),bytes.size(),accepted+1);});
        started.wait();const bool waited=blocked.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
        state->release();blocked.get();state->wait([&]{return state->inputs.size()==accepted+1;});
        state->wait([&]{return state->published==accepted+1;});decoder->stop();
        const auto stats=decoder->statistics();test_require(waited&&stats.async_backpressure_wait_ns>0,"async_backpressure_did_not_wait");
        test_require(stats.async_queue_packets_max<=packet_limit&&stats.async_queue_bytes_max<=kMaxBytes&&stats.async_discarded_packets==2,"async_queue_exceeded_bound_or_dropped_accepted_packets");
        for(unsigned i=0;i<=accepted;++i)test_require(state->inputs[i]==i+1,"async_packet_order_changed");
    }
    // Configure and deactivate are barriers: an in-flight callback/packet is
    // retired, queued old references are discarded, and same-size configure
    // restarts the underlying reference chain.
    for(const bool configure:{true,false}){
        auto state=std::make_shared<AsyncFixtureState>();state->hold=true;
        auto decoder=create(state);ReleaseFixture release{state};decoder->configure(5,256,256);
        const std::uint8_t byte=1;decoder->submit(&byte,1,1);state->wait([&]{return state->entered==1;});decoder->submit(&byte,1,2);
        auto barrier=std::async(std::launch::async,[&]{if(configure)decoder->configure(5,256,256);else decoder->deactivate();});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(decoder->statistics().async_discarded_packets!=1&&std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool waited=barrier.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
        state->release();barrier.get();decoder->stop();
        test_require(waited&&state->inputs==std::vector<std::uint64_t>({1})&&state->all_restarts,"async_barrier_failed_to_retire_or_discard");
        test_require(decoder->statistics().async_discarded_packets==1,"async_reset_discard_counter_failed");
    }
    // stop wakes a producer blocked on queue space but does not return while
    // the current owner call is still executing. All queued packets are dropped.
    {
        auto state=std::make_shared<AsyncFixtureState>();state->hold=true;
        auto decoder=create(state);ReleaseFixture release{state};decoder->configure(5,256,256);
        const std::uint8_t byte=1;decoder->submit(&byte,1,1);state->wait([&]{return state->entered==1;});
        for(unsigned i=2;i<=4;++i)decoder->submit(&byte,1,i);
        std::promise<void> entered;auto started=entered.get_future();
        auto blocked=std::async(std::launch::async,[&]{entered.set_value();try{decoder->submit(&byte,1,5);}catch(const VideoDecodeError& error){return std::string(error.what())=="async_decoder_stopped";}return false;});
        started.wait();auto stopping=std::async(std::launch::async,[&]{decoder->stop();});
        const bool waited=stopping.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
        state->release();stopping.get();const bool rejected=blocked.get();
        test_require(waited&&rejected&&state->inputs==std::vector<std::uint64_t>({1})&&decoder->statistics().async_discarded_packets==3,"async_stop_failed_to_join_or_wake_producer");
    }
    // Callback exceptions preserve their original type/message and are visible
    // to check and subsequent producer operations; stats remain readable.
    {
        auto state=std::make_shared<AsyncFixtureState>();state->throw_sink=true;
        auto decoder=create(state);ReleaseFixture release{state};decoder->configure(5,256,256);
        const std::uint8_t byte=1;decoder->submit(&byte,1,1);
        bool checked=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!checked&&std::chrono::steady_clock::now()<deadline){
            try{decoder->check();}catch(const std::runtime_error& error){checked=std::string(error.what())=="async_fixture_sink_failure";}
            if(!checked)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool submitted=false;try{decoder->submit(&byte,1,2);}catch(const std::runtime_error& error){submitted=std::string(error.what())=="async_fixture_sink_failure";}
        decoder->stop();test_require(checked&&submitted&&decoder->statistics().failed,"async_sink_failure_not_propagated");
    }
    // A keyframe ACK is enqueue-only even while the worker is busy. It is
    // coalesced and processed before the next queued input.
    {
        auto state=std::make_shared<AsyncFixtureState>();state->request_keyframe=true;
        auto decoder=create(state);ReleaseFixture release{state};decoder->configure(5,256,256);
        const std::uint8_t byte=1;decoder->submit(&byte,1,1);state->wait([&]{return state->published==1;});
        test_require(decoder->needs_keyframe(),"async_keyframe_request_missing");
        {std::lock_guard<std::mutex> lock(state->mutex);state->hold=true;}
        decoder->submit(&byte,1,2);state->wait([&]{return state->entered==2;});
        auto ack=std::async(std::launch::async,[&]{decoder->keyframe_request_queued();decoder->keyframe_request_queued();});
        const bool nonblocking=ack.wait_for(std::chrono::milliseconds(20))==std::future_status::ready;
        state->release();ack.get();state->wait([&]{return state->acknowledged==1;});decoder->stop();
        test_require(nonblocking&&!decoder->statistics().failed,"async_keyframe_ack_blocked_receiver");
    }
    // A late acknowledgement of an older recovery request cannot clear a
    // newer sequence request that appeared while the receiver queued control.
    {
        auto state=std::make_shared<AsyncFixtureState>();state->request_keyframe=true;
        auto decoder=create(state);ReleaseFixture release{state};decoder->configure(5,256,256);
        const std::uint8_t byte=1;decoder->submit(&byte,1,1);state->wait([&]{return state->published==1;});
        test_require(decoder->needs_keyframe(),"async_old_keyframe_request_missing");
        {std::lock_guard<std::mutex> lock(state->mutex);state->request_keyframe=false;}
        decoder->submit(&byte,1,2);state->wait([&]{return state->published==2;});
        {std::lock_guard<std::mutex> lock(state->mutex);state->request_keyframe=true;}
        decoder->submit(&byte,1,3);state->wait([&]{return state->published==3;});
        decoder->keyframe_request_queued();
        test_require(decoder->needs_keyframe(),"async_stale_ack_cleared_new_request");
        decoder->keyframe_request_queued();state->wait([&]{return state->acknowledged==1;});decoder->stop();
    }
    return 0;
}
} // namespace surfxr
