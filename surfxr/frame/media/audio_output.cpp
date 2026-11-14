// API lifecycle follows the installed PulseAudio 17 public headers and:
// https://www.freedesktop.org/software/pulseaudio/doxygen/threaded_mainloop.html
// https://www.freedesktop.org/software/pulseaudio/doxygen/stream_8h.html
#include "surfxr/audio_output.hpp"
#include <pulse/pulseaudio.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>

namespace surfxr {
namespace {constexpr unsigned reserve_frames=576,fade_frames=96;} // 12 ms / 2 ms at 48 kHz.
AudioPcmRing::AudioPcmRing(std::uint32_t frames,AudioPlaybackMode mode):capacity_(std::size_t{frames}*4),buffered_(mode==AudioPlaybackMode::Buffered) {
    if(!frames || frames>4800)throw std::invalid_argument("audio_ring_capacity_out_of_range");
}
bool AudioPcmRing::submit(const std::uint8_t* bytes,std::size_t count) noexcept {
    return submit_in_epoch(bytes,count,reset_epoch_.load(std::memory_order_acquire));
}
bool AudioPcmRing::submit_in_epoch(const std::uint8_t* bytes,std::size_t count,std::uint64_t epoch) noexcept {
    if(stopped_.load(std::memory_order_relaxed)){++after_stop_;return false;}
    // More than this is a malformed transport record, not a useful PCM chunk.
    if((!bytes && count) || count>16u*1024u*1024u){++invalid_;return false;}
    if(!count)return true;
    submitted_.fetch_add(count,std::memory_order_relaxed);
    if(epoch!=reset_epoch_.load(std::memory_order_acquire)){
        dropped_old_.fetch_add(count/4,std::memory_order_relaxed);return false;
    }
    if(producer_epoch_!=epoch){partial_size_=0;producer_epoch_=epoch;}
    const auto old_partial=partial_;
    const auto old_size=partial_size_;
    const auto total=old_size+count,complete=total-total%4;
    const auto copy_logical=[&](std::uint8_t* out,std::size_t offset,std::size_t length){
        if(offset<old_size){const auto n=std::min(length,old_size-offset);std::memcpy(out,old_partial.data()+offset,n);out+=n;offset+=n;length-=n;}
        if(length)std::memcpy(out,bytes+offset-old_size,length);
    };
    partial_size_=total%4;
    if(partial_size_)copy_logical(partial_.data(),complete,partial_size_);
    partial_count_.store(static_cast<std::uint32_t>(partial_size_),std::memory_order_relaxed);
    partial_epoch_.store(epoch,std::memory_order_release);
    if(!complete)return true;
    if(buffered_)return submit_buffered(bytes,old_partial,old_size,complete,epoch);
    std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock()){dropped_busy_.fetch_add(complete/4,std::memory_order_relaxed);return false;}
    if(stopped_.load(std::memory_order_relaxed)){++after_stop_;return false;}
    if(reset_epoch_.load(std::memory_order_acquire)!=epoch){
        dropped_old_.fetch_add(complete/4,std::memory_order_relaxed);return false;
    }
    std::size_t offset=0,amount=complete;
    if(amount>=capacity_){
        dropped_old_.fetch_add((size_+amount-capacity_)/4,std::memory_order_relaxed);
        offset=amount-capacity_;amount=capacity_;head_=size_=0;
    }else if(size_+amount>capacity_){
        const auto drop=size_+amount-capacity_;
        head_=(head_+drop)%capacity_;size_-=drop;
        dropped_old_.fetch_add(drop/4,std::memory_order_relaxed);
    }
    const auto tail=(head_+size_)%capacity_,first=std::min(amount,capacity_-tail);
    copy_logical(data_.data()+tail,offset,first);
    if(amount>first)copy_logical(data_.data(),offset+first,amount-first);
    size_+=amount;
    const auto frames=static_cast<std::uint32_t>(size_/4);
    queued_.store(frames,std::memory_order_relaxed);
    maximum_.store(std::max(maximum_.load(std::memory_order_relaxed),frames),std::memory_order_relaxed);
    return true;
}
std::size_t AudioPcmRing::read(std::uint8_t* output,std::size_t bytes) noexcept {
    if(!output || bytes%4 || bytes>4800*4)return 0;
    std::memset(output,0,bytes);
    if(stopped_.load(std::memory_order_relaxed))return 0;
    if(buffered_)return read_buffered(output,bytes);
    std::size_t actual=0;bool discontinuity=false,buffering=false,acquired=false;
    {
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if(lock.owns_lock() && !stopped_.load(std::memory_order_relaxed)){
            acquired=true;
            if(buffered_){
                const auto epoch=reset_epoch_.load(std::memory_order_acquire);
                if(consumer_epoch_!=epoch){
                    consumer_epoch_=epoch;primed_=false;last_sample_={};fade_remaining_=0;
                    consumer_dropped_=dropped_old_.load(std::memory_order_relaxed);
                }
                if(!primed_&&size_>=std::min(capacity_,bytes+reserve_frames*4)){
                    primed_=true;discontinuity=true;
                }
                const auto dropped=dropped_old_.load(std::memory_order_relaxed);
                discontinuity|=dropped!=consumer_dropped_;consumer_dropped_=dropped;
                buffering=!primed_;
            }
            actual=buffering?0:std::min(size_,bytes);
            const auto first=std::min(actual,capacity_-head_);
            std::memcpy(output,data_.data()+head_,first);
            if(actual>first)std::memcpy(output+first,data_.data(),actual-first);
            head_=(head_+actual)%capacity_;size_-=actual;
            queued_.store(static_cast<std::uint32_t>(size_/4),std::memory_order_relaxed);
        }
    }
    if(buffered_){
        if(buffering)++rebuffer_callbacks_;
        if(actual<bytes&&primed_&&acquired){primed_=false;++rebuffer_events_;}
        smooth(output,bytes,actual,discontinuity);
        // A busy mutex is not an empty queue. Taper this callback and resume
        // the preserved PCM next time, without adding a new priming delay.
        if(!acquired&&primed_){fade_from_={};fade_remaining_=fade_frames;}
    }
    consumed_.fetch_add(actual/4,std::memory_order_relaxed);
    if(actual<bytes){silence_.fetch_add((bytes-actual)/4,std::memory_order_relaxed);if(!buffering)++underruns_;}
    return actual;
}
void AudioPcmRing::smooth(std::uint8_t* output,std::size_t bytes,std::size_t actual,bool discontinuity) noexcept {
    const auto load=[](const std::uint8_t* p){const auto v=std::uint32_t{p[0]}|(std::uint32_t{p[1]}<<8);return v<32768?std::int32_t(v):std::int32_t(v)-65536;};
    const auto store=[](std::uint8_t* p,std::int32_t v){const auto bits=static_cast<std::uint16_t>(v);p[0]=static_cast<std::uint8_t>(bits);p[1]=static_cast<std::uint8_t>(bits>>8);};
    if(discontinuity){fade_from_=last_sample_;fade_remaining_=fade_frames;}
    const auto frames=actual/4;
    const auto tail=actual<bytes?std::min<std::size_t>(fade_frames,frames):0;
    for(std::size_t f=0;f<frames;++f){
        bool changed=fade_remaining_>0||f>=frames-tail;
        for(unsigned c=0;c<2;++c){
            auto* p=output+f*4+c*2;auto value=load(p);
            if(fade_remaining_)value=fade_from_[c]+(value-fade_from_[c])*std::int32_t(fade_frames-fade_remaining_+1)/std::int32_t(fade_frames);
            if(tail&&f>=frames-tail)value=value*std::int32_t(frames-f-1)/std::int32_t(tail);
            store(p,value);last_sample_[c]=value;
        }
        if(fade_remaining_)--fade_remaining_;
        if(changed)++transitions_;
    }
    if(actual<bytes){
        // If no PCM remains, taper the last output sample instead of making
        // an abrupt full-amplitude jump to zero. No captured sound is replayed.
        const auto n=std::min<std::size_t>(fade_frames,(bytes-actual)/4);
        if(!actual&&(last_sample_[0]||last_sample_[1])){
            for(std::size_t f=0;f<n;++f)for(unsigned c=0;c<2;++c)
                store(output+f*4+c*2,last_sample_[c]*std::int32_t(n-f-1)/std::int32_t(n));
            transitions_.fetch_add(n,std::memory_order_relaxed);
        }
        last_sample_={};fade_remaining_=0;
    }
}
void AudioPcmRing::discard_pending() noexcept {
    if(buffered_){
        reset_epoch_.fetch_add(1,std::memory_order_acq_rel);
        dropped_old_.fetch_add(discard_buffered(buffered_written_.load(std::memory_order_acquire)),std::memory_order_relaxed);
        ++resets_;return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    dropped_old_.fetch_add(size_/4,std::memory_order_relaxed);
    head_=size_=0;queued_.store(0,std::memory_order_relaxed);
    // Partial bytes remain producer-owned. Its next submit observes this epoch
    // and forgets the fragment; statistics hide the previous epoch meanwhile.
    reset_epoch_.fetch_add(1,std::memory_order_release);++resets_;
}
void AudioPcmRing::stop() noexcept {
    stopped_.store(true,std::memory_order_relaxed);
    // Do not race the single producer's partial-byte storage. It is discarded
    // logically here; no caller may submit again after stop.
    queued_.store(0,std::memory_order_relaxed);partial_count_.store(0,std::memory_order_relaxed);
}
AudioStats AudioPcmRing::statistics() const noexcept {
    AudioStats s;s.stopped=stopped_.load();s.submitted_bytes=submitted_.load();s.pcm_frames_consumed=consumed_.load();
    s.silence_frames=silence_.load();s.dropped_old_frames=dropped_old_.load();s.dropped_contention_frames=dropped_busy_.load();
    s.underrun_callbacks=underruns_.load();s.invalid_submissions=invalid_.load();s.submissions_after_stop=after_stop_.load();
    s.queued_frames=s.stopped?0:queued_.load();s.max_queued_frames=maximum_.load();
    if(buffered_&&!s.stopped){const auto read=buffered_read_.load(std::memory_order_acquire),written=buffered_written_.load(std::memory_order_acquire);
        s.queued_frames=static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity_/4,written>read?written-read:0));}
    s.partial_bytes=s.stopped||partial_epoch_.load()!=reset_epoch_.load()?0:partial_count_.load();
    s.queue_resets=resets_.load();s.rebuffer_callbacks=rebuffer_callbacks_.load();s.rebuffer_events=rebuffer_events_.load();
    s.transition_frames=transitions_.load();s.buffering_target_frames=buffered_?reserve_frames:0;return s;
}

