#include "surfxr/microphone_capture.hpp"
#include <pulse/pulseaudio.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <string>

namespace surfxr {
bool microphone_source_allowed(bool monitor,const char* media,const char* device) noexcept {
    if(monitor || (device && std::strcmp(device,"monitor")==0))return false;
    return !media || !*media || std::strcmp(media,"Audio/Source")==0;
}
MicrophonePcmRing::MicrophonePcmRing(std::uint32_t frames):capacity_(std::size_t{frames}*2){
    if(!frames || frames>4800)throw std::invalid_argument("microphone_ring_capacity_out_of_range");
}
bool MicrophonePcmRing::submit(const std::uint8_t* input,std::size_t count) noexcept {
    if(stopped_.load(std::memory_order_relaxed))return false;
    if((!input && count) || count>data_.size()){++invalid_;return false;}
    const auto old=partial_;
    const auto old_size=partial_size_;
    const auto total=old_size+count,complete=total-total%2;
    const auto byte=[&](std::size_t i){return i<old_size?old:input[i-old_size];};
    partial_size_=total%2;if(partial_size_)partial_=byte(complete);
    if(!complete)return true;
    std::uint64_t nonzero=0,squares=0;std::uint32_t peak=0;
    for(std::size_t i=0;i<complete;i+=2){
        const auto bits=std::uint16_t(byte(i))|(std::uint16_t(byte(i+1))<<8);
        const auto sample=static_cast<std::int16_t>(bits);
        const auto magnitude=static_cast<std::uint32_t>(sample<0?-static_cast<int>(sample):sample);
        nonzero+=magnitude!=0;squares+=std::uint64_t{magnitude}*magnitude;peak=std::max(peak,magnitude);
    }
    captured_.fetch_add(complete/2);nonzero_.fetch_add(nonzero);squares_.fetch_add(squares);
    peak_.store(std::max(peak_.load(),peak));
    std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock()){dropped_busy_.fetch_add(complete/2);return false;}
    if(stopped_.load(std::memory_order_relaxed))return false;
    auto amount=complete,offset=std::size_t{0};
    if(amount>=capacity_){dropped_old_.fetch_add((size_+amount-capacity_)/2);offset=amount-capacity_;amount=capacity_;head_=size_=0;}
    else if(size_+amount>capacity_){const auto drop=size_+amount-capacity_;head_=(head_+drop)%capacity_;size_-=drop;dropped_old_.fetch_add(drop/2);}
    for(std::size_t i=0;i<amount;++i)data_[(head_+size_+i)%capacity_]=byte(offset+i);
    size_+=amount;const auto frames=static_cast<std::uint32_t>(size_/2);
    queued_.store(frames);maximum_.store(std::max(maximum_.load(),frames));return true;
}
std::size_t MicrophonePcmRing::read(std::uint8_t* output,std::size_t bytes) noexcept {
    if(!output || !bytes || bytes%2 || bytes>capacity_ || stopped_.load())return 0;
    std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock() || stopped_.load() || size_<bytes)return 0;
    const auto first=std::min(bytes,capacity_-head_);std::memcpy(output,data_.data()+head_,first);
    if(bytes>first)std::memcpy(output+first,data_.data(),bytes-first);
    head_=(head_+bytes)%capacity_;size_-=bytes;queued_.store(static_cast<std::uint32_t>(size_/2));consumed_.fetch_add(bytes/2);return bytes;
}
void MicrophonePcmRing::stop() noexcept {stopped_.store(true);queued_.store(0);}
MicrophoneStats MicrophonePcmRing::statistics() const noexcept {
    MicrophoneStats s;s.stopped=stopped_.load();s.capture_frames=captured_.load();s.consumed_frames=consumed_.load();
    s.nonzero_samples=nonzero_.load();s.sample_square_sum=squares_.load();s.peak=peak_.load();
    s.dropped_old_frames=dropped_old_.load();s.dropped_contention_frames=dropped_busy_.load();s.invalid_chunks=invalid_.load();
    s.queued_frames=s.stopped?0:queued_.load();s.max_queued_frames=maximum_.load();return s;
}

struct MicrophoneCapture::Impl {
    MicrophonePcmRing ring;
    pa_threaded_mainloop* loop=nullptr;pa_context* context=nullptr;pa_stream* stream=nullptr;
    bool loop_started=false,attempted=false;
    std::atomic_bool stopping{false},ready{false},failed{false},validated{false};
    std::atomic_int context_state{PA_CONTEXT_UNCONNECTED},stream_state{PA_STREAM_UNCONNECTED},enumerated{0},error{0};
    std::atomic<std::uint64_t> callbacks{0},holes{0},oversized{0};
    std::atomic<std::int64_t> last_capture_ns{0};
    std::atomic<std::uint32_t> buffer_bytes{0},fragment_bytes{0};
    std::string selected_source;std::uint32_t selected_index=PA_INVALID_INDEX;int source_score=-1;
    std::mutex wait_mutex;std::condition_variable changed;
    struct Lock {pa_threaded_mainloop* loop;explicit Lock(pa_threaded_mainloop* p):loop(p){pa_threaded_mainloop_lock(loop);}~Lock(){pa_threaded_mainloop_unlock(loop);}};
    static std::int64_t clock_ns() noexcept {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    void fail() noexcept {failed.store(true);if(context)error.store(pa_context_errno(context));changed.notify_all();}
    static void context_changed(pa_context* c,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);const auto state=pa_context_get_state(c);s.context_state.store(state);
        if(!PA_CONTEXT_IS_GOOD(state) && !s.stopping.load())s.fail();
        s.changed.notify_all();
    }
    static void source_info(pa_context*,const pa_source_info* info,int end,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);
        if(end){if(end<0)s.fail();s.enumerated.store(1);s.changed.notify_all();return;}
        if(!info || !info->name || !info->proplist)return;
        const auto* media=pa_proplist_gets(info->proplist,"media.class");
        const auto* device=pa_proplist_gets(info->proplist,"device.class");
        if(!microphone_source_allowed(info->monitor_of_sink!=PA_INVALID_INDEX,media,device))return;
        // Prefer the internal hardware microphone. A virtual capture source is
        // permitted only if no real capture source exists, never a sink monitor.
        int score=(info->flags&PA_SOURCE_HARDWARE)?4:0;
        const auto* form=pa_proplist_gets(info->proplist,"device.form_factor");
        if(form && std::strcmp(form,"internal")==0)score+=2;
        if(media && std::strcmp(media,"Audio/Source")==0)++score;
        if(score>s.source_score){
            try{s.selected_source=info->name;s.selected_index=info->index;s.source_score=score;}
            catch(...){s.fail();}
        }
    }
    static void stream_changed(pa_stream* p,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);const auto state=pa_stream_get_state(p);s.stream_state.store(state);
        if(state==PA_STREAM_READY){
            if(pa_stream_get_device_index(p)!=s.selected_index){s.fail();return;}
            s.validated.store(true);
            if(const auto* a=pa_stream_get_buffer_attr(p)){s.buffer_bytes.store(a->maxlength);s.fragment_bytes.store(a->fragsize);}
        }else if(!PA_STREAM_IS_GOOD(state) && !s.stopping.load())s.fail();
        s.changed.notify_all();
    }
    static void moved(pa_stream* p,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);if(pa_stream_get_device_index(p)!=s.selected_index)s.fail();
    }
    static void read_ready(pa_stream* p,std::size_t,void* userdata) noexcept {
        auto& s=*static_cast<Impl*>(userdata);if(s.stopping.load() || s.failed.load())return;
        ++s.callbacks;
        const void* raw=nullptr;std::size_t count=0;
        if(pa_stream_peek(p,&raw,&count)<0){s.fail();return;}
        if(!count)return;
        s.last_capture_ns.store(clock_ns());
        // At most 100 ms is inspected/copied per callback. Excess queued audio
        // is discarded from the front, keeping complete, recent mono samples.
        const auto even=count-count%2,kept=std::min<std::size_t>(even,9600);
        if(even>kept)s.oversized.fetch_add((even-kept)/2);
        if(raw)s.ring.submit(static_cast<const std::uint8_t*>(raw)+even-kept,kept);
        else{
            s.holes.fetch_add(even/2);std::array<std::uint8_t,9600> silence{};s.ring.submit(silence.data(),kept);
        }
        if(count%2)s.fail();
        if(pa_stream_drop(p)<0)s.fail();
    }
    void wait_for(const std::atomic_int& state,int wanted,const char* stage,std::chrono::steady_clock::time_point deadline){
        std::unique_lock<std::mutex> lock(wait_mutex);
        while(state.load()!=wanted && !failed.load() && !stopping.load()){
            if(std::chrono::steady_clock::now()>=deadline)throw MicrophoneCaptureError(stage,error.load());
            changed.wait_for(lock,std::chrono::milliseconds(10));
        }
        if(failed.load() || stopping.load())throw MicrophoneCaptureError(stage,error.load());
    }
    void start(){
        if(attempted || stopping.load())throw MicrophoneCaptureError("microphone_already_started_or_stopped");
        attempted=true;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        try{
            loop=pa_threaded_mainloop_new();if(!loop)throw MicrophoneCaptureError("microphone_mainloop_allocation_failed");
            if(pa_threaded_mainloop_start(loop)<0)throw MicrophoneCaptureError("microphone_mainloop_start_failed");
            loop_started=true;
            {Lock lock(loop);
                context=pa_context_new(pa_threaded_mainloop_get_api(loop),"SurfXR microphone");
                if(!context)throw MicrophoneCaptureError("microphone_context_allocation_failed");
                pa_context_set_state_callback(context,context_changed,this);
                if(pa_context_connect(context,nullptr,PA_CONTEXT_NOAUTOSPAWN,nullptr)<0)throw MicrophoneCaptureError("microphone_context_connect_failed",pa_context_errno(context));
            }
            wait_for(context_state,PA_CONTEXT_READY,"microphone_context_not_ready",deadline);
            {Lock lock(loop);auto* operation=pa_context_get_source_info_list(context,source_info,this);
                if(!operation)throw MicrophoneCaptureError("microphone_source_query_failed",pa_context_errno(context));
                pa_operation_unref(operation);
            }
            wait_for(enumerated,1,"microphone_source_query_timeout",deadline);
            {Lock lock(loop);
                if(selected_index==PA_INVALID_INDEX || selected_source.empty())throw MicrophoneCaptureError("microphone_no_nonmonitor_source");
                const pa_sample_spec spec{PA_SAMPLE_S16LE,48000,1};pa_channel_map map{};pa_channel_map_init_mono(&map);
                stream=pa_stream_new(context,"Headset microphone",&spec,&map);
                if(!stream)throw MicrophoneCaptureError("microphone_stream_allocation_failed",pa_context_errno(context));
                pa_stream_set_state_callback(stream,stream_changed,this);pa_stream_set_read_callback(stream,read_ready,this);pa_stream_set_moved_callback(stream,moved,this);
                const pa_buffer_attr attributes{5760,UINT32_MAX,UINT32_MAX,UINT32_MAX,1920};
                // Explicit validated source and DONT_MOVE prevent the default
                // source changing to a playback monitor while capture is active.
                if(pa_stream_connect_record(stream,selected_source.c_str(),&attributes,PA_STREAM_DONT_MOVE)<0)
                    throw MicrophoneCaptureError("microphone_stream_connect_failed",pa_context_errno(context));
            }
            wait_for(stream_state,PA_STREAM_READY,"microphone_stream_not_ready",deadline);last_capture_ns.store(clock_ns());ready.store(true);
        }catch(const MicrophoneCaptureError& e){failed.store(true);error.store(e.pulse_code);stop();throw;}
        catch(...){failed.store(true);stop();throw MicrophoneCaptureError("microphone_start_failed");}
    }
    void stop() noexcept {
        stopping.store(true);ring.stop();changed.notify_all();
        if(loop){
            {Lock lock(loop);
                if(stream){pa_stream_set_read_callback(stream,nullptr,nullptr);pa_stream_set_state_callback(stream,nullptr,nullptr);pa_stream_set_moved_callback(stream,nullptr,nullptr);pa_stream_disconnect(stream);pa_stream_unref(stream);stream=nullptr;}
                if(context){pa_context_set_state_callback(context,nullptr,nullptr);pa_context_disconnect(context);pa_context_unref(context);context=nullptr;}
            }
            if(loop_started)pa_threaded_mainloop_stop(loop);
            pa_threaded_mainloop_free(loop);loop=nullptr;loop_started=false;
        }
    }
    ~Impl(){stop();}
};
MicrophoneCapture::MicrophoneCapture():impl_(std::make_unique<Impl>()){}
MicrophoneCapture::~MicrophoneCapture()=default;
void MicrophoneCapture::start(){impl_->start();}
void MicrophoneCapture::stop() noexcept {impl_->stop();}
std::size_t MicrophoneCapture::read_pcm(std::uint8_t* output,std::size_t capacity){
    if(impl_->failed.load())throw MicrophoneCaptureError("microphone_capture_failed",impl_->error.load());
    if(!output || capacity<kMicrophonePacketBytes)throw MicrophoneCaptureError("microphone_output_too_small");
    if(!impl_->ready.load() || impl_->stopping.load())return 0;
    if(Impl::clock_ns()-impl_->last_capture_ns.load()>2'000'000'000){
        impl_->failed.store(true);throw MicrophoneCaptureError("microphone_capture_stalled");
    }
    return impl_->ring.read(output,kMicrophonePacketBytes);
}
MicrophoneStats MicrophoneCapture::statistics() const noexcept {
    auto s=impl_->ring.statistics();s.initialized=impl_->ready.load();s.failed=impl_->failed.load();s.source_validated=impl_->validated.load();
    s.pulse_error=impl_->error.load();s.read_callbacks=impl_->callbacks.load();s.hole_frames=impl_->holes.load();s.dropped_old_frames+=impl_->oversized.load();
    s.server_buffer_bytes=impl_->buffer_bytes.load();s.server_fragment_bytes=impl_->fragment_bytes.load();return s;
}
} // namespace surfxr