struct AudioOutput::Impl {
    AudioPcmRing ring;
    explicit Impl(AudioPlaybackMode mode):ring(2880,mode){}
    pa_threaded_mainloop* loop=nullptr;
    pa_context* context=nullptr;
    pa_stream* stream=nullptr;
    bool loop_started=false,attempted=false;
    std::thread worker;
    std::atomic_bool stopping{false},ready{false},initialized{false},failed{false},reconnecting{false};
    std::atomic_int context_state{PA_CONTEXT_UNCONNECTED},stream_state{PA_STREAM_UNCONNECTED},error{0};
    std::atomic<std::uint64_t> callbacks{0},written{0},underflows{0};
    std::atomic<std::uint32_t> buffer_bytes{0},target_bytes{0};
    std::atomic<std::uint64_t> attempts{0},connections{0},failures{0};
    std::mutex wait_mutex;
    std::condition_variable changed;
    struct Lock {pa_threaded_mainloop* loop;explicit Lock(pa_threaded_mainloop* p):loop(p){pa_threaded_mainloop_lock(loop);}~Lock(){pa_threaded_mainloop_unlock(loop);}};
    void fail(int code) noexcept {
        ready.store(false);if(!failed.exchange(true))++failures;
        error.store(code);changed.notify_all();
    }
    static void context_changed(pa_context* c,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);const auto state=pa_context_get_state(c);s.context_state.store(state);
        if(!PA_CONTEXT_IS_GOOD(state) && !s.stopping.load())s.fail(pa_context_errno(c));
        s.changed.notify_all();
    }
    static void stream_changed(pa_stream* p,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);const auto state=pa_stream_get_state(p);s.stream_state.store(state);
        if(state==PA_STREAM_READY){if(const auto* a=pa_stream_get_buffer_attr(p)){s.buffer_bytes.store(a->maxlength);s.target_bytes.store(a->tlength);}}
        else if(!PA_STREAM_IS_GOOD(state) && !s.stopping.load())s.fail(pa_context_errno(s.context));
        s.changed.notify_all();
    }
    static void underflow(pa_stream*,void* userdata) noexcept {++static_cast<Impl*>(userdata)->underflows;}
    static void write_ready(pa_stream* p,std::size_t requested,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);
        if(s.stopping.load() || s.failed.load())return;
        ++s.callbacks;
        // Server buffer maxlength is requested at 100 ms. Bound each callback's
        // work even if the server negotiates a larger buffer.
        std::array<std::uint8_t,4800*4> data{};
        const auto bytes=std::min(requested-requested%4,data.size());
        if(!bytes)return;
        s.ring.read(data.data(),bytes);
        // null free_cb makes libpulse copy the bytes before this stack expires.
        if(pa_stream_write(p,data.data(),bytes,nullptr,0,PA_SEEK_RELATIVE)<0){s.fail(pa_context_errno(s.context));return;}
        s.written.fetch_add(bytes/4,std::memory_order_relaxed);
    }
    void wait_for(const std::atomic_int& state,int wanted,const char* stage,
                  std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(wait_mutex);
        // Short timed waits avoid lost callback notifications without acquiring
        // wait_mutex on the PulseAudio event thread.
        while(state.load()!=wanted && !failed.load() && !stopping.load()){
            if(std::chrono::steady_clock::now()>=deadline)throw AudioOutputError(stage,PA_ERR_TIMEOUT);
            changed.wait_for(lock,std::chrono::milliseconds(10));
        }
        if(failed.load() || stopping.load())throw AudioOutputError(stage,error.load());
    }
    void connect_once() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        loop=pa_threaded_mainloop_new();if(!loop)throw AudioOutputError("audio_mainloop_allocation_failed");
        if(pa_threaded_mainloop_start(loop)<0)throw AudioOutputError("audio_mainloop_start_failed");
        loop_started=true;
        {
            Lock lock(loop);
            context=pa_context_new(pa_threaded_mainloop_get_api(loop),"SurfXR");
            if(!context)throw AudioOutputError("audio_context_allocation_failed");
            pa_context_set_state_callback(context,context_changed,this);
            if(pa_context_connect(context,nullptr,PA_CONTEXT_NOAUTOSPAWN,nullptr)<0)
                throw AudioOutputError("audio_context_connect_failed",pa_context_errno(context));
        }
        wait_for(context_state,PA_CONTEXT_READY,"audio_context_not_ready",deadline);
        {
            Lock lock(loop);
            const pa_sample_spec spec{PA_SAMPLE_S16LE,48000,2};
            pa_channel_map map{};pa_channel_map_init_stereo(&map);
            stream=pa_stream_new(context,"Desktop audio",&spec,&map);
            if(!stream)throw AudioOutputError("audio_stream_allocation_failed",pa_context_errno(context));
            pa_stream_set_state_callback(stream,stream_changed,this);
            pa_stream_set_write_callback(stream,write_ready,this);
            pa_stream_set_underflow_callback(stream,underflow,this);
            // Per-stream buffering only; no sink, volume, routing, or global
            // clock setting is selected or changed by this module.
            const pa_buffer_attr attributes{19200,7680,3840,1152,UINT32_MAX};
            if(pa_stream_connect_playback(stream,nullptr,&attributes,PA_STREAM_NOFLAGS,nullptr,nullptr)<0)
                throw AudioOutputError("audio_stream_connect_failed",pa_context_errno(context));
        }
        wait_for(stream_state,PA_STREAM_READY,"audio_stream_not_ready",deadline);
        // Serialize publication with state callbacks: a simultaneous server
        // disconnect must not be overwritten by a late ready=true store.
        Lock lock(loop);
        if(stopping.load()||failed.load()||context_state.load()!=PA_CONTEXT_READY||stream_state.load()!=PA_STREAM_READY)
            throw AudioOutputError("audio_stream_not_ready",error.load());
        ready.store(true);
    }
    void disconnect() noexcept {
        ready.store(false);
        if(loop){
            if(loop_started){
                Lock lock(loop);
                if(stream){
                    pa_stream_set_write_callback(stream,nullptr,nullptr);pa_stream_set_state_callback(stream,nullptr,nullptr);
                    pa_stream_set_underflow_callback(stream,nullptr,nullptr);
                    pa_stream_disconnect(stream);pa_stream_unref(stream);stream=nullptr;
                }
                if(context){pa_context_set_state_callback(context,nullptr,nullptr);pa_context_disconnect(context);pa_context_unref(context);context=nullptr;}
            }
            // Public contract: stop must run outside the loop lock and callback.
            if(loop_started)pa_threaded_mainloop_stop(loop);
            pa_threaded_mainloop_free(loop);loop=nullptr;loop_started=false;
        }
        context_state.store(PA_CONTEXT_UNCONNECTED);stream_state.store(PA_STREAM_UNCONNECTED);
        buffer_bytes.store(0);target_bytes.store(0);
    }
    static unsigned next_delay(unsigned delay,unsigned maximum) noexcept {
        return delay>=maximum/2?maximum:delay*2;
    }
    template<class Connect,class Disconnect>
    void run(Connect connect,Disconnect cleanup,unsigned minimum_delay=250,unsigned maximum_delay=2000) noexcept {
        unsigned delay=minimum_delay;
        while(!stopping.load()){
            reconnecting.store(true);failed.store(false);++attempts;
            try{
                connect();
                if(!stopping.load()){
                    ++connections;initialized.store(true);reconnecting.store(false);delay=minimum_delay;
                    std::unique_lock<std::mutex> lock(wait_mutex);
                    // A bounded wait also covers callback notifications racing
                    // the predicate; the Pulse thread never takes wait_mutex.
                    while(!stopping.load()&&!failed.load())changed.wait_for(lock,std::chrono::milliseconds(100));
                }
            }catch(const AudioOutputError& e){if(!stopping.load()&&!failed.load())fail(e.pulse_code);}
            catch(...){if(!stopping.load()&&!failed.load())fail(PA_ERR_INTERNAL);}
            ready.store(false);cleanup();ring.discard_pending();
            if(stopping.load())break;
            reconnecting.store(true);
            std::unique_lock<std::mutex> lock(wait_mutex);
            changed.wait_for(lock,std::chrono::milliseconds(delay),[&]{return stopping.load();});
            delay=next_delay(delay,maximum_delay);
        }
        ready.store(false);reconnecting.store(false);
    }
    void initialize() {
        if(attempted || stopping.load())throw AudioOutputError("audio_already_initialized_or_stopped");
        attempted=true;reconnecting.store(true);
        try{worker=std::thread([this]{run([this]{connect_once();},[this]{disconnect();});});}
        catch(...){reconnecting.store(false);throw;}
    }
    void stop() noexcept {
        // Publish under the wait mutex so stop cannot race between the retry
        // predicate and its wait, losing the wakeup for a full backoff period.
        {std::lock_guard<std::mutex> lock(wait_mutex);stopping.store(true);}
        ready.store(false);ring.stop();changed.notify_all();
        if(worker.joinable())worker.join();
    }
    ~Impl(){stop();}
};
AudioOutput::AudioOutput(AudioPlaybackMode mode):impl_(std::make_unique<Impl>(mode)){}
AudioOutput::~AudioOutput()=default;
void AudioOutput::initialize(){impl_->initialize();}
bool AudioOutput::submit_pcm(const std::uint8_t* data,std::size_t size) noexcept {
    const auto epoch=impl_->ring.reset_epoch_.load(std::memory_order_acquire);
    if(!impl_->ready.load() || impl_->failed.load() || impl_->stopping.load())return false;
    return impl_->ring.submit_in_epoch(data,size,epoch);
}
void AudioOutput::stop() noexcept {impl_->stop();}
AudioStats AudioOutput::statistics() const noexcept {
    auto s=impl_->ring.statistics();s.initialized=impl_->initialized.load();s.ready=impl_->ready.load();
    s.failed=impl_->failed.load();s.reconnecting=impl_->reconnecting.load();s.pulse_error=impl_->error.load();
    s.context_state=impl_->context_state.load();s.stream_state=impl_->stream_state.load();
    s.connection_attempts=impl_->attempts.load();s.successful_connections=impl_->connections.load();
    s.reconnects=s.successful_connections?s.successful_connections-1:0;s.connection_failures=impl_->failures.load();
    s.write_callbacks=impl_->callbacks.load();s.written_frames=impl_->written.load();s.server_underflows=impl_->underflows.load();
    s.server_buffer_bytes=impl_->buffer_bytes.load();s.server_target_bytes=impl_->target_bytes.load();return s;
}
unsigned audio_output_recovery_self_test(){
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    const auto await=[&](const auto& predicate,const char* message){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!predicate()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(predicate(),message);
    };
    struct StopOnExit {AudioOutput& output;~StopOnExit(){output.stop();}};
    unsigned groups=0;
    {
        AudioPcmRing ring(2880,AudioPlaybackMode::Buffered);
        std::array<std::uint8_t,864*4> pcm{};pcm.fill(0x20);
        std::array<std::uint8_t,288*4> output{};
        ring.submit(pcm.data(),pcm.size());ring.read(output.data(),output.size());
        std::unique_lock<std::mutex> held(ring.mutex_);std::atomic_bool done{false};std::size_t actual=1;
        std::thread reader([&]{actual=ring.read(output.data(),output.size());done.store(true);});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!done.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool nonblocking=done.load();held.unlock();reader.join();
        require(nonblocking&&actual==output.size()&&ring.statistics().queued_frames==288,"audio_buffered_read_depends_on_mutex");
        require(ring.submit(pcm.data(),pcm.size())&&ring.statistics().dropped_contention_frames==0,"audio_buffered_submit_loses_pcm_on_mutex");
        require(ring.read(output.data(),output.size())==output.size()&&ring.statistics().rebuffer_events==0,
                "audio_contention_added_rebuffer_delay");
        ++groups;
    }
    {
        AudioPcmRing ring(8);const std::array<std::uint8_t,7> old_pcm{1,2,3,4,5,6,7};
        const auto old_epoch=ring.reset_epoch_.load();
        require(ring.submit(old_pcm.data(),old_pcm.size()),"audio_reset_fixture_not_queued");
        ring.discard_pending();
        require(!ring.submit_in_epoch(old_pcm.data(),4,old_epoch),"audio_reset_admitted_old_connection_submission");
        const std::array<std::uint8_t,4> fresh_pcm{9,10,11,12};std::array<std::uint8_t,4> read{};
        require(ring.statistics().queued_frames==0&&ring.statistics().partial_bytes==0&&
                ring.submit(fresh_pcm.data(),fresh_pcm.size())&&ring.read(read.data(),read.size())==read.size()&&read==fresh_pcm,
                "audio_reset_replayed_queue_or_partial_bytes");
        ++groups;
    }
    {
        unsigned delay=250;
        for(const unsigned expected:{500u,1000u,2000u,2000u}){
            delay=AudioOutput::Impl::next_delay(delay,2000);require(delay==expected,"audio_retry_delay_not_bounded");
        }
        ++groups;
    }
    {
        AudioOutput output;auto& s=*output.impl_;
        std::atomic<unsigned> attempts{0},cleanups{0};std::atomic_bool clean_start{true},owner_consistent{true};
        std::thread::id owner;
        StopOnExit stop{output};
        s.worker=std::thread([&]{
            owner=std::this_thread::get_id();
            s.run([&]{
                if(std::this_thread::get_id()!=owner)owner_consistent=false;
                const auto number=++attempts;
                if(number==1)throw AudioOutputError("synthetic_initial_failure",PA_ERR_CONNECTIONREFUSED);
                if(s.ring.statistics().queued_frames||s.ring.statistics().partial_bytes)clean_start=false;
                s.ready.store(true);
            },[&]{if(std::this_thread::get_id()!=owner)owner_consistent=false;++cleanups;},1,4);
        });
        await([&]{return s.connections.load()==1&&s.ready.load()&&s.initialized.load()&&!s.reconnecting.load();},"audio_initial_failure_never_recovered");
        const std::array<std::uint8_t,7> old_pcm{1,2,3,4,5,6,7};
        require(output.submit_pcm(old_pcm.data(),old_pcm.size()),"audio_recovery_fixture_not_queued");
        s.fail(PA_ERR_CONNECTIONTERMINATED);
        await([&]{return s.connections.load()==2&&s.ready.load()&&s.initialized.load()&&!s.reconnecting.load();},"audio_lost_connection_never_recovered");
        const std::array<std::uint8_t,4> fresh_pcm{9,10,11,12};std::array<std::uint8_t,4> read{};
        require(output.submit_pcm(fresh_pcm.data(),fresh_pcm.size())&&s.ring.read(read.data(),read.size())==read.size()&&read==fresh_pcm,
                "audio_recovery_replayed_old_pcm");
        // A second service loss must recover too; success does not consume a
        // one-shot retry token or leave the old failed state latched.
        s.fail(PA_ERR_CONNECTIONTERMINATED);
        await([&]{return s.connections.load()==3&&s.ready.load()&&s.initialized.load()&&!s.reconnecting.load();},
              "audio_repeated_connection_loss_never_recovered");
        const auto stats=output.statistics();
        require(stats.initialized&&stats.ready&&!stats.failed&&!stats.reconnecting&&stats.connection_attempts==4&&
                stats.successful_connections==3&&stats.reconnects==2&&stats.connection_failures==3&&stats.queue_resets==3&&
                stats.pulse_error==PA_ERR_CONNECTIONTERMINATED&&stats.dropped_old_frames==1&&clean_start&&owner_consistent,
                "audio_recovery_state_or_counters_wrong");
        output.stop();output.stop();
        require(cleanups==4&&output.statistics().stopped&&!output.statistics().ready&&!output.statistics().reconnecting,
                "audio_recovery_stop_did_not_join");
        ++groups;
    }
    {
        AudioOutput output;auto& s=*output.impl_;std::atomic_bool cleaned{false};
        StopOnExit stop{output};
        s.worker=std::thread([&]{s.run([]{throw AudioOutputError("synthetic_refused",PA_ERR_CONNECTIONREFUSED);},
                                    [&]{cleaned=true;},2000,2000);});
        await([&]{return cleaned.load();},"audio_failed_attempt_not_cleaned");
        const auto started=std::chrono::steady_clock::now();output.stop();
        require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(500)&&s.attempts.load()==1,
                "audio_stop_waited_for_retry_backoff");
        ++groups;
    }
    {
        AudioOutput output;auto& s=*output.impl_;std::atomic_bool connecting{false};std::atomic<unsigned> cleanups{0};
        StopOnExit stop{output};
        s.worker=std::thread([&]{s.run([&]{connecting=true;s.wait_for(s.context_state,PA_CONTEXT_READY,"synthetic_handshake",std::chrono::steady_clock::now()+std::chrono::seconds(3));},
                                    [&]{++cleanups;});});
        await([&]{return connecting.load();},"audio_handshake_fixture_not_started");
        const auto started=std::chrono::steady_clock::now();output.stop();
        require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(500)&&cleanups==1&&s.connections==0,
                "audio_stop_waited_for_handshake_deadline");
        ++groups;
    }
    return groups;
}
} // namespace surfxr
